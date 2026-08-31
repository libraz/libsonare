"""Tests for the parts of the fitter that decide things without rendering.

Nothing here builds or renders: these cover the knob-range rules (`knobs`), the
loss normalisation (`loss`), the stage classification (`staging`), the
write-back path translation (`writeback`) and the probe resolution (`autofit`) —
the logic a fit's correctness rests on and that a rendering test would only
exercise incidentally, slowly, and through several other layers.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit.py -q
"""

from __future__ import annotations

import argparse
import functools
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit  # noqa: E402
import build_lib  # noqa: E402
import loss as loss_module  # noqa: E402
import report as report_module  # noqa: E402
import voicematch  # noqa: E402
from _repo import REPO_ROOT  # noqa: E402
from autofit import Evaluator, check_holdout_oracle, resolve_probe, validate  # noqa: E402
from build_lib import configure_build  # noqa: E402
from catalogue import Catalogue  # noqa: E402
from knobs import (  # noqa: E402
    Knob,
    _auto_range,
    auto_spec,
    format_value,
    tunable_overrides,
)
from loss import (  # noqa: E402
    LOSS_TERMS,
    LossWeights,
    cli_weights,
    loss_terms,
    percussion_terms,
    probe_rows,
    score_terms,
)
import json  # noqa: E402
import profile as profile_module  # noqa: E402
from capture import RIG_BAKED, RIG_NONE, RIG_UNCLASSIFIED  # noqa: E402
from corpus import corpus_oracle, corpus_pattern, load_corpus  # noqa: E402
from knobs import at_bound, load_spec, load_spec_weights  # noqa: E402
from loss import (  # noqa: E402
    _refine_grid,
    _refine_partial,
    _refine_partial_direct,
    skeleton_note,
)
import metrics as metrics_module  # noqa: E402
from metrics import (  # noqa: E402
    analyze_note,
    attack_bands,
    attack_low_bands,
    level_of,
    note_onset,
)
from optimizers import cma_es, optimize  # noqa: E402
from smf import Note  # noqa: E402
from wavio import write_wav  # noqa: E402
from patterns import (  # noqa: E402
    PATTERN_BUILDERS,
    analysis_window_end,
    build_pattern,
    pattern_length,
)
from render_model import DEFAULT_DYLIB, check_gm_fallback  # noqa: E402
from render_oracle import oracle_may_carry_room  # noqa: E402
from report import report_result  # noqa: E402
from room import DRY  # noqa: E402
from staging import screen_knobs, stage_of, staged_indices  # noqa: E402
from writeback import (  # noqa: E402
    TUNING_LAYER_FILE,
    array_members,
    key_to_member_path,
    materialize,
    override_patch_names,
    patch_field_assignments,
    restore,
    write_drum_fields,
    write_edits,
    write_patch_fields,
)


# --------------------------------------------------------------------------- #
# Search ranges from the library's own clamp bounds
# --------------------------------------------------------------------------- #
def test_unit_bound_is_searched_end_to_end():
    """A normalized field's whole interval is the range, zero and one included."""
    assert _auto_range("violin.bowed_string.bow_force", 0.55, (0.0, 1.0)) == (0.0, 1.0, False)


def test_small_physical_bound_is_searched_end_to_end():
    """A bow position at 0.02..0.5 is small enough to search whole."""
    assert _auto_range("violin.bowed_string.bow_position", 0.12, (0.02, 0.5)) == (
        0.02, 0.5, False
    )


def test_wide_bound_becomes_a_log_window_around_the_default():
    """An envelope time's clamp spans four decades; the default anchors the window."""
    lo, hi, log = _auto_range("violin.amp_env.release_ms", 110.0, (1.0, 20000.0))
    assert log is True
    assert lo == pytest.approx(110.0 / 8.0)
    assert hi == pytest.approx(110.0 * 8.0)


def test_wide_window_is_capped_by_the_bound():
    """The window never leaves the interval the engine accepts."""
    lo, hi, _ = _auto_range("x.attack_ms", 4.0, (1.0, 20.0))
    assert lo == pytest.approx(1.0)
    assert hi == pytest.approx(20.0)


def test_wide_bound_with_a_zero_default_is_declined():
    """No magnitude to anchor on, and guessing one is what bounds replace."""
    assert _auto_range("x.percussion.base_freq_hz", 0.0, (0.0, 20000.0)) is None


def test_unbounded_knob_falls_back_to_the_name_heuristic():
    """An engine calibration constant has no clamp anywhere; the old rules apply."""
    lo, hi, log = _auto_range("bowed_string_voice.kSomeTime_ms", 12.0, None)
    assert (lo, hi, log) == (6.0, 24.0, True)


def test_structural_fields_are_never_auto_fitted():
    assert _auto_range("violin.glide_ms", 0.0, (0.0, 5000.0)) is None
    assert _auto_range("violin.amp_env.hold_ms", 0.0, (0.0, 5000.0)) is None


def test_catalogue_looks_a_bound_up_by_field_not_by_patch():
    """One bound serves every patch carrying the field, so the prefix is dropped."""
    cat = Catalogue({}, {}, {"bowed_string.bow_force": (0.0, 1.0)})
    assert cat.bound_for("violin.bowed_string.bow_force") == (0.0, 1.0)
    assert cat.bound_for("cello.bowed_string.bow_force") == (0.0, 1.0)
    assert cat.bound_for("cello.bowed_string.stribeck") is None


# --------------------------------------------------------------------------- #
# Loss normalisation
# --------------------------------------------------------------------------- #
def _terms(**kwargs) -> dict[str, float]:
    out = {name: 0.0 for name in LOSS_TERMS}
    out.update(kwargs)
    return out


def test_the_start_point_scores_exactly_one():
    weights = LossWeights({"harm": 1.0, "cents": 0.5, "tnr": 1.0})
    start = _terms(harm=42.0, cents=0.3, tnr=0.0)
    weights.calibrate(start)
    assert weights.combine(start) == pytest.approx(1.0)


def test_a_term_below_its_floor_cannot_swamp_the_others():
    """A noise penalty of exactly zero at the start must not divide by nothing."""
    weights = LossWeights({"harm": 1.0, "tnr": 1.0})
    weights.calibrate(_terms(harm=40.0, tnr=0.0))
    # One dB of new noise is one floor unit, not an infinity.
    assert weights.combine(_terms(harm=40.0, tnr=1.0)) < 3.0


def test_normalisation_makes_unequal_units_comparable():
    """Halving each term in turn moves the loss by the same amount."""
    weights = LossWeights({"harm": 1.0, "cents": 1.0})
    start = _terms(harm=60.0, cents=8.0)
    weights.calibrate(start)
    halved_harm = weights.combine(_terms(harm=30.0, cents=8.0))
    halved_cents = weights.combine(_terms(harm=60.0, cents=4.0))
    assert halved_harm == pytest.approx(halved_cents)


def test_raw_weighting_is_left_alone_without_calibration():
    weights = LossWeights({"harm": 1.0, "cents": 1.0})
    assert weights.combine(_terms(harm=60.0, cents=8.0)) == pytest.approx(68.0)


def test_unscorable_renders_are_infinite():
    assert LossWeights({"harm": 1.0}).combine(None) == float("inf")


def test_loss_terms_rejects_a_row_count_mismatch():
    row = {"harmonics_db": [0.0], "f0_cents_err": 0.0, "tnr_db": 0.0,
           "sustain_slope_db_s": 0.0, "release_ms": 0.0, "attack_ms": 0.0}
    assert loss_terms([row], [row, row], n_harm=1) is None


def test_a_pattern_with_no_analysis_notes_is_scorable_on_the_whole_timeline():
    """`scale` and `room-probe` have nothing per-note; the multi-scale term still does.

    No rows on either side is a pattern that carries no per-note evidence, not a
    render that failed — which is what a count mismatch is.
    """
    terms = loss_terms([], [], n_harm=1, mss=0.4)
    assert terms is not None
    assert terms["mss"] == pytest.approx(0.4)
    assert terms["harm"] == 0.0


def test_a_model_cleaner_than_its_oracle_is_not_penalised():
    def row(tnr):
        return {"harmonics_db": [0.0], "f0_cents_err": 0.0, "tnr_db": tnr,
                "sustain_slope_db_s": 0.0, "release_ms": 0.0, "attack_ms": 0.0}

    cleaner = loss_terms([row(40.0)], [row(20.0)], n_harm=1)
    noisier = loss_terms([row(10.0)], [row(20.0)], n_harm=1)
    assert cleaner["tnr"] == 0.0
    assert noisier["tnr"] == pytest.approx(10.0)


def test_a_silent_render_does_not_win_the_harmonic_term():
    """Silencing a gain was the cheapest way to score a perfect harmonic ladder.

    A render with no signal reports h1 at 0 dB by definition and every partial
    above it at the -120 dB floor. The floor guard then skips all of them, h1
    matches h1 exactly, and the term sums to 0.0 — its best possible value. It
    is not a match, it is the absence of anything to match, and the candidate
    has to be unscorable rather than optimal.
    """
    silent = {"harmonics_db": [0.0] + [-120.0] * 11, "f0_cents_err": 0.0,
              "tnr_db": 0.0, "sustain_slope_db_s": 0.0, "release_ms": 0.0,
              "attack_ms": 0.0}
    sounding = {"harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
                "f0_cents_err": 0.0, "tnr_db": 30.0, "sustain_slope_db_s": -3.0,
                "release_ms": 80.0, "attack_ms": 5.0}
    assert loss_terms([silent], [sounding], n_harm=12) is None
    assert loss_terms([sounding], [sounding], n_harm=12) is not None


def skeleton(bands, n=6):
    """A skeleton block whose three decay bands all carry `bands`."""
    return {"init_db": list(bands) + [None] * (12 - len(bands)),
            "early_db_s": list(bands), "late_db_s": list(bands),
            "tail_db_s": list(bands)}


def sounding_row(**over):
    row = {"harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9, "f0_cents_err": 0.0,
           "tnr_db": 30.0, "sustain_slope_db_s": -3.0, "release_ms": 80.0,
           "attack_ms": 5.0, "skeleton": skeleton([-2.0] * 6),
           "held_rms_dbfs": -20.0, "held_crest_db": 12.0}
    return row | over


def test_a_voice_that_stopped_sounding_does_not_win_the_decay_terms():
    """The same defect as the harmonic ladder's, one layer down.

    Comparing only the bands present on both sides means a model with no bands
    at all contributes nothing, and the decay terms average to 0.0 - their best
    value. Taking the amplitude envelope's sustain to zero reaches it: the note
    dies, every band goes unfitted at once, and three terms report a perfect
    match for a voice that makes no sound after its attack.
    """
    oracle = sounding_row()
    dead = sounding_row(skeleton=skeleton([None] * 6), held_crest_db=None)
    alive_but_wrong = sounding_row(skeleton=skeleton([-9.0] * 6))

    dead_terms = loss_terms([dead], [oracle], n_harm=12)
    wrong_terms = loss_terms([alive_but_wrong], [oracle], n_harm=12)
    for term in ("slope", "tail", "init"):
        assert dead_terms[term] > wrong_terms[term], term
    assert dead_terms["crest"] > 0.0


def test_a_band_the_reference_has_nothing_in_is_still_skipped():
    """The asymmetry is the point: an absent ORACLE value is a short probe.

    The aftersound band has no frames on a two-second probe, and charging the
    model for the reference's own silence would make every short probe score as
    a broken voice.
    """
    oracle = sounding_row(skeleton=skeleton([None] * 6))
    model = sounding_row(skeleton=skeleton([-2.0] * 6))
    terms = loss_terms([model], [oracle], n_harm=12)
    assert terms["slope"] == 0.0 and terms["tail"] == 0.0


def test_a_render_that_lost_only_its_top_partials_is_still_scored():
    """The guard is about having no partials at all, not about having few.

    A dark note is a defect the harmonic term exists to report, so rejecting it
    as unscorable would hide exactly what the fit is for.
    """
    dark = {"harmonics_db": [0.0, -8.0] + [-120.0] * 10, "f0_cents_err": 0.0,
            "tnr_db": 20.0, "sustain_slope_db_s": -3.0, "release_ms": 80.0,
            "attack_ms": 5.0}
    bright = {"harmonics_db": [0.0, -3.0, -6.0] + [-9.0] * 9, "f0_cents_err": 0.0,
              "tnr_db": 30.0, "sustain_slope_db_s": -3.0, "release_ms": 80.0,
              "attack_ms": 5.0}
    terms = loss_terms([dark], [bright], n_harm=12)
    assert terms is not None and terms["harm"] > 0.0
    # 5 dB on h2, weighted by how audible that partial is: these rows name
    # neither a pitch nor a measured f0, so only the masking half of the weight
    # applies and h2 at -3 dB under the loudest bin keeps 93 % of its vote.
    assert terms["harm"] == pytest.approx(5.0 * (1.0 - 3.0 / 45.0))
    # Unweighted it is exactly the raw difference, which is what
    # --flat-partial-weighting restores.
    flat = loss_terms([dark], [bright], n_harm=12, audibility=False)
    assert flat["harm"] == pytest.approx(5.0)


# --------------------------------------------------------------------------- #
# Stage classification
# --------------------------------------------------------------------------- #
def _knob(label: str) -> Knob:
    return Knob(label=label, lo=0.0, hi=1.0, log=False, start_value=0.5, tunable=label)


def test_stages_split_excitation_from_decay():
    knobs = [
        _knob("violin.bowed_string.bow_force"),
        _knob("violin.bowed_string.attack_ms"),
        _knob("violin.bowed_string.damping"),
        _knob("violin.amp_env.release_ms"),
        _knob("violin.cutoff_hz"),
    ]
    assert staged_indices(knobs, "excitation") == [0, 1]
    assert staged_indices(knobs, "decay") == [2, 3]


def test_a_section_name_does_not_classify_its_fields():
    """`bowed_string` contains "ring"; the field is what decides, not the section."""
    assert stage_of("violin.bowed_string.bow_force") == "excitation"
    assert stage_of("guitar.plucked_string.brightness") is None


def test_an_excitation_transient_is_not_a_loop_decay():
    """`click_decay_ms` is how fast the key click dies, not how fast the tone does."""
    assert stage_of("organ.additive.click_decay_ms") == "excitation"
    assert stage_of("organ.pipe_organ.tone_decay_s") == "decay"


def test_an_unclassified_knob_is_still_fitted_in_the_final_stage():
    """Only the final stage's membership matters for reach, and it takes everything."""
    knobs = [_knob("violin.cutoff_hz")]
    assert staged_indices(knobs, "excitation") == []
    assert staged_indices(knobs, "decay") == []


# --------------------------------------------------------------------------- #
# Write-back
# --------------------------------------------------------------------------- #
def test_array_elements_regain_their_brackets():
    assert key_to_member_path("pipe_organ.ranks2.level") == "pipe_organ.ranks[2].level"
    assert key_to_member_path("fm.ops1.ratio") == "fm.ops[1].ratio"
    assert key_to_member_path("modal.modes0.gain") == "modal.modes[0].gain"
    assert key_to_member_path("additive.drawbars3") == "additive.drawbars[3]"


