"""The kit loss: the per-piece terms, the drum probe and its write-back.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_drums.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from _repo import REPO_ROOT
from autofit import resolve_probe
from autofit_test_fixtures import (
    _knob,
    _probe_args,
)
from catalogue import Catalogue
from knobs import MODE_RATIO_RANGE, Knob, _auto_range, auto_spec
from loss import cli_weights, percussion_terms, refused_weights
from patterns import build_pattern
from staging import stage_of
from writeback import DRUM_TABLE_FILE, patch_field_assignments, write_drum_fields


# --------------------------------------------------------------------------- #
# Drums
# --------------------------------------------------------------------------- #
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
    assert set(weights) == {"band", "bdecay", "tilt", "bright", "lf", "env", "modes", "crest"}
    assert not {"harm", "cents", "tnr", "init", "slope", "tail", "hf", "stiff", "mod"} & set(
        weights
    )
    # `band` is an L1 per band and carries no direction, so the lean of the
    # spectrum and where its energy sits are their own terms — the two the kit's
    # gate names and the two a whole-kit fit under `band` alone took backwards.
    assert weights["tilt"] == 1.0 and weights["bright"] == 1.0
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


def test_a_weight_this_metric_set_cannot_produce_is_named_rather_than_dropped():
    """`--w-hf 1` on a drum probe is accepted, echoed nowhere and changes nothing.

    `percussion_terms` never computes `hf`, so the weight multiplies a constant
    0.0 — the best score that term has — and two runs differing only in the flag
    came back byte-identical. Dropping it is right; dropping it in silence is
    how a round gets spent believing an axis was weighted.
    """
    drum = _probe_args(percussive=True, drum_note=35, w_hf=1.0, w_tail=0.5, w_band=2.0)
    assert refused_weights(drum) == ["tail", "hf"]
    assert "hf" not in cli_weights(drum) and cli_weights(drum)["band"] == 2.0
    # The same weight on the probe that does produce the term is not a finding.
    assert refused_weights(_probe_args(percussive=False, w_hf=1.0)) == []
    # Nor is a class default the probe cannot produce: only what was asked for.
    assert refused_weights(_probe_args(percussive=True, drum_note=35)) == []


def _hit(bands, decay, attack=1.0, decay_ms=200.0, crest=10.0) -> dict:
    return {
        "bands_db": bands,
        "band_decay_db_s": decay,
        "attack_ms": attack,
        "decay_ms": decay_ms,
        "crest_db": crest,
    }


def test_a_matching_hit_scores_zero_on_every_percussion_term():
    hit = _hit([0.0, -6.0, -12.0], [-20.0, -30.0])
    terms = percussion_terms([hit], [hit])
    assert terms["band"] == 0.0
    assert terms["bdecay"] == 0.0
    assert terms["env"] == 0.0


def test_the_lean_of_the_spectrum_is_scored_where_the_band_profile_cannot_see_it():
    """`band` charges a magnitude per band and never a direction.

    A whole-kit fit under it improved the profile from 16.4 to 15.5 dB while
    taking the tilt from 6.8 to 9.4 dB and the centroid from 39 % to 75 % over
    its reference — brightness being the one of eight gated dimensions that had
    been inside the reference kits' own spread. Two hits equidistant from the
    reference in profile terms, one dull and one bright, are the shape of that:
    `band` cannot tell them apart and `tilt` puts them on opposite sides.
    """
    from metrics import THIRD_OCTAVE_CENTERS

    def sloped(step: float) -> list[float]:
        return [0.0 if c < 2000.0 else step for c in THIRD_OCTAVE_CENTERS]

    ref = _hit(sloped(0.0), [-20.0], attack=1.0)
    bright, dull = _hit(sloped(+6.0), [-20.0]), _hit(sloped(-6.0), [-20.0])
    assert percussion_terms([bright], [ref])["band"] == percussion_terms([dull], [ref])["band"]
    assert percussion_terms([bright], [ref])["tilt"] == pytest.approx(6.0)
    assert percussion_terms([dull], [ref])["tilt"] == pytest.approx(6.0)
    # The direction survives where it is read: a hit that leans the same way as
    # the reference costs nothing however far both lean.
    both = _hit(sloped(+6.0), [-20.0])
    assert percussion_terms([both], [_hit(sloped(+6.0), [-20.0])])["tilt"] == 0.0
    # And the centroid is the gate's own ratio, absent when the reference has none.
    assert percussion_terms([{**bright, "centroid_hz": 4000.0}], [{**ref, "centroid_hz": 2000.0}])[
        "bright"
    ] == pytest.approx(100.0)
    assert percussion_terms([bright], [ref])["bright_hits"] == 0.0


def test_one_empty_band_cannot_decide_the_whole_objective():
    """Two noise floors differ by whatever they happen to be; the cap bounds it."""
    model = _hit([0.0, -60.0], [-20.0])
    oracle = _hit([0.0, -200.0], [-20.0])
    assert percussion_terms([model], [oracle])["band"] <= 24.0


def test_an_unfittable_band_decay_is_charged_not_counted_as_agreement():
    """A band only the REFERENCE has a rate for used to be skipped, and this test
    read the skip back as 0.0 — which is the term's best score, so the name and
    the assertion said opposite things. A rate the model does not produce is a
    disagreement; an absence on the reference's own side is still nobody's."""
    model = _hit([0.0], [None, -30.0])
    oracle = _hit([0.0], [-20.0, -30.0])
    assert percussion_terms([model], [oracle])["bdecay"] == pytest.approx(3.0)
    both_absent = _hit([0.0], [None, -30.0])
    assert percussion_terms([model], [both_absent])["bdecay"] == 0.0


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
        defaults={
            "d038.percussion.wire_buzz": 0.5,
            "d038.amp_env.decay_ms": 250.0,
            "percussion_voice.kPhisemCollisionRate": 100.0,
            "violin.bowed_string.bow_force": 0.55,
        },
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
    # `--program-only` leaves the engine behind. A fit over one drum note has
    # nothing that could object to a constant every percussion voice reads.
    alone = {e["tunable"] for e in auto_spec(0, cat, drum_note=38, patch_only=True)}
    assert alone == {"d038.percussion.wire_buzz", "d038.amp_env.decay_ms"}


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
        defaults={
            "d038.gain": 0.8,
            "d038.percussion.wire_buzz": 0.5,
            "violin.gain": 1.0,
            "violin.bowed_string.bow_force": 0.55,
        },
        programs={(40, 0): "violin"},
        bounds={},
    )
    assert "d038.gain" not in {e["tunable"] for e in auto_spec(0, cat, drum_note=38)}
    assert "violin.gain" not in {e["tunable"] for e in auto_spec(40, cat)}
    # And the exclusion is by field name, not by a substring of the path.
    assert "d038.percussion.wire_buzz" in {e["tunable"] for e in auto_spec(0, cat, drum_note=38)}


def test_a_modes_bessel_zero_is_not_offered_but_its_ratio_is():
    """One is the membrane's geometry; the other is how the head is tuned.

    `mode_alpha` scales the argument the strike weighting is evaluated at, and
    `percussion_voice.h` ties it to the ratios by construction. A fit given it
    uses it as a per-mode gain instead — measured on a kick that took alpha0 to
    eight times the first zero of J0 while dropping alpha1 underneath it.
    """
    cat = Catalogue(
        defaults={
            "d036.percussion.mode_alpha0": 2.4048,
            "d036.percussion.mode_alpha1": 3.8317,
            "d036.percussion.mode_ratios1": 1.59,
            "d036.percussion.mode_decay_s": 0.22,
        },
        programs={},
        bounds={},
    )
    offered = {e["tunable"] for e in auto_spec(0, cat, drum_note=36)}
    assert not any(k.startswith("d036.percussion.mode_alpha") for k in offered)
    assert "d036.percussion.mode_ratios1" in offered
    assert "d036.percussion.mode_decay_s" in offered


def test_the_first_modes_ratio_is_the_base_frequency_and_is_not_offered():
    """`base_freq_hz * mode_ratios[0]` is where the first mode sounds.

    Pinning only the first factor leaves the second one carrying it: 36 kit notes
    hold a fitted `mode_ratios[0]`, from 0.0251 to 41.0, which is the stated pitch
    moved five octaves either way.
    """
    cat = Catalogue(
        defaults={"d036.percussion.mode_ratios0": 1.0, "d036.percussion.mode_ratios1": 1.59},
        programs={},
        bounds={},
    )
    offered = {e["tunable"] for e in auto_spec(0, cat, drum_note=36)}
    assert "d036.percussion.mode_ratios0" not in offered
    assert "d036.percussion.mode_ratios1" in offered


def test_the_drums_dimensions_are_read_off_it_but_the_air_spring_is_fitted():
    """Metres of head and shell are what the drum is; the cavity's stiffness is not.

    The head diameter weights every m >= 1 mode by (ka)^m — monotone in frequency
    and capped at 1 — so a search reaches for it wherever the model is too bright
    and lands on a drum of whatever size that needed. `air_spring` has a
    geometric estimate, but one that runs 30-40% high against the two measured
    drums, so it is a scalar a fit lands rather than a number read off the shell.
    """
    cat = Catalogue(
        defaults={
            "d038.percussion.head_diameter_m": 0.36,
            "d038.percussion.shell_depth_m": 0.14,
            "d038.percussion.air_spring": 2.0,
            "d038.percussion.mallet_ms": 2.0,
        },
        programs={},
        bounds={"d038.percussion.air_spring": (0.0, 8.0), "d038.percussion.mallet_ms": (0.0, 50.0)},
    )
    offered = {e["tunable"] for e in auto_spec(0, cat, drum_note=38)}
    assert "d038.percussion.head_diameter_m" not in offered
    assert "d038.percussion.shell_depth_m" not in offered
    assert {"d038.percussion.air_spring", "d038.percussion.mallet_ms"} <= offered


def test_a_mode_ratio_is_searched_over_a_window_a_write_back_cannot_widen():
    """Around the default, and never outside what a struck head can put up there.

    The clamp accepts 0..64, so a ratio offered as a magnitude re-anchors on
    whatever the last round wrote and reaches further every time — which is how
    the table came to hold 44.9. A ratio already outside the window is re-searched
    over the whole of it rather than held there.
    """
    lo, hi, log = _auto_range("d036.percussion.mode_ratios1", 1.59, (0.0, 64.0))
    assert (round(lo, 4), round(hi, 4)) == (0.795, 3.18)
    assert log

    floor, ceiling = MODE_RATIO_RANGE
    for escaped in (44.9033, 0.0251234):
        assert _auto_range("d036.percussion.mode_ratios1", escaped, (0.0, 64.0)) == (
            floor,
            ceiling,
            True,
        )

    # A mode switched off is off: a range would turn a silent slot into a partial.
    assert _auto_range("d036.percussion.mode_ratios3", 0.0, (0.0, 64.0)) is None


def test_the_pitch_a_drum_is_built_with_is_not_offered_but_its_voicing_is():
    """`base_freq_hz` and `shell_freq_hz` say what the instrument is, not how it sounds.

    The drum table sets both where each patch is built, from the spec or from a
    measurement, and the C++ voice tests assert several of them by frequency.
    Offered to a search they become a free spectral-shaping parameter: a
    whole-kit fit moved 34, taking the low timbale's head from 200 Hz to 6682
    and the open hi-hat's from 315 to 16 while every kit-level dimension
    improved, because a different object can match the same band profile.
    """
    cat = Catalogue(
        defaults={
            "d066.percussion.base_freq_hz": 200.0,
            "d066.percussion.shell_freq_hz0": 180.0,
            "d066.percussion.mode_ratios1": 1.59,
            "d066.percussion.mode_decay_s": 0.22,
        },
        programs={},
        bounds={},
    )
    offered = {e["tunable"] for e in auto_spec(0, cat, drum_note=66)}
    assert "d066.percussion.base_freq_hz" not in offered
    assert "d066.percussion.shell_freq_hz0" not in offered
    assert {"d066.percussion.mode_ratios1", "d066.percussion.mode_decay_s"} <= offered


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


def test_a_count_is_written_as_an_integer_and_a_switch_is_not_written_at_all():
    """The struct has `bool`, `int` and enum members among its floats, and a
    float literal spliced into one of those does not compile: the kick's fit
    emitted `t[35].one_shot = 0.884586f;` and five -Werror errors with it. Which
    is which comes from the override layer's own `I` / `I_TYPED` declarations."""

    def at(label: str, start: float) -> Knob:
        return Knob(label=label, lo=0.0, hi=8.0, log=False, start_value=start, tunable=label)

    knobs = [
        at("d035.percussion.num_modes", 2.0),  # a count
        at("d035.percussion.noise_output", 0.0),  # an enum: its type is its range
        at("d035.one_shot", 1.0),  # a bool, and the fit's 0.885 is 1
        at("d035.percussion.shell_num_modes", 1.0),
    ]
    per_patch, per_drum, other = patch_field_assignments(knobs, [3.94, 0.717, 0.885, 1.18])
    assert per_patch == {}
    # The count rounds as `std::lround` did when it rendered; the two switches are
    # reported for a human to place; the second rounds back to where it started.
    assert per_drum == {35: [("percussion.num_modes", 4.0)]}
    assert other == ["d035.percussion.noise_output", "d035.one_shot"]
    text = next(iter(write_drum_fields(per_drum).values()))
    assert "t[35].percussion.num_modes = 4;" in text
    assert "num_modes = 4.0f" not in text


