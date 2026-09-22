"""What the metrics read off a rendered note: level, crest, the aftersound
band, the attack's high end, the compare table and the fixed-resonance
attribution.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_metrics.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json
import profile as profile_module

import autofit
import loss as loss_module
import metrics as metrics_module
from autofit_test_fixtures import _bounded_knob
from knobs import at_bound
from loss import (
    _refine_grid,
    _refine_partial,
    _refine_partial_direct,
    loss_terms,
    probe_rows,
    score_terms,
    skeleton_note,
)
from metrics import analyze_note, attack_bands, attack_low_bands, level_of, note_onset
from patterns import build_pattern, pattern_length
from smf import Note


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


def _string(sr: int, b: float, n_partials: int = 12, f0: float = 55.0, step_db: float = 3.0):
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
    tone = sum(10 ** (-2.0 * (k - 1) / 20) * np.sin(2 * np.pi * 110 * k * t) for k in range(1, 25))
    env = np.where(
        t < 0.08, 0.5 * (1 - np.cos(np.pi * np.clip(t / 0.008, 0, 1))) * np.exp(-t * 40), 0.0
    )
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
    assert b[0] - a[0] > 20.0  # 20-60 Hz: the defect
    for i in (2, 3, 4):  # 200 Hz and up: untouched
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
        s = sum(
            10 ** ((-3.0 * (k - 1) + rise * (k - 1)) / 20.0) * np.sin(2 * np.pi * f0 * k * t)
            for k in range(1, 11)
        )
        at = int(n.start * sr)
        y[at : at + len(t)] += s * np.exp(-t * 1.5)
    return pattern, y


def test_the_dynamics_term_sees_a_curve_the_harmonic_ladder_cannot():
    """The reason it exists: `harm` prices per-note error and averages it.

    Two candidates with the SAME per-note ladder error, one tracking the
    reference's response to force and one inverted. The harmonic term scores
    them identically — it has no way not to — and the fit would pick either.
    """
    sr = 48000
    ref = lambda v: (v - 40) / 87.0 * 5.0
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
    dead = (np.sin(2 * np.pi * 2093.0 * t) + 0.3 * np.sin(2 * np.pi * 4186.0 * t)) * np.exp(
        -t * 4.0
    )
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
def _rung_note(
    sr: int,
    f0: float,
    ring_hz: float | None,
    ring_db: float = -12.0,
    b: float = 0.0,
    top_hz: float = 3600.0,
):
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
    rows = _resonance_rows(
        sr, [(_RES_NOTES[0][0], _RES_NOTES[0][1], ring), (_RES_NOTES[1][0], _RES_NOTES[1][1], None)]
    )
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
    ring = 261.63 * 37.5  # exactly midway for C4, a quarter off for C5
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


@pytest.mark.parametrize("percussive", [False, True])
def test_measuring_the_notes_at_once_measures_the_same_notes(percussive):
    """Threads are allowed to change the wall clock and nothing else.

    Both metric sets, because they are two separate loops over the probe and a
    change that threads one of them reads as covered by a test of the other.
    """
    sr = 48000
    pattern = build_pattern("drum" if percussive else "sustain", 0)
    y = np.zeros(int(pattern_length(pattern) * sr))
    for note in pattern.analysis_notes:
        seg = _rung_note(sr, 440.0 * 2 ** ((note.note - 69) / 12.0), ring_hz=9700.0)
        a = int(note.start * sr)
        n = min(len(seg), len(y) - a)
        y[a : a + n] += seg[:n]
    serial = probe_rows(y, pattern, sr, raw=y)
    threaded = probe_rows(y, pattern, sr, raw=y, threads=4)
    assert len(serial) > 1, "one note cannot tell an ordering mistake from a correct one"
    assert threaded == serial


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