def test_a_member_whose_name_ends_in_a_digit_is_not_an_array():
    assert key_to_member_path("lfo2_rate_hz") == "lfo2_rate_hz"
    assert key_to_member_path("bowed_string.bow_force") == "bowed_string.bow_force"


def test_every_indexed_key_the_library_forms_is_known_to_be_an_array():
    """The list is derived, because the hand-written one drifted and broke a build.

    Two percussion arrays were added to the tuning layer and not to the list, so
    a fitted drum value was written as `mode_ratios1` — not a member of
    anything. This asserts the derivation covers the whole key space rather than
    the four members somebody happened to remember.
    """
    members = array_members()
    text = (REPO_ROOT / TUNING_LAYER_FILE).read_text()
    assert len(members) >= 9
    for name in members:
        assert f'"{name}"' in text or f'{name}" +' in text


def test_an_array_whose_own_name_carries_digits_still_gets_its_brackets():
    # `shell_t60_s3` is the whole reason the index is read as the trailing run
    # rather than as everything after the last letter.
    assert "shell_t60_s" in array_members()
    assert key_to_member_path("percussion.shell_t60_s3") == "percussion.shell_t60_s[3]"
    assert key_to_member_path("percussion.mode_ratios1") == "percussion.mode_ratios[1]"
    assert key_to_member_path("percussion.mode_alpha2") == "percussion.mode_alpha[2]"


def test_the_patch_name_list_matches_the_x_macro():
    names = override_patch_names()
    assert len(names) == len(set(names))
    assert "violin" in names and "ocarina" in names and "church_organ" in names


def test_a_new_patch_field_is_appended_to_its_block():
    edited = write_patch_fields({"violin": [("bowed_string.bow_force", 0.61)]})
    assert len(edited) == 1
    text = next(iter(edited.values()))
    assert "o.violin.bowed_string.bow_force = 0.61f;" in text
    path = next(iter(edited))
    assert "o.violin.bowed_string.bow_force" not in path.read_text()


def test_an_existing_assignment_is_replaced_not_duplicated():
    """The table already sets `o.violin.cutoff_hz`; a fit must move it, not add a second."""
    edited = write_patch_fields({"violin": [("cutoff_hz", 5200.0)]})
    text = next(iter(edited.values()))
    assert text.count("o.violin.cutoff_hz") == 1
    assert "o.violin.cutoff_hz = 5200.0f;" in text


def test_the_appended_line_keeps_the_surrounding_indentation():
    edited = write_patch_fields({"violin": [("bowed_string.rosin", 0.2)]})
    text = next(iter(edited.values()))
    line = next(ln for ln in text.splitlines() if "o.violin.bowed_string.rosin" in ln)
    other = next(ln for ln in text.splitlines() if ln.strip().startswith("o.violin."))
    assert line[: len(line) - len(line.lstrip())] == other[: len(other) - len(other.lstrip())]


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #
def test_only_the_knobs_that_moved_reach_the_override_string():
    knobs = [_knob("a.b"), _knob("c.d")]
    assert tunable_overrides(knobs, [0.5, 0.9], changed_only=True) == "c.d=0.9"
    assert tunable_overrides(knobs, [0.5, 0.9]) == "a.b=0.5,c.d=0.9"


def test_a_written_literal_always_carries_a_decimal_point():
    assert format_value(22.0) == "22.0"
    assert format_value(0.5) == "0.5"


# --------------------------------------------------------------------------- #
# Drums
# --------------------------------------------------------------------------- #
def _probe_args(**kwargs) -> argparse.Namespace:
    """A Namespace carrying what the probe, the weights and the oracle routes read."""
    base = dict(
        program=0, drum_note=None, pattern="sustain", notes="", velocities="",
        # Every weight is None, exactly as the parser leaves one that was not
        # given, so the instrument's class defaults are what these tests see.
        w_harm=None, w_cents=None, w_tnr=None, w_env=None, w_init=None,
        w_slope=None, w_mss=None, w_band=None, w_bdecay=None, n_harm=10,
        w_tail=None, w_hf=None, w_level=None, w_crest=None, w_lf=None,
        w_stiff=None, w_dyn=None, w_modes=None, w_mod=None, w_kit=None,
        corpus="", corpus_timbre="", spec="auto",
        oracle_wav="", au="", au_dry=False, room="auto",
        validate_notes="", validate_velocities="", validate_oracle_wav="",
    )
    base.update(kwargs)
    return argparse.Namespace(**base)


def test_a_drum_probe_is_written_on_the_drum_channel():
    """Channel 10 is what makes a note number select an instrument, not a pitch."""
    probe = build_pattern("drum", 0, notes=(38,))
    assert probe.channel == 9
    assert probe.percussive is True
    assert {n.note for n in probe.notes} == {38}
    assert probe.analysis_notes == probe.notes


def test_a_drum_probe_leaves_each_hit_room_to_decay():
    """The gap has to outlast an open cymbal, and keep the next onset out of the window."""
    probe = build_pattern("drum", 0)
    gaps = [b.start - a.start for a, b in zip(probe.notes, probe.notes[1:])]
    assert gaps and min(gaps) >= 1.8
    assert max(n.dur for n in probe.notes) <= 0.1


def test_the_drum_holdout_shares_no_velocity_with_the_probe():
    """A held-out set that overlaps the fitted one measures nothing."""
    fitted = {n.velocity for n in build_pattern("drum", 0).notes}
    held = {n.velocity for n in build_pattern("drum-holdout", 0).notes}
    assert fitted and held and not (fitted & held)


def test_a_drum_note_selects_the_drum_pattern_and_its_own_notes():
    args = _probe_args(drum_note=38)
    resolve_probe(args)
    assert args.pattern == "drum"
    assert args.notes == "38"
    assert args.percussive is True


def test_an_explicit_melodic_pattern_with_a_drum_note_is_refused():
    """Channel 1 would sound pitch 38 rather than the snare — silently, if allowed."""
    with pytest.raises(ValueError, match="channel 1"):
        resolve_probe(_probe_args(drum_note=38, pattern="room-probe"))


def test_a_drum_pattern_without_a_drum_note_is_refused():
    """Nothing would tell the fit which note's knobs to move."""
    with pytest.raises(ValueError, match="--drum-note"):
        resolve_probe(_probe_args(pattern="drum"))


def test_a_pattern_with_no_analysis_notes_refuses_a_per_note_objective():
    """A weight NAMED on the command line is refused; an inherited one is dropped.

    Asking for a measurement the probe cannot take is a mistake worth reporting.
    Inheriting one from the instrument's class is not the caller saying
    anything, so it collapses to the whole-timeline term instead of failing a
    run nobody misconfigured.
    """
    with pytest.raises(ValueError, match="no analyzable notes"):
        resolve_probe(_probe_args(pattern="scale", w_harm=1.0))
    args = _probe_args(pattern="scale")
    resolve_probe(args)
    assert set(cli_weights(args)) <= {"mss"}


def test_a_pattern_with_no_analysis_notes_accepts_a_whole_timeline_objective():
    args = _probe_args(pattern="scale", w_harm=0.0, w_cents=0.0, w_tnr=0.0, w_mss=1.0)
    resolve_probe(args)  # does not raise
    assert args.percussive is False


def test_a_drum_fit_weights_the_percussion_terms_not_the_harmonic_ones():
    args = _probe_args(drum_note=38)
    resolve_probe(args)
    weights = cli_weights(args)
    # The percussion pair, the low end, the envelope, and the measured partial
    # series — a tom, a conga, a woodblock and a cowbell all have a pitch the
    # 1/3-octave profile cannot resolve. None of the harmonic ones: a hit has no
    # fundamental, so a ladder or an intonation error would be measuring a
    # frequency the sound does not contain.
    assert set(weights) == {"band", "bdecay", "lf", "env", "modes", "crest"}
    assert not {"harm", "cents", "tnr", "init", "slope", "tail", "hf",
                "stiff", "mod"} & set(weights)
    # `lf` is in both sets under one name and is not one measurement. For a
    # pitched voice it is the attack's low bands; here it is the kick's whole
    # region of the 1/3-octave profile, which `band` averages away.
    assert weights["lf"] == 1.0
    # The envelope is most of what tells two drums apart, so it is weighted for
    # a kit. A sustained voice weights it at half that: there it is a
    # refinement, and the spectrum is the identity.
    assert weights["env"] == 1.0
    # A bowed voice weights it at half that: there the envelope is a refinement
    # and the spectrum is the identity. A piano is a struck string and gets the
    # drum's weighting for the same reason a drum does.
    assert cli_weights(_probe_args(program=40))["env"] == 0.5
    assert cli_weights(_probe_args(program=0))["env"] == 1.0


def _hit(bands, decay, attack=1.0, decay_ms=200.0, crest=10.0) -> dict:
    return {"bands_db": bands, "band_decay_db_s": decay, "attack_ms": attack,
            "decay_ms": decay_ms, "crest_db": crest}


def test_a_matching_hit_scores_zero_on_every_percussion_term():
    hit = _hit([0.0, -6.0, -12.0], [-20.0, -30.0])
    terms = percussion_terms([hit], [hit])
    assert terms["band"] == 0.0
    assert terms["bdecay"] == 0.0
    assert terms["env"] == 0.0


def test_one_empty_band_cannot_decide_the_whole_objective():
    """Two noise floors differ by whatever they happen to be; the cap bounds it."""
    model = _hit([0.0, -60.0], [-20.0])
    oracle = _hit([0.0, -200.0], [-20.0])
    assert percussion_terms([model], [oracle])["band"] <= 24.0


def test_an_unfittable_band_decay_is_skipped_not_counted_as_agreement():
    model = _hit([0.0], [None, -30.0])
    oracle = _hit([0.0], [-20.0, -30.0])
    assert percussion_terms([model], [oracle])["bdecay"] == 0.0


def test_the_percussion_and_harmonic_paths_share_one_mismatch_rule():
    hit = _hit([0.0], [-20.0])
    assert percussion_terms([hit], [hit, hit]) is None
    assert percussion_terms([], [], mss=0.7)["mss"] == pytest.approx(0.7)


def test_percussion_knobs_land_in_the_stage_their_evidence_is_in():
    assert stage_of("d038.percussion.noise_decay_ms") == "excitation"
    assert stage_of("d038.percussion.strike_r") == "excitation"
    assert stage_of("d038.percussion.pitch_drop") == "excitation"
    assert stage_of("d038.percussion.mode_decay_s") == "decay"
    assert stage_of("d038.percussion.wire_buzz") == "decay"
    assert stage_of("d038.percussion.shimmer") == "decay"


def test_a_drum_spec_comes_from_the_note_not_from_the_program_map():
    """A drum note is not a GM program, so the program map has no entry for it."""
    cat = Catalogue(
        defaults={"d038.percussion.wire_buzz": 0.5, "d038.amp_env.decay_ms": 250.0,
                  "percussion_voice.kPhisemCollisionRate": 100.0,
                  "violin.bowed_string.bow_force": 0.55},
        programs={40: "violin"},
        bounds={"percussion.wire_buzz": (0.0, 4.0)},
    )
    spec = auto_spec(0, cat, drum_note=38)
    keys = {e["tunable"] for e in spec}
    assert "d038.percussion.wire_buzz" in keys
    assert "percussion_voice.kPhisemCollisionRate" in keys
    assert not any(k.startswith("violin.") for k in keys)
    # The clamp bound belongs to the field, so a `dNNN` key inherits it.
    buzz = next(e for e in spec if e["tunable"] == "d038.percussion.wire_buzz")
    assert (buzz["min"], buzz["max"]) == (0.0, 4.0)


def test_an_unknown_drum_note_is_a_clear_error_not_an_empty_spec():
    with pytest.raises(ValueError, match="drum note 99"):
        auto_spec(0, Catalogue({"d038.gain": 1.0}, {}, {}), drum_note=99)


def test_the_output_gain_is_not_offered_as_a_knob():
    """No objective here can see it, so a search handed it uses it to hide with.

    `gain` is applied after the nonlinearity, so it changes no shape, and every
    loss term is either normalised by the note's own level or measured around
    the grid's median offset. A fit that keeps it spends it absorbing whatever
    level its other choices cost — measured on a hi-hat that came back 31 dB
    down with a bit-identical band profile and a better score.
    """
    cat = Catalogue(
        defaults={"d038.gain": 0.8, "d038.percussion.wire_buzz": 0.5,
                  "violin.gain": 1.0, "violin.bowed_string.bow_force": 0.55},
        programs={(40, 0): "violin"},
        bounds={},
    )
    assert "d038.gain" not in {e["tunable"] for e in auto_spec(0, cat, drum_note=38)}
    assert "violin.gain" not in {e["tunable"] for e in auto_spec(40, cat)}
    # And the exclusion is by field name, not by a substring of the path.
    assert "d038.percussion.wire_buzz" in {
        e["tunable"] for e in auto_spec(0, cat, drum_note=38)
    }


def test_a_modes_bessel_zero_is_not_offered_but_its_ratio_is():
    """One is the membrane's geometry; the other is how the head is tuned.

    `mode_alpha` scales the argument the strike weighting is evaluated at, and
    `percussion_voice.h` ties it to the ratios by construction. A fit given it
    uses it as a per-mode gain instead — measured on a kick that took alpha0 to
    eight times the first zero of J0 while dropping alpha1 underneath it.
    """
    cat = Catalogue(
        defaults={"d036.percussion.mode_alpha0": 2.4048,
                  "d036.percussion.mode_alpha1": 3.8317,
                  "d036.percussion.mode_ratios1": 1.59,
                  "d036.percussion.mode_decay_s": 0.22},
        programs={},
        bounds={},
    )
    offered = {e["tunable"] for e in auto_spec(0, cat, drum_note=36)}
    assert not any(k.startswith("d036.percussion.mode_alpha") for k in offered)
    assert "d036.percussion.mode_ratios1" in offered
    assert "d036.percussion.mode_decay_s" in offered


def test_the_report_states_how_far_the_winner_moved_the_level():
    """Always, and not only when it is large: the quiet case is the one to confirm."""
    import io
    from contextlib import redirect_stdout

    from report import LEVEL_DRIFT_WARN_DB, print_level_drift

    class _Ev:
        start_level_offset_db = -2.0
        best_level_offset_db = -2.0 - LEVEL_DRIFT_WARN_DB * 2

    buf = io.StringIO()
    with redirect_stdout(buf):
        print_level_drift(_Ev())
    out = buf.getvalue()
    assert "-8.0 dB against the start point" in out
    assert "bought with loudness" in out

    class _Held(_Ev):
        best_level_offset_db = -2.4

    buf = io.StringIO()
    with redirect_stdout(buf):
        print_level_drift(_Held())
    out = buf.getvalue()
    assert "-0.4 dB against the start point" in out
    assert "bought with loudness" not in out