def test_a_note_assigned_in_a_chain_still_finds_its_anchor():
    """`t[41] = t[43] = ... = d.tom;` is one statement; appending after it is correct."""
    edited = write_drum_fields({43: [("percussion.tone_gain", 0.7)]})
    text = next(iter(edited.values()))
    assert "t[43].percussion.tone_gain = 0.7f;" in text


def test_a_note_whose_table_line_wraps_is_written_after_the_whole_statement():
    """The cowbell's `make_metal(...)` wraps, and its comment carries a paren.

    Appending after the matched line put the fitted block inside the argument
    list. That is not a subtle failure and it does not stop at the note that
    caused it: the tree stopped compiling, and each of the twenty-six drum notes
    after it failed at the rebuild its own fit begins with.
    """
    table = (REPO_ROOT / DRUM_TABLE_FILE).resolve()
    # Synthetic rather than the real table, whose t[56] block is whatever the
    # last fit left there.
    before = (
        "  t[56] = make_metal(587.0f, {1.0f, 1.44f}, 2, 0.25f,\n"
        "                     0.5f);  // Cowbell (587/845 Hz)\n"
        "  t[67] = make_metal(1200.0f, {1.0f, 2.7f}, 2, 0.25f, 0.45f);\n"
    )
    text = write_drum_fields({56: [("percussion.tone_gain", 0.7)]}, {table: before})[table]
    lines = text.splitlines()
    written = next(i for i, ln in enumerate(lines) if "t[56].percussion.tone_gain" in ln)
    # The call is still one statement and the closing line keeps its comment.
    assert lines[written - 1].strip() == "0.5f);  // Cowbell (587/845 Hz)"
    assert lines[written + 1].strip().startswith("t[67] =")


def test_a_labelled_assignment_is_replaced_rather_than_shadowed():
    """A trailing comment made the line invisible, so a second one was appended.

    The second assignment wins silently, which leaves the label — and whatever
    comment stands above it — describing a value nothing uses. Four of the drum
    table's cymbals were carrying such a pair, one of them a `// at the clamp`
    note on a dead line.
    """
    table = (REPO_ROOT / DRUM_TABLE_FILE).resolve()
    before = "  t[49] = make_cymbal(3600.0f, 1.10f);\n  t[49].gain = 1.2698f;  // Crash 1\n"
    text = write_drum_fields({49: [("gain", 0.6029)]}, {table: before})[table]
    assert text.count("t[49].gain") == 1
    assert "t[49].gain = 0.6029f;  // Crash 1" in text
