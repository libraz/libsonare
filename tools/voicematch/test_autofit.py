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
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from autofit import resolve_probe  # noqa: E402
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
)
from patterns import PATTERN_BUILDERS, build_pattern  # noqa: E402
from staging import stage_of, staged_indices  # noqa: E402
from writeback import (  # noqa: E402
    key_to_member_path,
    override_patch_names,
    patch_field_assignments,
    write_drum_fields,
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
    """A Namespace carrying only what `resolve_probe` and `cli_weights` read."""
    base = dict(
        program=0, drum_note=None, pattern="sustain", notes="", velocities="",
        w_harm=1.0, w_cents=0.5, w_tnr=1.0, w_env=None, w_init=0.0, w_slope=0.0,
        w_mss=0.0, w_band=1.0, w_bdecay=1.0,
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
    with pytest.raises(ValueError, match="no analyzable notes"):
        resolve_probe(_probe_args(pattern="scale"))


def test_a_pattern_with_no_analysis_notes_accepts_a_whole_timeline_objective():
    args = _probe_args(pattern="scale", w_harm=0.0, w_cents=0.0, w_tnr=0.0, w_mss=1.0)
    resolve_probe(args)  # does not raise
    assert args.percussive is False


def test_a_drum_fit_weights_the_percussion_terms_not_the_harmonic_ones():
    args = _probe_args(drum_note=38)
    resolve_probe(args)
    weights = cli_weights(args)
    assert set(weights) == {"band", "bdecay", "env", "mss"}
    # The envelope is most of what tells two drums apart, so it is on by default
    # here and off for a sustained voice.
    assert weights["env"] == 1.0
    assert cli_weights(_probe_args())["env"] == 0.0


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