def test_a_drum_field_is_routed_to_the_drum_table_not_reported_as_unplaceable():
    knobs = [_knob("d038.percussion.wire_buzz"), _knob("fam3.piano.brightness")]
    per_patch, per_drum, other = patch_field_assignments(knobs, [0.9, 0.9])
    assert per_patch == {}
    assert per_drum == {38: [("percussion.wire_buzz", 0.9)]}
    assert other == ["fam3.piano.brightness"]


def test_a_fitted_drum_field_is_written_after_its_own_table_line():
    edited = write_drum_fields({38: [("percussion.wire_buzz", 0.9)]})
    text = next(iter(edited.values()))
    assert "t[38].percussion.wire_buzz = 0.9f;" in text
    lines = text.splitlines()
    written = next(i for i, ln in enumerate(lines) if "t[38].percussion.wire_buzz" in ln)
    anchor = next(i for i, ln in enumerate(lines) if ln.strip().startswith("t[38] ="))
    clamp = next(i for i, ln in enumerate(lines) if "clamp_synth_patch(p)" in ln)
    assert anchor < written < clamp  # inside the block, ahead of the clamp pass


def test_an_existing_drum_correction_is_moved_not_duplicated():
    """The table already sets `t[46].amp_env.release_ms`; a fit must replace it."""
    edited = write_drum_fields({46: [("amp_env.release_ms", 55.0)]})
    text = next(iter(edited.values()))
    assert text.count("t[46].amp_env.release_ms") == 1
    assert "t[46].amp_env.release_ms = 55.0f;" in text


def test_a_note_assigned_in_a_chain_still_finds_its_anchor():
    """`t[41] = t[43] = ... = d.tom;` is one statement; appending after it is correct."""
    edited = write_drum_fields({43: [("percussion.tone_gain", 0.7)]})
    text = next(iter(edited.values()))
    assert "t[43].percussion.tone_gain = 0.7f;" in text


# --------------------------------------------------------------------------- #
# The room probe
# --------------------------------------------------------------------------- #
def test_the_room_probe_leaves_more_silence_than_it_makes_sound():
    assert "room-probe" in PATTERN_BUILDERS
    probe = build_pattern("room-probe", 19)
    assert probe.analysis_notes == []
    gaps = [b.start - (a.start + a.dur) for a, b in zip(probe.notes, probe.notes[1:])]
    assert gaps and min(gaps) >= 3.0
    assert max(n.dur for n in probe.notes) <= 0.5


# --------------------------------------------------------------------------- #
# What the probe overrides may be combined with
# --------------------------------------------------------------------------- #
def test_every_pattern_either_takes_a_probe_override_or_refuses_it_by_name():
    """`--pattern` offers every builder, so every builder has to answer both flags.

    Answering does not have to mean accepting. `drum-sequence` plays a written
    phrase and has neither axis — its notes and velocities ARE the phrase — so
    what it owes a caller is an error naming what it does take, not a
    `TypeError` about a parameter the flag never mentioned.
    """
    for name in PATTERN_BUILDERS:
        for axis, value in (("notes", (62,)), ("velocities", (90,))):
            try:
                assert build_pattern(name, 0, **{axis: value}).notes
            except ValueError as exc:
                assert f"has no {axis} axis" in str(exc)
                assert "sequence" in str(exc) or "neither" in str(exc)


def test_a_single_pitch_pattern_takes_the_override_as_its_pitch():
    assert {n.note for n in build_pattern("velocity", 0, notes=(62,)).notes} == {62}
    assert {n.velocity for n in build_pattern("sustain", 0, velocities=(90,)).notes} == {90}


def test_a_list_handed_to_a_single_pitch_pattern_is_refused_by_name():
    """It cannot mean anything, and it must not mean the first value silently."""
    with pytest.raises(ValueError, match="single value"):
        build_pattern("velocity", 0, notes=(60, 62))
    with pytest.raises(ValueError, match="single value"):
        build_pattern("scale", 0, velocities=(60, 90))


def test_a_pattern_with_neither_axis_names_what_it_takes(monkeypatch):
    def fixed(program: int, *, dur: float = 1.0):
        return build_pattern("sustain", program)

    monkeypatch.setitem(PATTERN_BUILDERS, "fixed", fixed)
    with pytest.raises(ValueError, match="neither notes nor velocities"):
        build_pattern("fixed", 0, notes=(60,))


# --------------------------------------------------------------------------- #
# The per-note analysis window
# --------------------------------------------------------------------------- #
class _FakeMetrics:
    """Stands in for a measurement so a window test costs no spectra."""

    def to_dict(self) -> dict:
        return {}


def _window_ends(pattern, monkeypatch) -> list[tuple[float, float]]:
    """(onset, window end) for every analysis note `probe_rows` measures."""
    seen: list[tuple[float, float]] = []

    def spy(mono, sr, note, end, **kwargs):
        seen.append((note.start, end))
        return _FakeMetrics()

    monkeypatch.setattr(loss_module, "analyze_note", spy)
    monkeypatch.setattr(loss_module, "analyze_hit", spy)
    monkeypatch.setattr(loss_module, "skeleton_note", lambda *a, **k: {})
    probe_rows(np.zeros(int(pattern_length(pattern) * 48000), dtype=np.float32),
               pattern, 48000)
    return seen


def test_the_default_probe_would_overrun_its_own_gap():
    """The clamp is not hypothetical: the tail outlasts the space before the next note."""
    probe = build_pattern("sustain", 0)
    first, second = probe.notes[0], probe.notes[1]
    assert first.start + first.dur + probe.tail > second.start
    assert analysis_window_end(probe, first) == second.start


def test_a_sustained_note_is_never_measured_into_the_next_one(monkeypatch):
    """Its release would otherwise be the next note's attack."""
    probe = build_pattern("sustain", 0)
    onsets = [n.start for n in probe.notes]
    measured = _window_ends(probe, monkeypatch)
    assert len(measured) == len(probe.analysis_notes)
    for start, end in measured:
        later = [s for s in onsets if s > start]
        assert not later or end <= min(later)


def test_a_drum_hit_keeps_the_window_it_always_had(monkeypatch):
    probe = build_pattern("drum", 0)
    onsets = [n.start for n in probe.notes]
    for start, end in _window_ends(probe, monkeypatch):
        later = [s for s in onsets if s > start]
        assert end == (min(later) if later else pattern_length(probe))


# --------------------------------------------------------------------------- #
# Which oracle carries a room
# --------------------------------------------------------------------------- #
def _oracle_args(**kwargs) -> argparse.Namespace:
    base = dict(oracle_wav="", au="", au_dry=False, room="auto")
    base.update(kwargs)
    return argparse.Namespace(**base)


def test_an_au_oracle_is_treated_as_wet_unless_it_was_asked_to_be_dry():
    """The AU route is the most likely of the three to arrive in a hall."""
    assert oracle_may_carry_room(_oracle_args(au="Pianoteq"))
    assert not oracle_may_carry_room(_oracle_args(au="Pianoteq", au_dry=True))


def test_the_fluidsynth_oracle_is_dry_by_construction():
    assert not oracle_may_carry_room(_oracle_args())
    assert oracle_may_carry_room(_oracle_args(oracle_wav="rendered.wav"))


def test_an_au_oracle_has_its_room_measured(monkeypatch):
    """The measurement is what the model is then convolved to match."""
    measured = []
    monkeypatch.setattr(autofit, "obtain_oracle",
                        lambda *a, **k: np.zeros((48000, 1), dtype=np.float32))
    monkeypatch.setattr(autofit, "probe_rows", lambda *a, **k: [])
    monkeypatch.setattr(autofit, "estimate_room",
                        lambda *a, **k: measured.append(1) or DRY)
    args = _probe_args(au="Pianoteq", au_dry=False, oracle_wav="", room="auto")
    resolve_probe(args)
    autofit.oracle_reference(args)
    assert measured == [1]


# --------------------------------------------------------------------------- #
# The hold-out check
# --------------------------------------------------------------------------- #
def test_a_hold_out_against_a_fixed_wav_is_refused_before_the_fit_starts():
    """The WAV holds the fitted notes; scoring the held-out ones against it is fiction."""
    with pytest.raises(ValueError, match="needs its own reference"):
        resolve_probe(_probe_args(oracle_wav="probe.wav", validate_notes="43,55,67"))


def test_a_hold_out_reference_satisfies_the_check():
    resolve_probe(_probe_args(oracle_wav="probe.wav", validate_notes="43,55,67",
                              validate_oracle_wav="holdout.wav"))


def test_a_re_rendering_oracle_route_needs_no_hold_out_reference():
    """fluidsynth and the AU host both render whatever score they are handed."""
    resolve_probe(_probe_args(validate_notes="43,55,67"))
    resolve_probe(_probe_args(au="Pianoteq", validate_notes="43,55,67"))


def test_a_hold_out_reference_without_a_fit_reference_is_refused():
    with pytest.raises(ValueError, match="belongs with --oracle-wav"):
        check_holdout_oracle(_probe_args(validate_oracle_wav="holdout.wav"))


def test_the_hold_out_is_scored_against_its_own_reference(monkeypatch):
    seen: dict[str, str] = {}

    def fake_oracle(holdout):
        seen["wav"] = holdout.oracle_wav
        seen["notes"] = holdout.notes
        # Rows, audio, room, band edge — the edge is a percussion quantity and
        # this is a pitched probe, so None is what the real one returns here.
        return [], None, None, None

    monkeypatch.setattr(autofit, "oracle_reference", fake_oracle)
    args = _probe_args(oracle_wav="probe.wav", validate_notes="43,55,67",
                       validate_oracle_wav="holdout.wav")
    resolve_probe(args)
    assert validate(args, Path("."), [], [], [], None) is None
    assert seen == {"wav": "holdout.wav", "notes": "43,55,67"}


# --------------------------------------------------------------------------- #
# Sweeping a knob the library reads
# --------------------------------------------------------------------------- #
def test_room_match_refuses_a_library_that_ignores_the_override(monkeypatch):
    """Without BUILD_TUNING the decay axis is inert and every render is identical."""
    monkeypatch.setattr(voicematch, "dump_catalogue",
                        lambda *a, **k: Catalogue({"gs_effects.kOther": 1.0}, {}, {}))

    def refuse(*args, **kwargs):
        raise AssertionError("run_room_match rendered before checking the override table")

    monkeypatch.setattr(voicematch, "obtain_oracle", refuse)
    with pytest.raises(SystemExit, match="kReverbDecayScale"):
        voicematch.run_room_match(
            argparse.Namespace(programs="19", pattern="sustain", verbose=False)
        )


def test_room_match_reports_a_library_built_without_the_table(monkeypatch):
    def no_dump(*args, **kwargs):
        raise RuntimeError("the render produced no knob dump")

    monkeypatch.setattr(voicematch, "dump_catalogue", no_dump)
    with pytest.raises(SystemExit, match="BUILD_TUNING=ON"):
        voicematch.require_live_tunable(19, "sustain", voicematch.DECAY_SCALE_KEY)


def test_room_match_proceeds_when_the_library_reports_the_key(monkeypatch):
    monkeypatch.setattr(
        voicematch, "dump_catalogue",
        lambda *a, **k: Catalogue({voicematch.DECAY_SCALE_KEY: 1.0}, {}, {}),
    )
    voicematch.require_live_tunable(19, "sustain", voicematch.DECAY_SCALE_KEY)


# --------------------------------------------------------------------------- #
# The build dir a fit renders through
# --------------------------------------------------------------------------- #
def _cache(build_dir: Path, **options) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    lines = [f"CMAKE_HOME_DIRECTORY:INTERNAL={REPO_ROOT}"]
    lines += [f"{name}:STRING={value}" for name, value in options.items()]
    (build_dir / "CMakeCache.txt").write_text("\n".join(lines) + "\n")
    return build_dir


def test_a_dir_configured_without_the_shared_target_is_reconfigured(tmp_path, monkeypatch):
    """`sonare_shared` would not exist, and the build error names no option."""
    build_dir = _cache(tmp_path / "build", BUILD_TUNING="ON", BUILD_SHARED="OFF",
                       CMAKE_BUILD_TYPE="Release")
    ran: list[list[str]] = []
    monkeypatch.setattr(build_lib.subprocess, "run", lambda cmd, **k: ran.append(cmd))
    configure_build(build_dir, "cmake", tuning=True)
    assert ran and "-DBUILD_SHARED=ON" in ran[0]


def test_a_debug_dir_is_reconfigured_to_release(tmp_path, monkeypatch):
    build_dir = _cache(tmp_path / "build", BUILD_TUNING="OFF", BUILD_SHARED="ON",
                       CMAKE_BUILD_TYPE="Debug")
    ran: list[list[str]] = []
    monkeypatch.setattr(build_lib.subprocess, "run", lambda cmd, **k: ran.append(cmd))
    configure_build(build_dir, "cmake", tuning=False)
    assert ran and "-DCMAKE_BUILD_TYPE=Release" in ran[0]


def test_a_matching_dir_is_left_alone(tmp_path, monkeypatch):
    build_dir = _cache(tmp_path / "build", BUILD_TUNING="ON", BUILD_SHARED="ON",
                       CMAKE_BUILD_TYPE="Release")
    monkeypatch.setattr(build_lib.subprocess, "run",
                        lambda *a, **k: pytest.fail("reconfigured a compatible dir"))
    configure_build(build_dir, "cmake", tuning=True)


def test_a_dir_belonging_to_another_checkout_is_refused(tmp_path):
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    (build_dir / "CMakeCache.txt").write_text(
        "CMAKE_HOME_DIRECTORY:INTERNAL=/somewhere/else\nBUILD_SHARED:STRING=ON\n"
    )
    with pytest.raises(RuntimeError, match="/somewhere/else"):
        configure_build(build_dir, "cmake", tuning=True)


# --------------------------------------------------------------------------- #
# What a render is allowed to have come from
# --------------------------------------------------------------------------- #
class _ManifestEntry:
    def __init__(self, program: int, backend: int):
        self.program, self.backend = program, backend


def test_a_soundfont_render_is_not_reported_as_the_native_bank():
    check_gm_fallback([_ManifestEntry(0, 0), _ManifestEntry(40, 0)])
    with pytest.raises(RuntimeError, match="expected GM fallback"):
        check_gm_fallback([_ManifestEntry(0, 0), _ManifestEntry(40, 1)])


def test_the_corpus_renderer_checks_the_backend_too():
    """Read from the source: importing `render_corpus` loads the dylib at module scope."""
    source = (Path(__file__).resolve().parent / "render_corpus.py").read_text()
    assert "check_gm_fallback(manifest)" in source


# --------------------------------------------------------------------------- #
# Keeping the tree consistent with the candidate being scored
# --------------------------------------------------------------------------- #
def _source_knob(tmp_path: Path, name: str, literal: str) -> tuple[Path, Knob]:
    """A file holding one numeric literal, and the source knob that points at it."""
    path = tmp_path / name
    head, tail = "constexpr float kValue = ", "f;\n"
    path.write_text(head + literal + tail)
    return path, Knob(
        label=name, lo=0.0, hi=10.0, log=False, start_value=float(literal),
        file=path, pattern=r"kValue = ([0-9.]+)f",
        span_start=len(head), span_end=len(head) + len(literal),
    )


def test_a_file_whose_knob_is_back_at_its_default_is_still_written(tmp_path):
    """Left out, it would keep the previous candidate's value through this render."""
    a_path, a = _source_knob(tmp_path, "a.cpp", "1.0")
    b_path, b = _source_knob(tmp_path, "b.cpp", "2.0")
    pristine = {a_path: a_path.read_text(), b_path: b_path.read_text()}
    full = materialize([a, b], [9.0, 2.0], pristine, full=True)
    assert set(full) == {a_path, b_path}
    assert full[b_path] == pristine[b_path]
    # The report keeps the minimal diff: nothing to say about a knob that stayed.
    assert set(materialize([a, b], [9.0, 2.0], pristine)) == {a_path}


def _fit_args(**kwargs) -> argparse.Namespace:
    base = dict(raw_loss=False, workers=1, cmake="cmake", jobs=1, n_harm=10,
                percussive=False)
    base.update(kwargs)
    return _probe_args(**base)


def test_every_source_file_matches_the_candidate_being_rendered(tmp_path, monkeypatch):
    """A stale file would score a knob vector that was never assembled."""
    a_path, a = _source_knob(tmp_path, "a.cpp", "1.0")
    b_path, b = _source_knob(tmp_path, "b.cpp", "2.0")
    pristine = {a_path: a_path.read_text(), b_path: b_path.read_text()}
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    evaluator = Evaluator([a, b], pristine, [], None, _fit_args(), tmp_path / "build")

    on_disk: list[tuple[str, str]] = []

    def capture(values):
        on_disk.append((a_path.read_text(), b_path.read_text()))
        return _terms(harm=1.0)

    evaluator._render_terms = capture
    evaluator([1.0, 5.0])   # b moves away from its default
    evaluator([9.0, 2.0])   # ...and back to it, while a moves
    assert on_disk[1] == (materialize([a], [9.0], pristine, full=True)[a_path],
                          pristine[b_path])


def test_a_cached_point_still_decides_the_best(tmp_path, monkeypatch):
    """Each stage re-scores the point it inherited, which is always a cache hit."""
    knob = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
    evaluator = Evaluator([knob], {}, [], None, _fit_args(), tmp_path / "build")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    scores = {"0.5": 10.0, "0.9": 40.0}
    evaluator._render_terms = lambda values: _terms(harm=scores[format_value(values[0])])

    evaluator([0.5])
    evaluator.restage({"harm": 1.0}, "decay")
    evaluator([0.5])  # cached, and better than anything this stage renders
    evaluator([0.9])
    assert evaluator.best_values == [0.5]


# --------------------------------------------------------------------------- #
# Undoing a fit's edits without undoing anyone else's
# --------------------------------------------------------------------------- #
def test_restore_puts_back_what_the_fit_wrote(tmp_path):
    path, _ = _source_knob(tmp_path, "a.cpp", "1.0")
    pristine = {path: path.read_text()}
    written: dict[Path, str] = {}
    write_edits({path: "spliced\n"}, written)
    restore(pristine, written)
    assert path.read_text() == pristine[path]


def test_restore_leaves_a_file_the_fit_never_wrote(tmp_path):
    """A runtime knob's declaration file is snapshotted for the diff, never written."""
    path, _ = _source_knob(tmp_path, "declaration.cpp", "1.0")
    pristine = {path: path.read_text()}
    path.write_text("edited by hand while the fit ran\n")
    restore(pristine, {})
    assert path.read_text() == "edited by hand while the fit ran\n"


def test_restore_does_not_roll_back_an_edit_made_after_its_own(tmp_path, capsys):
    """Hours of fitting must not cost someone the file they were editing meanwhile."""
    path, _ = _source_knob(tmp_path, "a.cpp", "1.0")
    pristine = {path: path.read_text()}
    written: dict[Path, str] = {}
    write_edits({path: "spliced\n"}, written)
    path.write_text("edited by hand while the fit ran\n")
    restore(pristine, written)
    assert path.read_text() == "edited by hand while the fit ran\n"
    assert "was edited after the fit wrote it" in capsys.readouterr().err


# --------------------------------------------------------------------------- #
# Every named patch has somewhere to write to
# --------------------------------------------------------------------------- #
def _table_texts() -> dict[Path, str]:
    from writeback import PROGRAM_TABLE_FILES

    return {REPO_ROOT / name: (REPO_ROOT / name).read_text()
            for name in PROGRAM_TABLE_FILES if (REPO_ROOT / name).exists()}


def test_every_named_patch_has_a_write_back_site():
    """A patch with none falls silently into `unplaced` and the fit loop never closes."""
    tables = _table_texts()
    unplaced = [patch for patch in override_patch_names()
                if not write_patch_fields({patch: [("amp_env.release_ms", 123.0)]}, tables)]
    assert unplaced == []


def test_a_patch_built_through_a_reference_is_written_through_it():
    """A third of the melodic bank never spells `o.<patch>` after the binding line."""
    edited = write_patch_fields({"vibraphone": [("body_mix", 0.33)]})
    text = next(iter(edited.values()))
    lines = text.splitlines()
    written = next(i for i, ln in enumerate(lines) if "vb.body_mix = 0.33f;" in ln)
    bound = next(i for i, ln in enumerate(lines)
                 if "NativeSynthPatch& vb = o.vibraphone;" in ln)
    following = next(i for i, ln in enumerate(lines)
                     if i > bound and "NativeSynthPatch&" in ln)
    assert bound < written < following


def test_a_reference_built_patch_replaces_its_own_line_rather_than_adding_one():
    edited = write_patch_fields({"vibraphone": [("modal.decay_s", 4.2)]})
    text = next(iter(edited.values()))
    assert text.count("vb.modal.decay_s =") == 1
    assert "vb.modal.decay_s = 4.2f;" in text


def test_two_write_backs_into_one_file_both_survive(tmp_path, monkeypatch, capsys):
    """A `SONARE_TUNABLE` splice and a patch-field line can land in the same file."""
    path, knob = _source_knob(tmp_path, "shared.h", "1.0")
    knob = Knob(**{**vars(knob), "tunable": "violin.kValue"})
    patch_knob = Knob(label="violin.cutoff_hz", lo=0.0, hi=1.0, log=False,
                      start_value=0.5, tunable="violin.cutoff_hz")

    def fake_write(per_patch, base=None):
        return {path: (base or {})[path] + "// patch field\n"}

    monkeypatch.setattr(report_module, "write_patch_fields", fake_write)
    monkeypatch.setattr(report_module, "REPO_ROOT", tmp_path)  # the knob file lives here
    evaluator = argparse.Namespace(trajectory=[], best_loss=0.5, normalize=True)
    args = _probe_args(out="", dry_run=True)
    report_result([knob, patch_knob], {path: path.read_text()}, [7.0, 0.9],
                  evaluator, args)
    printed = capsys.readouterr().out
    assert "kValue = 7.0f" in printed and "// patch field" in printed


# --------------------------------------------------------------------------- #
# Termination
# --------------------------------------------------------------------------- #
class _CacheOnlyEvaluator:
    """Every point it is asked about has already been rendered.

    Which is a state a converged search reaches: the budget counts renders, so
    a generation of cache hits advances nothing the loop can terminate on.
    """

    def __init__(self, start: list[float], limit: int = 400):
        self.trajectory: list[tuple[float, float, str]] = []
        self.best_loss = 1.0
        self.best_values = list(start)
        self.workers = 1
        self.quiet = False
        self.calls = 0
        self.limit = limit

    def __call__(self, values) -> float:
        self.calls += 1
        if self.calls > self.limit:
            raise AssertionError(
                f"the search did not terminate: {self.calls} evaluations, all cached, "
                f"trajectory still empty"
            )
        return 1.0

    def evaluate_batch(self, batch) -> list[float]:
        return [self(values) for values in batch]


def _optimizer_args(**kwargs) -> argparse.Namespace:
    base = dict(max_evals=30, per_knob_evals=6, population=6, sigma0=0.25, seed=0,
                restarts=0)
    base.update(kwargs)
    return argparse.Namespace(**base)


def test_cma_es_terminates_when_every_candidate_is_a_cache_hit():
    knobs = [_knob("a.b"), _knob("c.d")]
    evaluator = _CacheOnlyEvaluator([k.start_value for k in knobs])
    assert cma_es(evaluator, knobs, _optimizer_args()) == [0.5, 0.5]


def test_coordinate_descent_terminates_when_every_candidate_is_a_cache_hit():
    knobs = [_knob("a.b"), _knob("c.d")]
    evaluator = _CacheOnlyEvaluator([k.start_value for k in knobs])
    assert optimize(evaluator, knobs, _optimizer_args()) == [0.5, 0.5]


# --------------------------------------------------------------------------- #
# Starting up the way a user starts it
# --------------------------------------------------------------------------- #
HERE = Path(__file__).resolve().parent

def _is_entry_point(path: Path) -> bool:
    """Whether a file has a `__main__` block — the line itself, not a mention of it.

    Anchored on the whole line because this file quotes that source line, and a
    substring search over the directory therefore matches the test module too.
    """
    if path.name.startswith("test_"):
        return False
    return any(
        line.rstrip() == 'if __name__ == "__main__":'
        for line in path.read_text().splitlines()
    )


# Every script here with a `__main__`, discovered rather than listed: the next
# module to grow one is covered without anyone remembering to add it.
CLI_SCRIPTS = sorted(path.name for path in HERE.glob("*.py") if _is_entry_point(path))

# Run the file's module scope in a child, with the path a direct `python
# <script>` produces and nothing else. `run_name` keeps `main()` out of it —
# what is under test is what happens before the first line of a command runs.
_IMPORT_SMOKE = (
    "import runpy, sys\n"
    "sys.path[0] = sys.argv[1]\n"
    "runpy.run_path(sys.argv[2], run_name='__voicematch_import_smoke__')\n"
)


def _needs_native_library(path: Path) -> bool:
    """Whether a script loads the C library while it imports rather than when it runs.

    Column zero only: `render_model` imports `libsonare` inside a function, on
    purpose, so that `SONARE_LIB_PATH` is set before the dylib is mapped.
    """
    return any(
        line.startswith(("import libsonare", "from libsonare import"))
        for line in path.read_text().splitlines()
    )


# What `render_model.ensure_lib_path` would resolve to, replayed in a child with
# no repo imports of its own, so the only thing that can fail in it is the load.
_NATIVE_PROBE = (
    "import os, sys\n"
    "if 'SONARE_LIB_PATH' not in os.environ and os.path.exists(sys.argv[1]):\n"
    "    os.environ['SONARE_LIB_PATH'] = sys.argv[1]\n"
    "import libsonare\n"
)


@functools.lru_cache(maxsize=1)
def _native_library_loads() -> bool:
    """Whether a libsonare dylib is available to load at all."""
    proc = subprocess.run(
        [sys.executable, "-c", _NATIVE_PROBE, str(DEFAULT_DYLIB)],
        capture_output=True, text=True,
    )
    return proc.returncode == 0


def test_every_entry_point_is_covered_by_the_import_smoke():
    """The discovery is the test's reach; an empty or shrunken list is a silent pass."""
    assert len(CLI_SCRIPTS) >= 7
    assert {"voicematch.py", "autofit.py"} <= set(CLI_SCRIPTS)
    # Covered even where the case below can only skip: a script that cannot run
    # here still has to be one the suite knows about.
    assert "render_corpus.py" in CLI_SCRIPTS
    assert not [s for s in CLI_SCRIPTS if s.startswith("test_")]  # not entry points


@pytest.mark.parametrize("script", CLI_SCRIPTS)
def test_a_cli_entry_point_imports_as_shipped(script, tmp_path):
    """Each CLI must resolve its own imports without this test file's help.

    `python tools/voicematch/<script>.py` puts `tools/voicematch/` on the path
    and nothing else, so a module-scope import of anything living in `tools/`
    — `_repo`, reached through `catalogue`, `knobs` or `writeback` — needs the
    script to insert that directory itself. No in-process test can see the
    difference: this file puts `tools/` on the path when pytest imports it, so
    every module resolves either way and a CLI that dies on the user's first
    keystroke passes anyway. Hence a child process, started the way the
    docstrings say to start it.

    `render_corpus.py` imports libsonare at module scope, so its case needs the
    built dylib as well as a resolvable path. That one is skipped when the
    library is absent rather than failed: this suite is otherwise pure Python
    and runs from a bare checkout, and a developer who reads one environmental
    red tends to stop trusting every other case with it. The skip is keyed on
    whether the library loads at all, so an ImportError from inside
    `render_corpus.py` is still red, it stays a hard failure wherever a dylib is
    present, and the coverage test above still requires the script to be
    discovered.
    """
    if _needs_native_library(HERE / script) and not _native_library_loads():
        pytest.skip(
            f"{script} imports libsonare at module scope and no shared library is "
            f"available: set SONARE_LIB_PATH, or build one with "
            f"`cmake --build build-python-shared --target sonare_shared` "
            f"(expected at {DEFAULT_DYLIB})"
        )
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)  # never let an ambient path answer the question
    proc = subprocess.run(
        [sys.executable, "-c", _IMPORT_SMOKE, str(HERE), str(HERE / script)],
        capture_output=True, text=True, cwd=tmp_path, env=env,
    )
    assert proc.returncode == 0, (
        f"{script} does not import on a clean interpreter "
        f"(exit {proc.returncode}):\n{proc.stderr.strip()[-1200:]}"
    )


# --------------------------------------------------------------------------- #
# The captured corpus as a probe
# --------------------------------------------------------------------------- #
def _write_corpus(root: Path, *, notes=(60, 72), velocities=(56, 120),
                  gate_ms=8000, seconds=10.1, preroll_ms=100, dry=True,
                  channel=1, groups=None, rig=None) -> Path:
    """A miniature capture: one short tone per slot, plus the manifest beside it.

    `seconds` is a number for a grid captured at one flat tail, or a note-keyed
    dict for one that recorded longer for some of its notes — which is what
    `tail_by_note` produces and what a single slot length cannot describe.
    """
    sr = 48000
    root.mkdir(parents=True, exist_ok=True)
    renders = []
    for note in notes:
        secs = seconds[note] if isinstance(seconds, dict) else seconds
        for vel in velocities:
            rel = f"t/n{note:03d}_v{vel:03d}.wav"
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            n = int(secs * sr)
            t = np.arange(n) / sr
            # Silence through the preroll, then a decaying tone at the note's own
            # pitch, so a misplaced onset shows up as a measurable shift.
            body = np.sin(2 * np.pi * 440.0 * 2 ** ((note - 69) / 12.0) * t)
            body *= np.exp(-t * 1.5) * (vel / 127.0)
            body[: int(preroll_ms / 1000.0 * sr)] = 0.0
            write_wav(path, np.stack([body, body], axis=1).astype(np.float32), sr)
            renders.append({"id": rel, "timbre": "t", "note": note, "velocity": vel,
                            "path": rel, "seconds": secs})
    header = {
        "id": "mini", "sample_rate": sr, "gate_ms": gate_ms, "tail": "2s",
        "preroll_ms": preroll_ms, "dry": dry,
        "timbres": [{"id": "t", "label": "mini timbre", "channel": channel}],
        "notes": list(notes), "velocities": list(velocities), "renders": renders,
    }
    if groups is not None:
        header["groups"] = groups
    # Left out entirely unless asked for, so a manifest that never answered the
    # rig question can be written — which is what every corpus captured before
    # the field looks like.
    if rig is not None:
        header["rig"] = rig
    (root / "manifest.json").write_text(json.dumps(header))
    return root


def test_a_corpus_probe_is_laid_out_by_the_capture_not_by_a_builder(tmp_path):
    """Its notes, its velocities and its gate all come from the manifest."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus)
    assert [(n.note, n.velocity) for n in probe.notes] == [
        (60, 56), (60, 120), (72, 56), (72, 120)
    ]
    assert {n.dur for n in probe.notes} == {8.0}
    assert probe.analysis_notes == probe.notes


def test_corpus_slots_are_spaced_by_the_capture_s_own_length(tmp_path):
    """So a note's analysis window is exactly the audio recorded for it.

    A gap chosen independently of the capture would either cut the reference's
    tail off or leave the model ringing into the next slot, and either one is a
    decay metric measuring the layout rather than the voice.
    """
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus)
    starts = [n.start for n in probe.notes]
    assert all(b - a == pytest.approx(corpus.slot_s) for a, b in zip(starts, starts[1:]))
    # 10.1 s captured minus the 0.1 s preroll that is dropped on assembly.
    assert corpus.slot_s == pytest.approx(10.0)
    for note in probe.notes:
        assert analysis_window_end(probe, note) == pytest.approx(note.start + corpus.slot_s)


def test_a_note_captured_for_longer_is_analysed_for_longer(tmp_path):
    """A grid with a per-note tail gives each slot the window it was recorded in.

    A kit records eight seconds for a ride and two for a kick, because a cymbal's
    wash is most of what makes it a cymbal. Reduced to one slot length the whole
    grid takes the short one, and six seconds of ride are dropped before any
    metric sees them — which reads as a model whose bands all decay too fast,
    on the notes the longer tail was captured for.
    """
    root = _write_corpus(tmp_path / "c", gate_ms=1000,
                         seconds={60: 4.1, 72: 10.1})
    corpus = load_corpus(root)
    probe = corpus_pattern(corpus, velocities=(56,))
    starts = [n.start for n in probe.notes]
    # The short note is followed by its own 4 s, not by the grid's longest and
    # not by the flat gate + tail that a single length would have fallen back to.
    assert starts == pytest.approx([0.0, 4.0])
    assert probe.tail == pytest.approx(10.0 - 1.0)

    assert corpus.slot_for(60, 56) == pytest.approx(4.0)
    assert corpus.slot_for(72, 56) == pytest.approx(10.0)
    for note in probe.notes:
        assert analysis_window_end(probe, note) == pytest.approx(
            note.start + corpus.slot_for(note.note, note.velocity)
        )


def test_the_long_note_keeps_its_tail_when_the_grid_also_holds_short_ones(tmp_path):
    """The assembled oracle carries the full capture, not the shortest slot's worth."""
    root = _write_corpus(tmp_path / "c", gate_ms=1000,
                         seconds={60: 4.1, 72: 10.1})
    corpus = load_corpus(root)
    probe = corpus_pattern(corpus, notes=(72,), velocities=(56,))
    audio = corpus_oracle(corpus, probe, 48000)
    # 10.1 s captured less the 0.1 s preroll that assembly drops.
    assert len(audio) / 48000.0 == pytest.approx(10.0, abs=0.01)


def test_the_corpus_oracle_places_each_capture_at_its_own_onset(tmp_path):
    """The preroll is dropped, so a captured onset lands where the model's does."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus, notes=(60,), velocities=(120,))
    audio = corpus_oracle(corpus, probe, 48000)
    mono = np.abs(audio).mean(axis=1)
    onset = int(np.argmax(mono > 0.01)) / 48000.0
    assert onset == pytest.approx(probe.notes[0].start, abs=0.005)


def test_a_corpus_run_refuses_a_grid_it_has_no_captures_for(tmp_path):
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    with pytest.raises(ValueError, match="no capture"):
        corpus_pattern(corpus, notes=(60, 61), velocities=(56,))


def test_a_corpus_run_refuses_the_oracle_routes_that_would_contradict_it(tmp_path):
    args = _probe_args(corpus=str(_write_corpus(tmp_path / "c")), oracle_wav="/tmp/x.wav")
    with pytest.raises(ValueError, match="both name the reference"):
        resolve_probe(args)
    args = _probe_args(corpus=str(tmp_path / "c"), drum_note=38)
    with pytest.raises(ValueError, match="pitched single notes"):
        resolve_probe(args)


def test_a_kit_corpus_and_a_drum_note_go_together_and_each_needs_the_other(tmp_path):
    """The pairing is decided by the capture's channel, not by the flag alone.

    A kit corpus IS a grid the drum probe has captures for, which is what makes
    the two compatible; the refusal that used to be unconditional was written
    before one existed. Both one-sided combinations stay refused: a drum probe
    over a pitched corpus scores every slot against silence, and a kit corpus
    with no drum note has no single patch to move, since a kit has one per note.
    """
    kit = _write_corpus(tmp_path / "kit", notes=(38, 42), velocities=(56, 120), channel=10)
    corpus = load_corpus(kit)
    assert corpus.percussive()
    probe = corpus_pattern(corpus, notes=(42,), velocities=(56, 120))
    assert probe.percussive and probe.channel == 9

    args = _probe_args(corpus=str(kit), drum_note=42)
    resolve_probe(args)
    assert args.percussive

    args = _probe_args(corpus=str(kit))
    with pytest.raises(ValueError, match="pass --drum-note"):
        resolve_probe(args)


def test_a_corpus_probe_reports_its_dryness_from_the_capture_config(tmp_path):
    """A wet capture has to be measured; a dry one must not have a room invented for it."""
    assert load_corpus(_write_corpus(tmp_path / "dry", dry=True)).dry is True
    assert load_corpus(_write_corpus(tmp_path / "wet", dry=False)).dry is False


def test_a_corpus_carries_what_its_capture_answered_about_a_rig(tmp_path):
    """And a manifest that never answered reads as unclassified, not as `none`."""
    assert load_corpus(_write_corpus(tmp_path / "u")).rig == RIG_UNCLASSIFIED
    assert load_corpus(_write_corpus(tmp_path / "n", rig=RIG_NONE)).rig == RIG_NONE
    assert load_corpus(_write_corpus(tmp_path / "b", rig=RIG_BAKED)).rig == RIG_BAKED
    # Dry and rigged together is the pair the record exists for: a close-mic'd
    # amplifier has no tail, so dryness reads clean with the whole rig in it.
    rigged = load_corpus(_write_corpus(tmp_path / "amp", dry=True, rig=RIG_BAKED))
    assert rigged.dry is True and rigged.rig == RIG_BAKED
    # An answer nothing understands is an unanswered one. `load_config` refuses a
    # misspelling outright; a hand-edited manifest never goes through it, and
    # reading "DI" as `none` would let a rigged reference into a fit.
    assert load_corpus(_write_corpus(tmp_path / "typo", rig="DI")).rig == RIG_UNCLASSIFIED


def test_the_model_stops_on_the_same_side_of_the_boundary_the_reference_did():
    """The rig record drives the model render, not only the fit refusal.

    A direct reference is compared against the model's direct signal; one
    recorded through an amplifier against the model plus its rig. Getting this
    backwards measures the amplifier as if it were the string, and the model
    side is exactly where nothing would complain.
    """
    from capture import model_rig

    assert model_rig(RIG_NONE) is False
    assert model_rig(RIG_BAKED) is True
    # Unanswered gets the rig, which is the product sound. Comparing is what an
    # unclassified reference is still allowed to do; fitting is what it is not,
    # and that refusal is `check_rig`'s rather than this function's.
    assert model_rig(RIG_UNCLASSIFIED) is True


def test_a_fit_against_a_rigged_reference_is_refused(tmp_path):
    """A rig has no inverse, so unlike a room it cannot be measured out of the
    reference; the fit would reproduce an amplifier with the instrument's own
    parameters and lose the values the moment the rig became a stage."""
    root = _write_corpus(tmp_path / "amp", rig=RIG_BAKED)
    with pytest.raises(ValueError, match="carries a rig"):
        resolve_probe(_probe_args(program=30, corpus=str(root)))
    # `baked` is an answer wherever it is written, so the family table does not
    # get to overrule one.
    with pytest.raises(ValueError, match="carries a rig"):
        resolve_probe(_probe_args(program=0, corpus=str(root)))


def test_an_unanswered_rig_stops_a_fit_only_where_a_rig_is_possible(tmp_path):
    """Seventy captures predate the question, and answering them all is not the
    price of fitting a flute. Where a rig IS possible the silence is the hazard:
    the rigged reference is usually the one that already exists."""
    root = _write_corpus(tmp_path / "u")
    with pytest.raises(ValueError, match="nothing says whether"):
        resolve_probe(_probe_args(program=30, corpus=str(root)))
    for program in (0, 19, 73):  # a piano, a church organ's building, a flute
        args = _probe_args(program=program, corpus=str(root))
        resolve_probe(args)
        assert args.pattern == "corpus"


def test_a_reference_captured_at_the_instrument_s_boundary_fits(tmp_path):
    """`none` is the answer a fit is for, on the family that could have said otherwise."""
    args = _probe_args(program=30, corpus=str(_write_corpus(tmp_path / "di", rig=RIG_NONE)))
    resolve_probe(args)
    assert args.pattern == "corpus"


def test_comparing_and_diagnosing_a_rigged_reference_are_unaffected(tmp_path):
    """The gate is on the fit alone. Checking the instrument-and-rig against a
    reference that carries one is the acceptance measurement the rule asks for,
    and a diagnosis reports which knobs reach which term rather than moving any
    of them towards the reference."""
    root = _write_corpus(tmp_path / "amp", rig=RIG_BAKED)
    args = _probe_args(program=30, corpus=str(root), diagnose=True)
    resolve_probe(args)
    assert args.pattern == "corpus"
    corpus = load_corpus(root)
    assert corpus_oracle(corpus, corpus_pattern(corpus), 48000).size


def test_the_rig_refusal_can_be_pushed_through_and_says_so(tmp_path, capsys):
    """Explicitly, and loudly: the values a forced run produces transfer to nothing."""
    args = _probe_args(program=30, allow_rigged_oracle=True,
                       corpus=str(_write_corpus(tmp_path / "amp", rig=RIG_BAKED)))
    resolve_probe(args)
    assert args.pattern == "corpus"
    assert "--allow-rigged-oracle" in capsys.readouterr().err


def test_an_oracle_route_with_no_rig_record_warns_where_a_rig_is_possible(capsys):
    """`--oracle-wav` and `--au` cannot answer the question a capture answers,
    and the hazard is the same size. Warned rather than refused, since nothing on
    this route knows the answer — but not silent, because being unclassifiable
    and being safe are different things."""
    for route in ({"oracle_wav": "amp.wav"}, {"au": "SomeAmpSim"}):
        resolve_probe(_probe_args(program=30, **route))
        assert "carries no record of a rig" in capsys.readouterr().err
    # Not on a family nobody is waiting on.
    resolve_probe(_probe_args(program=73, oracle_wav="flute.wav"))
    assert "rig" not in capsys.readouterr().err


def test_the_built_in_gm_oracle_is_refused_on_a_family_it_cannot_be_a_di_for():
    """Not "unclassified" — known. General MIDI defines these programs by the
    sound of an amplified instrument, so a sample set's recording of one has the
    cabinet in it, and no capture field can change that answer."""
    with pytest.raises(ValueError, match="cannot be a DI"):
        resolve_probe(_probe_args(program=30))
    with pytest.raises(ValueError, match="cannot be a DI"):
        resolve_probe(_probe_args(program=33, sf2="general.sf2"))
    # A flute is not waiting on anyone, here as everywhere.
    resolve_probe(_probe_args(program=73))


def test_the_gm_oracle_refusal_takes_the_same_override_as_the_capture_one(capsys):
    """One flag, because the certainty is not uniform across the family — 29 and
    30 are definitional while 33-37 are only usual — and a second table splitting
    the sure from the likely is the thing that drifts."""
    args = _probe_args(program=33, allow_rigged_oracle=True)
    resolve_probe(args)
    err = capsys.readouterr().err
    assert "--allow-rigged-oracle" in err and "cannot be a DI" in err


def test_a_corpus_carries_the_families_its_capture_identified(tmp_path):
    """A kit's own note numbers say nothing about which of them are one drum."""
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    assert load_corpus(kit).groups == {"toms": (41, 43, 45)}
    # A capture that named none gives none rather than a guess from note numbers,
    # and so does a manifest written before the block existed.
    assert load_corpus(_write_corpus(tmp_path / "plain")).groups == {}


def test_the_kit_relations_are_dropped_when_a_probe_has_no_family_to_read(tmp_path):
    """`--drum-note N` narrows the grid to that one note unless --notes says
    otherwise, so the default drum fit has no family in it at all. The class
    default supplies `kit` for every drum fit and it turns itself off there,
    rather than scoring 0.0 — which is also its best value.
    """
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    one = _probe_args(corpus=str(kit), drum_note=41)
    resolve_probe(one)
    assert one.notes == "41"
    assert one.has_kit_groups is False
    assert "kit" not in cli_weights(one)

    whole = _probe_args(corpus=str(kit), drum_note=41, notes="41,43,45")
    resolve_probe(whole)
    assert whole.has_kit_groups is True
    assert cli_weights(whole)["kit"] == 1.0


def test_an_explicit_kit_weight_is_refused_rather_than_scored_at_its_best(tmp_path):
    """Dropping a class default is right; dropping what someone asked for is not."""
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    args = _probe_args(corpus=str(kit), drum_note=41, w_kit=1.0)
    with pytest.raises(ValueError, match="relations inside a kit"):
        resolve_probe(args)


# --------------------------------------------------------------------------- #
# Level, crest, the aftersound band and the attack's high end
# --------------------------------------------------------------------------- #
def _level_row(held, crest, peak=-10.0) -> dict:
    return {"peak_dbfs": peak, "held_rms_dbfs": held, "held_crest_db": crest}


def test_the_level_term_scores_the_spread_and_not_the_output_gain():
    """A model uniformly louder than its reference is a gain, not a voicing error.

    Fitting it would spend the voice's knobs on a number no knob here is for,
    so the grid's own median offset comes out first and what is scored is what
    is left.
    """
    model = [_level_row(h, 10.0) for h in (-20.0, -22.0, -24.0)]
    oracle = [_level_row(h, 10.0) for h in (-29.0, -31.0, -33.0)]
    balance, crest, offset = loss_module._level_terms(model, oracle)
    assert offset == pytest.approx(9.0)
    assert balance == pytest.approx(0.0)
    assert crest == pytest.approx(0.0)


def test_the_level_term_does_see_a_register_that_is_the_wrong_loudness():
    model = [_level_row(-20.0, 10.0), _level_row(-20.0, 10.0), _level_row(-14.0, 10.0)]
    oracle = [_level_row(-30.0, 10.0)] * 3
    balance, _, offset = loss_module._level_terms(model, oracle)
    assert offset == pytest.approx(10.0)
    assert balance > 1.0


def test_crest_survives_a_gain_difference_because_it_is_a_ratio():
    """The defect it exists for — a note that never falls after its attack."""
    model = [_level_row(-20.0, 8.0)]
    oracle = [_level_row(-40.0, 26.0)]
    _, crest, _ = loss_module._level_terms(model, oracle)
    assert crest == pytest.approx(18.0)


def test_a_held_window_with_no_note_left_in_it_reports_no_level():
    """Two renders that have both decayed to nothing otherwise score a perfect match."""
    sr = 48000
    note = Note(84, 96, 0.0, 8.0)
    t = np.arange(int(8.5 * sr)) / sr
    dead = np.sin(2 * np.pi * 1000.0 * t) * np.exp(-t * 200.0)
    row = level_of(dead, sr, note, 8.5)
    assert row["held_rms_dbfs"] is None
    assert row["held_crest_db"] is None
    assert row["peak_dbfs"] is not None
    alive = np.sin(2 * np.pi * 1000.0 * t) * np.exp(-t * 0.5)
    assert level_of(alive, sr, note, 8.5)["held_rms_dbfs"] is not None


def test_the_held_window_does_not_move_with_the_probe_s_gate():
    """A fraction of the note would read the top octave entirely after it has stopped."""
    sr = 48000
    t = np.arange(int(9.0 * sr)) / sr
    tone = np.sin(2 * np.pi * 1000.0 * t) * np.exp(-t * 0.8)
    short = level_of(tone, sr, Note(60, 96, 0.0, 2.0), 3.5)
    long = level_of(tone, sr, Note(60, 96, 0.0, 8.0), 9.0)
    assert short["held_rms_dbfs"] == pytest.approx(long["held_rms_dbfs"], abs=0.01)


def test_the_aftersound_band_only_exists_when_the_probe_holds_the_note_that_long():
    sr = 48000
    t = np.arange(int(8.0 * sr)) / sr
    tone = np.sin(2 * np.pi * 440.0 * t) * np.exp(-t * 0.4)
    held = skeleton_note(tone, sr, Note(69, 96, 0.0, 8.0))
    brief = skeleton_note(tone, sr, Note(69, 96, 0.0, 2.0))
    assert held["tail_db_s"][0] is not None
    assert brief["tail_db_s"][0] is None
    # The bands the two-second probe could always reach are unaffected.
    assert brief["early_db_s"][0] is not None
    assert brief["late_db_s"][0] is not None


def _refine_probe(sr, f_target, interferers=()):
    """A windowed half-second segment carrying one partial plus what crowds it."""
    t = np.arange(int(0.5 * sr)) / sr
    rng = np.random.default_rng(0)
    sig = np.sin(2 * np.pi * f_target * t)
    for i, (freq, level) in enumerate(interferers):
        sig = sig + level * np.sin(2 * np.pi * freq * t + 0.7 * (i + 1))
    return (sig + 0.01 * rng.standard_normal(t.size)) * np.hanning(t.size), t


@pytest.mark.parametrize("f0", [55.0, 110.3, 220.7, 440.3, 745.0, 1319.5, 2637.0])
@pytest.mark.parametrize("harmonic", [1, 2, 5, 12])
def test_the_stepped_refiner_picks_the_same_partial_as_the_direct_one(f0, harmonic):
    """The recurrence is an optimisation, so its only licence is the same answer.

    Crowded on purpose: an approximate refiner tried here first, and what broke
    it was not the isolated tone but a strong neighbour a few Hz off the point
    where its decimation folded. That failure is silent -- a wrong frequency
    still yields a decay slope and an onset level, so the skeleton terms come
    back confident and mean nothing -- so the neighbours stay in the fixture.
    """
    sr = 48000
    f = f0 * harmonic
    if f > sr / 2 - 500.0:
        pytest.skip("above the range the refiner is asked about")
    crowd = [(f + k * 750.0 + d, 0.5) for k in (1, 2, 3) for d in (-5.0, 0.0, 5.0)]
    ref_w, t_ref = _refine_probe(sr, f, crowd)
    assert _refine_partial(ref_w, t_ref, f) == _refine_partial_direct(ref_w, t_ref, f)


def test_the_stepped_refiner_answers_from_the_grid_it_was_given():
    """Its value is one of the 41 candidates, not an interpolation between them."""
    sr = 48000
    ref_w, t_ref = _refine_probe(sr, 443.1)
    got = _refine_partial(ref_w, t_ref, 440.0)
    assert got in set(_refine_grid(440.0).tolist())
    # And it moves off the guess towards where the tone actually is.
    assert got > 440.0


def test_the_attack_band_measure_is_blind_to_how_loud_the_attack_was():
    """It is a tilt, which is what makes it usable on an RMS-normalised render."""
    sr = 48000
    t = np.arange(int(0.5 * sr)) / sr
    burst = np.sin(2 * np.pi * 10000.0 * t) * np.exp(-t * 40.0)
    quiet = attack_bands(burst * 0.01, sr, Note(60, 96, 0.0, 0.4), 0.0)
    loud = attack_bands(burst, sr, Note(60, 96, 0.0, 0.4), 0.0)
    pairs = [(a, b) for a, b in zip(quiet, loud) if a is not None and b is not None]
    assert pairs
    assert all(a == pytest.approx(b, abs=0.05) for a, b in pairs)


def test_the_attack_band_measure_sees_a_top_end_that_is_too_hot():
    sr = 48000
    t = np.arange(int(0.5 * sr)) / sr
    dull = np.sin(2 * np.pi * 500.0 * t)
    ticky = dull + 0.5 * np.sin(2 * np.pi * 18000.0 * t) * np.exp(-t * 60.0)
    a = attack_bands(dull, sr, Note(60, 96, 0.0, 0.4), 0.0)
    b = attack_bands(ticky, sr, Note(60, 96, 0.0, 0.4), 0.0)
    # Band index 3 is 16-20 kHz, first 20 ms slice.
    assert b[3] - a[3] > 20.0


def test_a_render_too_short_for_the_attack_window_contributes_nothing():
    sr = 48000
    rows = attack_bands(np.zeros(int(0.05 * sr)), sr, Note(60, 96, 0.0, 0.4), 0.0)
    assert rows[-1] is None


def _string(sr: int, b: float, n_partials: int = 12, f0: float = 55.0,
            step_db: float = 3.0):
    """An A1 whose partials are stretched by stiffness `b`, on a known ladder."""
    t = np.arange(int(3.0 * sr)) / sr
    y = np.zeros_like(t)
    for n in range(1, n_partials + 1):
        fn = n * f0 * np.sqrt(1.0 + b * n * n)
        y += 10 ** (-step_db * (n - 1) / 20.0) * np.sin(2 * np.pi * fn * t)
    return y, [-step_db * (n - 1) for n in range(1, n_partials + 1)]


def test_a_harmonic_voice_is_measured_exactly_as_it_was_before_stiffness():
    """The identity case. An organ or a brass voice must not move at all."""
    sr = 48000
    y, ladder = _string(sr, 0.0)
    m = analyze_note(y, sr, Note(33, 96, 0.0, 2.0), 3.0)
    assert m.inharmonicity_b == 0.0
    assert m.harmonics_db == pytest.approx(ladder, abs=0.05)


def test_a_stiff_string_is_measured_where_its_partials_actually_are():
    """Searching integer multiples read the twelfth 49 dB dark at B=4e-4.

    Not a small error and not a gradient: the partial is either inside the
    +-40 cent window or it is not, so a model and a reference whose stiffness
    differs slightly land on opposite sides of a cliff and the term that
    carries weight 1.0 by default reports tens of decibels that are not timbre.
    """
    sr = 48000
    for b in (0.0004, 0.0008, 0.0016):
        y, ladder = _string(sr, b)
        m = analyze_note(y, sr, Note(33, 96, 0.0, 2.0), 3.0)
        assert m.inharmonicity_b == pytest.approx(b, rel=0.15)
        worst = max(abs(a - e) for a, e in zip(m.harmonics_db, ladder))
        assert worst < 2.0, f"B={b} worst partial error {worst:.1f} dB"


def test_stiffness_is_read_from_the_signal_and_never_from_the_note_number():
    """A harmonic voice on a bass note must still come back at exactly zero.

    The estimate is clamped at zero rather than allowed to go negative, so the
    guard that matters is the one against inventing stiffness out of noise.
    """
    sr = 48000
    rng = np.random.default_rng(0)
    y, _ = _string(sr, 0.0)
    noisy = y + 0.01 * rng.standard_normal(len(y))
    assert analyze_note(noisy, sr, Note(33, 96, 0.0, 2.0), 3.0).inharmonicity_b == 0.0


def test_the_noise_term_does_not_file_stretched_partials_as_noise():
    """`tnr` is scored one-directionally, so a mistracked mask is not neutral.

    Only a model noisier than its oracle is charged. A mask on integer
    multiples buckets a stiff string's upper partials as noise, so whichever
    side is stiffer reads as the noisy one and the fit is sent to correct a
    property of the ruler.
    """
    sr = 48000
    rng = np.random.default_rng(0)
    y, _ = _string(sr, 0.0008, n_partials=30, step_db=1.2)
    y = y + 1e-3 * rng.standard_normal(len(y))
    stiff = analyze_note(y, sr, Note(33, 96, 0.0, 2.0), 3.0)
    harmonic, _ = _string(sr, 0.0, n_partials=30, step_db=1.2)
    harmonic = harmonic + 1e-3 * rng.standard_normal(len(harmonic))
    clean = analyze_note(harmonic, sr, Note(33, 96, 0.0, 2.0), 3.0)
    # Two renders of the same ladder, one stiff and one not, are both clean
    # signals: the stiff one must not report itself dozens of dB noisier.
    assert abs(stiff.tnr_db - clean.tnr_db) < 12.0


def _band_limited_low_excess(sr: int):
    """A note plus a 40 Hz excess with no onset click, so it stays in its band.

    The click matters: a hard-edged burst is genuinely broadband and every band
    should move for it. Testing the normaliser needs a defect that really is
    confined, or the test proves nothing about where the deltas came from.
    """
    t = np.arange(int(1.0 * sr)) / sr
    tone = sum(10 ** (-2.0 * (k - 1) / 20) * np.sin(2 * np.pi * 110 * k * t)
               for k in range(1, 25))
    env = np.where(t < 0.08,
                   0.5 * (1 - np.cos(np.pi * np.clip(t / 0.008, 0, 1))) * np.exp(-t * 40),
                   0.0)
    return tone, tone + 6.0 * np.sin(2 * np.pi * 40.0 * t) * env


def test_a_low_band_defect_does_not_fabricate_deltas_in_the_bands_above_it():
    """The anchor's whole job. A share of the window total does not do this.

    Measured the other way, the three bands from 200 Hz up each reported
    4.92 dB of difference from a defect that is entirely below 60 Hz.
    """
    sr = 48000
    clean, defective = _band_limited_low_excess(sr)
    note = Note(45, 96, 0.0, 1.0)
    a = attack_low_bands(clean, sr, note, 0.0)
    b = attack_low_bands(defective, sr, note, 0.0)
    assert b[0] - a[0] > 20.0                      # 20-60 Hz: the defect
    for i in (2, 3, 4):                            # 200 Hz and up: untouched
        assert a[i] == pytest.approx(b[i], abs=0.01)


def test_the_high_bands_keep_their_own_normaliser():
    """A 20 ms slice cannot resolve the frequencies the anchor excludes.

    The low measure's anchor is right at 50 ms and inverts at 20 ms, where a
    slice is shorter than one cycle of a 40 Hz excess and the leakage lands in
    the anchor band itself. Anchoring here moved this term's worst band 6.83 dB
    on the signal above, against 1.18 for the share of the total it keeps.
    """
    sr = 48000
    clean, defective = _band_limited_low_excess(sr)
    note = Note(45, 96, 0.0, 1.0)
    a = attack_bands(clean, sr, note, 0.0)
    b = attack_bands(defective, sr, note, 0.0)
    deltas = [abs(x - y) for x, y in zip(a, b) if x is not None and y is not None]
    assert deltas and max(deltas) < 3.0


def _velocity_probe(sr: int, brightness):
    """A velocity grid whose upper partials open up by `brightness(velocity)`."""
    pattern = build_pattern("velocity", 0, note=57)
    total = max(n.start + n.dur for n in pattern.notes) + 2.0
    y = np.zeros(int(total * sr))
    for n in pattern.notes:
        t = np.arange(int(n.dur * sr)) / sr
        f0 = 440.0 * 2.0 ** ((n.note - 69) / 12.0)
        rise = brightness(n.velocity)
        s = sum(10 ** ((-3.0 * (k - 1) + rise * (k - 1)) / 20.0)
                * np.sin(2 * np.pi * f0 * k * t) for k in range(1, 11))
        at = int(n.start * sr)
        y[at:at + len(t)] += s * np.exp(-t * 1.5)
    return pattern, y


def test_the_dynamics_term_sees_a_curve_the_harmonic_ladder_cannot():
    """The reason it exists: `harm` prices per-note error and averages it.

    Two candidates with the SAME per-note ladder error, one tracking the
    reference's response to force and one inverted. The harmonic term scores
    them identically — it has no way not to — and the fit would pick either.
    """
    sr = 48000
    ref = lambda v: (v - 40) / 87.0 * 5.0  # noqa: E731
    pattern, oracle = _velocity_probe(sr, ref)
    o = probe_rows(oracle, pattern, sr, raw=oracle)

    def terms(brightness):
        _, y = _velocity_probe(sr, brightness)
        return score_terms(probe_rows(y, pattern, sr, raw=y), o, n_harm=10)

    right = terms(lambda v: ref(v) + 0.6)
    wrong = terms(lambda v: ref(v) + 0.6 * np.sign(v - 83.5))
    # Near-identical rather than identical: a partial's vote is scaled by how
    # audible it is, and the two candidates put their error on opposite sides of
    # the reference, so the louder of the two levels differs slightly. What
    # matters is the scale of it — 5 % here against a sixfold separation below.
    assert right["harm"] == pytest.approx(wrong["harm"], rel=0.08)
    assert wrong["dyn"] > 6.0 * right["dyn"]
    assert right["dyn"] < 0.5
    assert wrong["dyn"] > 3.0
    assert right["dyn_groups"] == 1.0


def test_a_probe_with_no_velocity_axis_reports_that_it_measured_nothing():
    """Zero is this term's best score, so it must not be its unmeasurable one."""
    sr = 48000
    pattern = build_pattern("sustain", 0, notes=[45])
    t = np.arange(int(8.0 * sr)) / sr
    y = np.sin(2 * np.pi * 110 * t) + 0.5 * np.sin(2 * np.pi * 220 * t)
    rows = probe_rows(y, pattern, sr, raw=y)
    out = score_terms(rows, rows, n_harm=10)
    assert out["dyn"] == 0.0
    assert out["dyn_groups"] == 0.0


def test_the_attack_windows_follow_the_note_rather_than_the_score():
    """A model that speaks late must not read as a model with a different timbre."""
    sr = 48000
    t = np.arange(int(2.0 * sr)) / sr
    note = Note(45, 96, 0.1, 1.0)

    def voice(delay: float):
        tn = np.clip(t - (0.1 + delay), 0, None)
        live = (t >= 0.1 + delay).astype(float)
        click = 8.0 * np.exp(-tn * 300.0) * np.sin(2 * np.pi * 3000 * tn)
        body = np.sin(2 * np.pi * 110 * tn) + 0.4 * np.sin(2 * np.pi * 550 * tn)
        return (body + click) * live * np.exp(-tn * 4.0)

    prompt, late = voice(0.0), voice(0.030)
    assert note_onset(late, sr, note, 2.0) - note_onset(prompt, sr, note, 2.0) > 0.02

    def worst(anchor_prompt, anchor_late):
        a = attack_low_bands(prompt, sr, note, anchor_prompt)
        b = attack_low_bands(late, sr, note, anchor_late)
        return max(abs(x - y) for x, y in zip(a, b) if x is not None and y is not None)

    on_score = worst(note.start, note.start)
    on_onset = worst(note_onset(prompt, sr, note, 2.0), note_onset(late, sr, note, 2.0))
    assert on_onset < on_score / 2.0


def _thumped_note(sr: int):
    """An A2 whose attack carries a 40 Hz burst nothing radiates.

    The shape of the defect the low-band measure exists to catch: energy dumped
    below the instrument's output, confined to the attack, on a note whose
    harmonics are otherwise right. 40 Hz sits under the fundamental and under
    every harmonic of it, so the burst cannot show up in the ladder even in
    principle, and it is 25 dB down by the time the sustain window opens.
    """
    t = np.arange(int(1.0 * sr)) / sr
    tone = np.sin(2 * np.pi * 110.0 * t) + 0.5 * np.sin(2 * np.pi * 220.0 * t)
    thump = 3.0 * np.sin(2 * np.pi * 40.0 * t) * np.exp(-t * 60.0)
    return tone, tone + thump, Note(45, 96, 0.0, 0.8)


def test_the_attack_low_band_measure_is_blind_to_how_loud_the_attack_was():
    """A tilt, like the high-band measure, so it survives RMS normalisation."""
    sr = 48000
    _, thumped, note = _thumped_note(sr)
    quiet = attack_low_bands(thumped * 0.01, sr, note, 0.0)
    loud = attack_low_bands(thumped, sr, note, 0.0)
    pairs = [(a, b) for a, b in zip(quiet, loud) if a is not None and b is not None]
    assert pairs
    assert all(a == pytest.approx(b, abs=0.05) for a, b in pairs)


def test_the_attack_low_band_measure_sees_a_bass_attack_with_no_onset():
    sr = 48000
    clean, thumped, note = _thumped_note(sr)
    a = attack_low_bands(clean, sr, note, 0.0)
    b = attack_low_bands(thumped, sr, note, 0.0)
    # Band index 0 is 20-60 Hz, which is where the burst lives.
    assert b[0] - a[0] > 20.0


def test_the_harmonic_ladder_cannot_see_what_the_low_band_measure_catches():
    """The term is not a restatement of one already here.

    This is the reason the measure was added rather than a weight adjusted: the
    ladder is h1-normalised and reads the settled middle of the note, so a
    defect that is over before that window opens leaves it bit-for-bit
    unchanged. A term that moved with `harm` would buy nothing.
    """
    sr = 48000
    clean, thumped, note = _thumped_note(sr)
    before = analyze_note(clean, sr, note, 1.0)
    after = analyze_note(thumped, sr, note, 1.0)
    assert after.harmonics_db == before.harmonics_db
    assert after.tnr_db == pytest.approx(before.tnr_db, abs=0.05)
    # And the new measure does move on the same pair, so the pass above is the
    # ladder being blind rather than the signals being identical.
    assert attack_low_bands(thumped, sr, note, 0.0)[0] > attack_low_bands(clean, sr, note, 0.0)[0]


def test_a_render_too_short_for_the_low_band_window_contributes_nothing():
    sr = 48000
    rows = attack_low_bands(np.zeros(int(0.02 * sr)), sr, Note(60, 96, 0.0, 0.4), 0.0)
    assert rows == [None] * len(rows)
    # A silent render that IS long enough is also nothing, not a floor value:
    # two silences must not score as a perfect low-end match.
    assert attack_low_bands(np.zeros(sr), sr, Note(60, 96, 0.0, 0.4), 0.0) == rows


# --------------------------------------------------------------------------- #
# The range a search was allowed to visit
# --------------------------------------------------------------------------- #
def _bounded_knob(label, lo, hi, start) -> Knob:
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start, tunable=label)


def test_a_result_on_a_range_bound_is_named_rather_than_reported_as_an_optimum():
    """The most expensive failure this tool has, because nothing else looks wrong."""
    knobs = [_bounded_knob("kTrebleDecayOct", 0.5, 3.0, 1.9),
             _bounded_knob("kOther", 0.0, 1.0, 0.5)]
    pinned = autofit.report_pinned(knobs, [3.0, 0.5])
    assert len(pinned) == 1
    assert "kTrebleDecayOct" in pinned[0] and "maximum" in pinned[0]
    assert autofit.report_pinned(knobs, [1.9, 0.0])[0].endswith("at its minimum (0)")
    assert autofit.report_pinned(knobs, [1.9, 0.5]) == []


# --------------------------------------------------------------------------- #
# A spec that carries the weights its knobs answer to
# --------------------------------------------------------------------------- #
def _spec_file(tmp_path: Path, body) -> Path:
    path = tmp_path / "spec.json"
    path.write_text(json.dumps(body))
    return path


def test_a_bare_array_spec_still_loads(tmp_path):
    entry = {"tunable": "kX", "min": 0.0, "max": 1.0}
    path = _spec_file(tmp_path, [entry])
    assert load_spec(path) == [entry]
    assert load_spec_weights(path) == {}


def test_a_spec_can_carry_the_weights_its_knobs_answer_to(tmp_path):
    entry = {"tunable": "kX", "min": 0.0, "max": 1.0}
    path = _spec_file(tmp_path, {"weights": {"tail": 2.0, "crest": 2.0}, "knobs": [entry]})
    assert load_spec(path) == [entry]
    assert load_spec_weights(path) == {"tail": 2.0, "crest": 2.0}


def test_spec_weights_apply_but_never_over_an_explicit_flag(tmp_path):
    path = _spec_file(tmp_path, {"weights": {"tail": 2.0, "crest": 3.0}, "knobs": [
        {"tunable": "kX", "min": 0.0, "max": 1.0}]})
    # argparse has already put the flag's value on the namespace by this point;
    # what apply_spec_weights decides is whether the spec is allowed to replace it.
    args = _probe_args(spec=str(path), w_crest=0.5)
    autofit.apply_spec_weights(args, ["--w-crest", "0.5"])
    assert args.w_tail == 2.0
    assert args.w_crest == 0.5


def test_a_spec_naming_a_term_that_does_not_exist_is_refused(tmp_path):
    path = _spec_file(tmp_path, {"weights": {"loudness": 1.0}, "knobs": [
        {"tunable": "kX", "min": 0.0, "max": 1.0}]})
    with pytest.raises(ValueError, match="not a loss term"):
        autofit.apply_spec_weights(_probe_args(spec=str(path)), [])


# --------------------------------------------------------------------------- #
# The compare-table gate
# --------------------------------------------------------------------------- #
def test_the_summary_carries_the_absolute_median_the_signed_one_cannot_fail_on():
    """Errors of opposite sign in different registers cancel in the signed median."""
    summary = profile_module.summarize_deltas({"centroid_pct": [-40.0, -35.0, 35.0, 40.0]})
    assert summary["centroid_pct"]["median"] == pytest.approx(0.0)
    assert summary["centroid_pct"]["abs_median"] == pytest.approx(37.5)


def test_a_gate_fails_on_the_dimension_a_change_broke(tmp_path):
    gate = tmp_path / "gate.json"
    summary = profile_module.summarize_deltas({"centroid_pct": [1.0, 1.0], "decay": [0.2, 0.2]})
    assert profile_module.write_gate_file(summary, gate, "grand-227", 1.25) == 0
    assert profile_module.check_gate(summary, gate, "grand-227") == 0

    broke = profile_module.summarize_deltas({"centroid_pct": [60.0, 60.0], "decay": [0.2, 0.2]})
    assert profile_module.check_gate(broke, gate, "grand-227") == 1


def test_a_gate_recorded_against_another_reference_is_refused(tmp_path):
    gate = tmp_path / "gate.json"
    summary = profile_module.summarize_deltas({"decay": [0.2, 0.2]})
    profile_module.write_gate_file(summary, gate, "grand-227", 1.25)
    assert profile_module.check_gate(summary, gate, "grand-274") == 2


def test_a_gate_bound_has_a_floor_so_a_near_zero_dimension_stays_gateable(tmp_path):
    """Otherwise a dimension that happens to read zero today can never be met again."""
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(
        profile_module.summarize_deltas({"decay": [0.0, 0.0]}), gate, "grand-227", 1.25
    )
    assert json.loads(gate.read_text())["bounds"]["decay"]["median"] > 0.0


def test_a_knob_that_started_pinned_is_still_named_even_though_it_never_moved():
    """The case a start-to-best diff cannot show, and the one that has cost most.

    A spec whose range no longer contains the constant's default has that
    default clamped into range on load. The fit then reports it unchanged —
    because by the only measure the report had, nothing happened — while the
    value it actually searched around was never the compiled-in one.
    """
    knob = _bounded_knob("kTrebleDecayOct", 0.5, 3.0, 3.0)  # start clamped down from 5.0
    pinned = autofit.report_pinned([knob], [3.0])
    assert pinned and "maximum" in pinned[0]


def test_the_bound_test_is_proportional_to_the_range_not_an_absolute_epsilon():
    """A cutoff searched over [900, 2600] that lands on 2599.9 is pinned."""
    knob = _bounded_knob("kBridgeHillHz", 900.0, 2600.0, 1500.0)
    assert at_bound(knob, 2599.9) == "maximum"
    assert at_bound(knob, 2000.0) is None


def test_the_sustain_window_is_unchanged_on_every_existing_probe():
    """The cap sits exactly where the two-second probes' fractions already land."""
    sr = 48000
    t = np.arange(int(4.0 * sr)) / sr
    tone = np.sin(2 * np.pi * 440.0 * t) * np.exp(-t * 0.5)
    for dur in (2.0, 1.5, 0.12):
        note = Note(69, 96, 0.0, dur)
        a = int(min(0.3 * dur, metrics_module.SUSTAIN_WINDOW_S[0]) * sr)
        b = int(min(0.9 * dur, metrics_module.SUSTAIN_WINDOW_S[1]) * sr)
        assert (a, b) == (int(0.3 * dur * sr), int(0.9 * dur * sr))
        assert analyze_note(tone, sr, note, dur + 1.0).f0_hz > 0.0


def test_a_long_gate_does_not_put_the_sustain_window_after_the_note():
    """The defect an eight-second corpus probe introduced on the top two octaves.

    A fraction of the gate reads 2.4-7.2 s in, where a treble note has already
    stopped; the ladder then compares one render's floor with another's and
    reports a confident number no knob can move.
    """
    sr = 48000
    t = np.arange(int(10.0 * sr)) / sr
    # A treble note that is over well before the old window began.
    dead = (np.sin(2 * np.pi * 2093.0 * t) + 0.3 * np.sin(2 * np.pi * 4186.0 * t)) * np.exp(-t * 4.0)
    note = Note(96, 88, 0.0, 8.0)
    got = analyze_note(dead, sr, note, 10.0)
    # h2 is 10 dB under h1 in the signal; measured after the note it would be
    # the floor difference of two silences instead.
    assert got.harmonics_db[1] > -30.0


def test_an_aftersound_band_with_nothing_in_it_is_not_fitted():
    sr = 48000
    t = np.arange(int(8.0 * sr)) / sr
    over_by_one_second = np.sin(2 * np.pi * 2093.0 * t) * np.exp(-t * 12.0)
    sk = skeleton_note(over_by_one_second, sr, Note(96, 88, 0.0, 8.0))
    assert sk["tail_db_s"][0] is None
    assert sk["early_db_s"][0] is not None


def test_stiffness_is_priced_now_that_the_ladder_no_longer_prices_it():
    """Tracking the series made `harm` correct and left the series unscored.

    Before, a model stiffer than its reference showed up as tens of decibels of
    fabricated harmonic error — the wrong quantity in the wrong term, but a
    pressure in the right direction. `harm` must now be blind to it and `stiff`
    must not be.
    """
    sr = 48000
    soft, _ = _string(sr, 0.0003)
    stiff, _ = _string(sr, 0.0012)
    note = Note(33, 96, 0.0, 2.0)
    rows_soft = [analyze_note(soft, sr, note, 3.0).to_dict()]
    rows_stiff = [analyze_note(stiff, sr, note, 3.0).to_dict()]
    out = loss_terms(rows_stiff, rows_soft, n_harm=10)
    same = loss_terms(rows_stiff, rows_stiff, n_harm=10)
    # Stated against the harness's own floor rather than against a threshold:
    # comparing a render with itself still leaves a little L1, because a partial
    # is located to a bin and interpolated. What matters is that a fourfold
    # stiffness difference adds almost nothing on top of that — it used to add
    # tens of decibels — while the term that should carry it carries all of it.
    assert out["harm"] - same["harm"] < 5.0
    assert out["stiff"] > 20.0
    assert same["stiff"] == 0.0
    assert out["stiff_notes"] == 1.0


def test_two_harmonic_voices_agree_that_neither_stretches():
    """Zero here is a real match, not a missing measurement — unlike `dyn`."""
    sr = 48000
    a, _ = _string(sr, 0.0)
    note = Note(33, 96, 0.0, 2.0)
    rows = [analyze_note(a, sr, note, 3.0).to_dict()]
    out = loss_terms(rows, rows, n_harm=10)
    assert out["stiff"] == 0.0
    assert out["stiff_notes"] == 1.0


def test_a_stiffness_fit_with_too_few_partials_is_not_counted():
    """A B from four partials is a number rather than a measurement."""
    sr = 48000
    thin, _ = _string(sr, 0.0008, n_partials=3)
    note = Note(33, 96, 0.0, 2.0)
    rows = [analyze_note(thin, sr, note, 3.0).to_dict()]
    assert rows[0]["inharmonicity_partials"] < 6
    assert loss_terms(rows, rows, n_harm=10)["stiff_notes"] == 0.0


# --------------------------------------------------------------------------- #
# Attack peaks and the fixed-resonance attribution
# --------------------------------------------------------------------------- #
def _rung_note(sr: int, f0: float, ring_hz: float | None,
               ring_db: float = -12.0, b: float = 0.0, top_hz: float = 3600.0):
    """A struck string, optionally with a free resonance ringing over its attack.

    The ring decays inside the attack window and the partials do not, which is
    what a lightly-damped filter excited by a strike looks like and what the
    measure has to find without being told the frequency.

    The partial series stops below where `attack_peaks` starts looking, and a
    noise bed sits under the whole thing, for one reason each. A bare sum of
    sinusoids is silent between its partials, so every partial stands over a
    neighbourhood of numerical zero and reports a prominence no real render
    produces — which makes a synthetic comb a test of the signal rather than of
    the detector. Keeping the series out of the search band leaves the ring as
    the only thing in it, and the noise gives the baseline something real to be.
    """
    t = np.arange(int(2.0 * sr)) / sr
    y = np.zeros_like(t)
    for n in range(1, 40):
        fn = n * f0 * np.sqrt(1.0 + b * n * n)
        if fn >= top_hz:
            break
        y += 10 ** (-2.0 * (n - 1) / 20.0) * np.sin(2 * np.pi * fn * t)
    y *= np.exp(-1.5 * t)
    rng = np.random.default_rng(7)
    y += 1e-3 * rng.standard_normal(len(t)) * np.exp(-1.5 * t)
    if ring_hz is not None:
        y += (10 ** (ring_db / 20.0)) * np.sin(2 * np.pi * ring_hz * t) * np.exp(-30.0 * t)
    return y


def test_a_free_resonance_in_the_attack_is_found_and_named():
    """The measure exists to say which frequency, so it has to get it right."""
    sr = 48000
    note = Note(60, 100, 0.0, 1.0)
    y = _rung_note(sr, 261.6, ring_hz=9700.0)
    peaks = metrics_module.attack_peaks(y, sr, note, 0.0)
    assert peaks, "an 8 kHz-wide window with a 30 dB ring in it must find something"
    near = [f for f, _ in peaks if abs(f - 9700.0) < 60.0]
    assert near, f"9700 Hz ring not among {[round(f) for f, _ in peaks]}"


def test_a_window_the_render_cannot_fill_reports_no_peaks_rather_than_inventing_them():
    sr = 48000
    y = _rung_note(sr, 261.6, ring_hz=9700.0)[: int(0.05 * sr)]
    assert metrics_module.attack_peaks(y, sr, Note(60, 100, 0.0, 1.0), 0.0) == []


def test_a_partial_is_on_its_series_and_a_free_ring_is_not():
    """`partial_offset` is the whole basis of telling the two apart."""
    from metrics import MAX_EXTRAPOLATED_PARTIAL, partial_offset
    f0 = 261.6
    assert partial_offset(f0 * 10, f0, 0.0) == pytest.approx(0.0, abs=1e-6)
    assert partial_offset(f0 * 10.5, f0, 0.0) == pytest.approx(0.5, abs=1e-6)
    # Past where the stiffness fit can speak, the answer is unknown — not zero,
    # which would file every high frequency as an ordinary partial.
    assert partial_offset(f0 * (MAX_EXTRAPOLATED_PARTIAL + 5), f0, 0.0) is None
    assert partial_offset(1000.0, 0.0, 0.0) is None


#: Two notes an augmented fourth apart, which is the point: their partial
#: series share almost nothing, so a frequency can sit between the partials of
#: both at once. An octave pair cannot do that on a harmonic voice — see
#: `test_an_octave_pair_on_a_harmonic_voice_cannot_corroborate_a_ring`.
_RES_NOTES = ((60, 261.63), (66, 369.99))


def _between_partials(f0s, near_hz: float) -> float:
    """A frequency near `near_hz` that none of `f0s` can explain as a partial.

    Chosen rather than written down, because whether a given frequency is
    off-partial depends on every f0 in the probe at once and a hand-picked
    number silently stops being off-partial the moment a note changes.
    """
    from metrics import partial_offset
    best, best_off = near_hz, -1.0
    for hz in np.arange(near_hz - 400.0, near_hz + 400.0, 1.0):
        offs = [partial_offset(float(hz), f0, 0.0) for f0 in f0s]
        if any(o is None for o in offs):
            continue
        worst = min(offs)
        if worst > best_off:
            best, best_off = float(hz), worst
    assert best_off > 0.40, f"no off-partial frequency near {near_hz} for {f0s}"
    return best


def _resonance_rows(sr: int, notes, *, b: float = 0.0):
    """One row per (midi, f0, ring) triple, shaped as `probe_rows` shapes them."""
    rows = []
    for midi, f0, ring in notes:
        y = _rung_note(sr, f0, ring_hz=ring, b=b)
        note = Note(midi, 100, 0.0, 1.0)
        row = analyze_note(y, sr, note, 2.0).to_dict()
        row["attack_peaks"] = metrics_module.attack_peaks(y, sr, note, 0.0)
        rows.append(row)
    return rows


def test_a_ring_at_one_frequency_on_two_notes_is_reported_as_a_fixed_resonance():
    sr = 48000
    ring = _between_partials([f0 for _, f0 in _RES_NOTES], 9700.0)
    rows = _resonance_rows(sr, [(m, f0, ring) for m, f0 in _RES_NOTES])
    found = loss_module.fixed_resonances(rows)
    assert found, "a ring at the same place on both notes is the case this exists for"
    assert found[0]["hz"] == pytest.approx(ring, abs=80.0)
    assert found[0]["notes"] == [m for m, _ in _RES_NOTES]


def test_a_ring_on_only_one_note_is_not_called_fixed():
    """One note cannot tell a resonance from a coincidence in its own spectrum."""
    sr = 48000
    ring = _between_partials([f0 for _, f0 in _RES_NOTES], 9700.0)
    rows = _resonance_rows(sr, [(_RES_NOTES[0][0], _RES_NOTES[0][1], ring),
                                (_RES_NOTES[1][0], _RES_NOTES[1][1], None)])
    assert loss_module.fixed_resonances(rows) == []


def test_a_clean_string_reports_no_fixed_resonance_on_either_note():
    """The measure has to be silent on a voice that has nothing wrong with it.

    Recurrence on its own was not: run without the off-partial condition it
    reported fifteen resonances on a sampled reference that has none.
    """
    sr = 48000
    rows = _resonance_rows(sr, [(m, f0, None) for m, f0 in _RES_NOTES])
    assert loss_module.fixed_resonances(rows) == []


def test_a_peak_sitting_on_a_partial_is_not_a_free_resonance():
    """A driven partial is loud, and being loud is not the criterion."""
    sr = 48000
    rows = _resonance_rows(sr, [(m, f0, f0 * round(9700.0 / f0)) for m, f0 in _RES_NOTES])
    assert loss_module.fixed_resonances(rows) == []


def test_a_note_whose_stiffness_fit_was_too_thin_contributes_no_peaks():
    """Judging a peak against a guessed partial series is not a measurement."""
    sr = 48000
    ring = _between_partials([f0 for _, f0 in _RES_NOTES], 9700.0)
    rows = _resonance_rows(sr, [(m, f0, ring) for m, f0 in _RES_NOTES])
    for row in rows:
        row["inharmonicity_partials"] = 2
    assert loss_module.fixed_resonances(rows) == []


def test_an_octave_pair_on_a_harmonic_voice_cannot_corroborate_a_ring():
    """A documented blind spot, pinned so it is a known limit and not a surprise.

    On a voice with no stiffness the upper octave's partials are every second
    partial of the lower, so a frequency exactly midway between the lower note's
    partials is a quarter of the way between the upper note's — never off-partial
    for both at once. A piano escapes this because stiffness stretches the two
    series by different amounts; a harmonic voice probed in octaves does not,
    and there the measure reports nothing rather than guessing.
    """
    sr = 48000
    ring = 261.63 * 37.5           # exactly midway for C4, a quarter off for C5
    rows = _resonance_rows(sr, [(60, 261.63, ring), (72, 523.25, ring)])
    assert all(r["attack_peaks"] for r in rows), "both notes must have found the ring"
    assert loss_module.fixed_resonances(rows) == []


def test_probe_rows_carries_the_attack_peaks_it_measured():
    sr = 48000
    pattern = build_pattern("sustain", 0)
    y = np.zeros(int(pattern_length(pattern) * sr))
    for note in pattern.analysis_notes:
        seg = _rung_note(sr, 440.0 * 2 ** ((note.note - 69) / 12.0), ring_hz=9700.0)
        a = int(note.start * sr)
        n = min(len(seg), len(y) - a)
        y[a : a + n] += seg[:n]
    rows = probe_rows(y, pattern, sr)
    assert all("attack_peaks" in r for r in rows)
    assert any(r["attack_peaks"] for r in rows), "the ring is in every note's attack"


def test_a_zero_noise_term_says_whether_it_measured_anything():
    """`tnr` is one-sided, so its zero has two opposite meanings.

    Charged only where the model is noisier, it reads 0.00 both when every note
    matched and when the model is cleaner than the reference everywhere — and on
    a normalised objective the second is worth a full unit of loss to whichever
    candidate reaches it first. The count is what tells a reader which happened.
    """
    sr = 48000
    y, _ = _string(sr, 0.0)
    note = Note(33, 96, 0.0, 2.0)
    row = analyze_note(y, sr, note, 3.0).to_dict()
    same = loss_terms([row], [row], n_harm=10)
    assert same["tnr"] == 0.0 and same["tnr_notes"] == 0.0

    noisy = dict(row)
    noisy["tnr_db"] = row["tnr_db"] - 6.0
    charged = loss_terms([noisy], [row], n_harm=10)
    assert charged["tnr"] == pytest.approx(6.0, abs=0.01)
    assert charged["tnr_notes"] == 1.0

    cleaner = dict(row)
    cleaner["tnr_db"] = row["tnr_db"] + 6.0
    free = loss_terms([cleaner], [row], n_harm=10)
    assert free["tnr"] == 0.0, "a cleaner model is deliberately not penalised"
    assert free["tnr_notes"] == 0.0, "and the zero must not read as a match"


# --------------------------------------------------------------------------- #
# Proving the probe reached the code
# --------------------------------------------------------------------------- #
class _FakeEvaluator:
    """Scores a knob vector from a formula, so a probe's reach is decidable."""

    def __init__(self, response):
        self.response = response
        self.quiet = False

    def __call__(self, values):
        return self.response(values)

    def evaluate_batch(self, points):
        return [self.response(p) for p in points]


def _screen_args(threshold=0.001, max_evals=1000):
    return argparse.Namespace(screen_threshold=threshold, max_evals=max_evals)


def _reach_knob(label, lo, hi, start):
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start,
                tunable=f"file.{label}")


def test_a_screen_that_moves_nothing_is_an_error_rather_than_a_full_knob_list():
    """Zero is the signature of a probe that never reached the fields it swept.

    The fallback used to restore the whole list, so a spec that moved NOTHING
    and a spec that moved everything continued into the fit with the same knob
    count and the same one-line message.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]
    with pytest.raises(RuntimeError, match="0 of 2 knobs move the loss at all"):
        screen_knobs(_FakeEvaluator(lambda v: 1.0), knobs, _screen_args())


def test_a_screen_that_moves_something_still_narrows_to_it():
    """The inert-knob finding is unchanged; only the all-inert case is new."""
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]
    kept = screen_knobs(_FakeEvaluator(lambda v: v[0]), knobs, _screen_args())
    assert kept == [0]


def test_the_fit_refuses_a_library_that_ignores_the_override_table(monkeypatch):
    """Without BUILD_TUNING every candidate renders the compiled-in defaults.

    The fit then searches nothing and reports the start point as its winner,
    which nothing else about the run would show.
    """
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(ev, "_render_terms",
                        lambda values: {t: 1.0 for t in LOSS_TERMS}, raising=False)
    with pytest.raises(RuntimeError, match="not reaching the library"):
        ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.5)])


def test_the_reach_check_passes_when_the_override_moves_the_render(monkeypatch):
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    seen = []

    def render(values):
        seen.append(list(values))
        return {t: float(values[0]) for t in LOSS_TERMS}

    monkeypatch.setattr(ev, "_render_terms", render, raising=False)
    ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25)])
    # Base at the start values, then the far end of the range — the end further
    # from the start, so the probe has the whole interval behind it.
    assert seen == [[0.25], [1.0]]


def test_a_source_only_spec_has_no_override_plumbing_to_check(monkeypatch):
    """A rebuilding knob does not travel through the environment at all."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(ev, "_render_terms",
                        lambda values: pytest.fail("a source knob needs no reach check"),
                        raising=False)
    ev.check_overrides_reach([Knob(label="a", lo=0.0, hi=1.0, log=False,
                                   start_value=0.5, file=Path("x.cpp"), pattern="p")])


# --------------------------------------------------------------------------- #
# The resolved weight, not the flag
# --------------------------------------------------------------------------- #
def test_a_run_that_never_named_a_weight_still_knows_whether_it_needs_audio():
    """Every `--w-*` defaults to None so an unset weight stays distinguishable
    from an explicit zero, and the number it stands for comes from the
    instrument's class. Anything deciding on `args.w_mss` itself therefore
    compares None against a float, which is a TypeError rather than a weight -
    and the guard that renders twice to prove the overrides reach the library
    asks that question before the weights have been resolved, so every drum fit
    hit it.
    """
    base = {f"w_{t}": None for t in LOSS_TERMS}
    base.update(program=0, drum_note=42, percussive=True, raw_loss=False, workers=1)
    args = argparse.Namespace(**base)

    resolved = cli_weights(args)
    assert isinstance(resolved.get("mss", 0.0), float)

    ev = Evaluator([], {}, [], None, args, Path("build-none"))
    assert ev.want_audio is (resolved.get("mss", 0.0) > 0.0)
