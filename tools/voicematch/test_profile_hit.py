"""How a struck note is measured: the band comparisons, the ring length, the
attack floor, and where the onset is placed when the host sounded late.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_profile_hit.py -q
"""

from __future__ import annotations

import dataclasses
import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

import metrics as metrics_module
import profile_percussion
from profile_test_fixtures import (
    CAPTURE_DIR,
    REFERENCE_DIR,
    SR,
    _hit_row,
    shipped_captures,
)
from toneclass import PERCUSSION_DIMENSIONS, canonical_dimensions


def test_the_band_comparisons_report_a_direction_and_a_magnitude_separately():
    """Tilt says which way a hit is wrong; shape says how much that failed to explain."""
    flat = [0.0] * len(metrics_module.THIRD_OCTAVE_CENTERS)
    assert metrics_module.band_tilt_db(flat) == pytest.approx(0.0)
    assert profile_module.band_shape_error_db(flat, flat) == pytest.approx(0.0)

    bright = [
        0.0 if c >= metrics_module.TILT_HIGH_HZ else -12.0
        for c in metrics_module.THIRD_OCTAVE_CENTERS
    ]
    assert metrics_module.band_tilt_db(bright) == pytest.approx(12.0)
    # Same tilt, different spectrum: a resonance in the wrong band with a hole
    # beside it cancels out of the tilt and has to survive in the magnitude.
    lumpy = list(flat)
    lumpy[3], lumpy[4] = 9.0, -9.0
    assert metrics_module.band_tilt_db(lumpy) == pytest.approx(0.0, abs=1e-9)
    assert profile_module.band_shape_error_db(lumpy, flat) > 2.0

    assert metrics_module.band_tilt_db(None) is None
    assert profile_module.band_shape_error_db([], [0.0]) is None


def test_a_band_that_decayed_on_only_one_side_is_left_out_of_the_decay_average():
    """`analyze_hit` reports None for a band with no energy; that is not agreement."""
    assert profile_module.mean_band_decay_delta(
        [1.0, None, 3.0], [0.0, 2.0, None]
    ) == pytest.approx(1.0)
    assert profile_module.mean_band_decay_delta([None], [1.0]) is None


def test_the_decay_average_reports_how_many_octaves_it_was_read_over():
    """A row averaged over two octaves and one averaged over seven look alike.

    The average is a per-row number and carries no count, so a model that stopped
    resolving the top of its spectrum contributes a figure drawn from the bottom
    of it and reads like any other row. Both counts are against the REFERENCE's
    cells: what the model resolved where the reference did not is not evidence
    about a band the reference has nothing to say about.
    """
    reach = profile_percussion.band_decay_reach
    assert reach(
        {"band_decay_db_s": [-10.0, -20.0, None]}, {"band_decay_db_s": [-11.0, -21.0, -31.0]}
    ) == (2, 3)
    # The model resolving MORE than the reference does not raise either number.
    assert reach(
        {"band_decay_db_s": [-10.0, -20.0, -30.0]}, {"band_decay_db_s": [-11.0, None, None]}
    ) == (1, 1)
    assert reach({"band_decay_db_s": []}, {"band_decay_db_s": []}) == (0, 0)


def test_ring_length_is_measured_in_doublings_and_refuses_a_capped_reading():
    """A capped `decay_ms` is the analysis window, the way a capped damper is.

    In doublings because the kit spans 24x on this quantity, so a median taken
    in milliseconds is the cymbals and nothing else.
    """
    ring = profile_percussion.ring_doublings
    assert ring({"decay_ms": 400.0}, {"decay_ms": 200.0}) == pytest.approx(1.0)
    assert ring({"decay_ms": 100.0}, {"decay_ms": 200.0}) == pytest.approx(-1.0)
    # Symmetric, which is the whole reason for the unit: a percent would price
    # the doubling at +100 and the halving at -50.
    assert ring({"decay_ms": 400.0}, {"decay_ms": 200.0}) == pytest.approx(
        -ring({"decay_ms": 100.0}, {"decay_ms": 200.0})
    )
    assert ring({"decay_ms": 400.0, "decay_capped": True}, {"decay_ms": 200.0}) is None
    assert ring({"decay_ms": 400.0}, {"decay_ms": 200.0, "decay_capped": True}) is None
    assert ring({"decay_ms": 0.0}, {"decay_ms": 200.0}) is None
    assert ring({}, {"decay_ms": 200.0}) is None


def test_a_hit_is_compared_on_tonality_and_image_as_well_as_on_its_spectrum():
    """The three qualities no band profile can carry, and their one-sided case.

    `None` on either side is an absence rather than a flat spectrum or a centred
    source, so the column drops the row instead of charging it a number invented
    from a missing reading.
    """
    deltas = profile_percussion.percussion_row_deltas(
        _hit_row(flatness_db=-12.0, stereo_width=0.7, decay_ms=400.0), _hit_row()
    )
    assert deltas["tonality"] == pytest.approx(8.0)
    assert deltas["stereo"] == pytest.approx(0.3)
    assert deltas["ring"] == pytest.approx(1.0)
    for field in ("flatness_db", "stereo_width"):
        one_sided = profile_percussion.percussion_row_deltas(_hit_row(**{field: None}), _hit_row())
        assert one_sided["tonality" if field == "flatness_db" else "stereo"] is None


def test_every_dimension_a_kit_names_is_one_the_percussion_set_produces():
    """A named dimension nothing measures reads as a column that was fine.

    `select_dimensions` prints it once on the run that notices and the gate holds
    nothing there, which is the same silence as a dimension that was never named
    at all. The canonical set is the list that says what the words are allowed to
    be, on both sides: a capture excusing one it could not have had asserts a
    measurement that was never available to fail.

    Percussion captures only, which is the class this list belongs to. The
    melodic classes are not held here because two of their exclusions are
    deliberately broader than their own canonical set — a sustained voice's
    capture may argue `decay` away in its own words although the class already
    drops it — and one of them is an open question rather than a slip.
    """
    wrong = []
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        if not profile_module.is_percussion(cfg):
            continue
        canon = set(canonical_dimensions(int(cfg.get("program", 0)), percussive=True))
        for key in ("dimensions", "dimensions_na"):
            for dim in cfg.get(key) or []:
                if dim not in canon:
                    wrong.append(f"{name}.{key}: {dim!r} is not one of this class's")
    assert wrong == []


def test_a_kit_is_judged_on_every_percussion_dimension_or_told_why_not():
    """The drum capture is the one percussion capture, so its list IS the claim.

    A dimension in neither list is a gap, which `status.py` reports and nothing
    fails on — legitimate while a measurement is being built and indefensible
    once one exists. This kit has none, and that is worth holding: the three
    added last are exactly the qualities the spectral columns cannot carry, and
    losing one back into silence is how the set got to eight in the first place.
    """
    cfg = json.loads((CAPTURE_DIR / "drums.json").read_text())
    named = set(cfg["dimensions"]) | set(cfg["dimensions_na"])
    assert named == set(PERCUSSION_DIMENSIONS)
    assert not (set(cfg["dimensions"]) & set(cfg["dimensions_na"])), (
        "a bound recorded from a measurement the same file calls invalid asserts both"
    )
    assert all(cfg["dimensions_na"].values()), "an excuse with no reason excuses nothing"


def _attack_of(sig, sr=48000):
    """The attack rule `analyze_hit` applies, on a bare signal."""
    t, env = metrics_module._rms_envelope(
        sig,
        sr,
        hop_ms=metrics_module.HIT_ENVELOPE_HOP_MS,
        win_ms=metrics_module.HIT_ENVELOPE_WIN_MS,
    )
    peak = float(np.max(env))
    reached = np.where(env >= peak * 10.0 ** (metrics_module.HIT_ATTACK_TOLERANCE_DB / 20.0))[0]
    return float(t[int(reached[0]) if reached.size else int(np.argmax(env))] * 1000.0)


def test_the_attack_metric_has_a_floor_and_it_is_where_the_constant_says():
    """Anything faster than half the envelope window measures the ruler.

    An RMS window half full of a step is already 3.01 dB down, which is the
    tolerance the attack is read at, so a step, an impulse and a ramp inside the
    floor cannot be told apart. More than half the drum kit's model rows sit
    here, and a delta taken from one is a lower bound on the gap rather than a
    measurement of it.
    """
    sr = 48000
    noise = np.random.default_rng(0).standard_normal(int(0.3 * sr))
    impulse = np.zeros(int(0.3 * sr))
    impulse[0] = 1.0
    assert _attack_of(noise) == pytest.approx(metrics_module.ATTACK_FLOOR_MS)
    assert _attack_of(impulse) == pytest.approx(metrics_module.ATTACK_FLOOR_MS)
    inside = noise.copy()
    k = int(sr * metrics_module.ATTACK_FLOOR_MS / 2000.0)
    inside[:k] *= np.linspace(0.0, 1.0, k)
    assert _attack_of(inside) == pytest.approx(metrics_module.ATTACK_FLOOR_MS)

    # Above the floor it separates rise times again, and reads low rather than
    # high -- which is the direction that keeps a floored model honest.
    slow = noise.copy()
    k = int(sr * 0.030)
    slow[:k] *= np.linspace(0.0, 1.0, k)
    measured = _attack_of(slow)
    assert metrics_module.ATTACK_FLOOR_MS < measured < 30.0


def test_a_hit_records_whether_its_attack_was_floored():
    """The counterpart of `decay_capped`, and it has to be in the row to be read."""
    sr = 48000
    noise = np.random.default_rng(1).standard_normal(int(0.3 * sr))
    slow = noise.copy()
    k = int(sr * 0.030)
    slow[:k] *= np.linspace(0.0, 1.0, k)
    assert _attack_of(noise) <= metrics_module.ATTACK_FLOOR_MS
    assert _attack_of(slow) > metrics_module.ATTACK_FLOOR_MS
    assert "attack_floored" in {f.name for f in dataclasses.fields(metrics_module.HitMetrics)}


def test_the_model_grid_is_not_measured_into_the_reference_it_is_measured_against():
    """`render-grid` writes into the same corpus; a profile is the target half of it."""
    shipped = [
        t["id"]
        for t in json.loads((REFERENCE_DIR / "drums.json").read_text())["capture"]["timbres"]
    ]
    measured = {r["timbre"] for r in json.loads((REFERENCE_DIR / "drums.json").read_text())["rows"]}
    assert measured <= set(shipped)
    assert "model" not in measured


def _decaying_burst(sr: int, seconds: float, tau_s: float, seed: int = 0) -> np.ndarray:
    burst = np.random.default_rng(seed).normal(0, 0.2, int(sr * seconds))
    return burst * np.exp(-np.arange(len(burst)) / (tau_s * sr))


def test_a_hit_the_host_sounded_late_measures_the_same_as_one_it_sounded_on_time():
    """The window follows the strike, because a hosted plugin's does not follow the note-on.

    Measured on a sampled kit: the same key at six velocities started anywhere
    from 0 to 750 ms after its note-on. Anchoring on the note-on charges that
    latency to the instrument — time to peak comes back as the delay itself, and
    the leading silence dilutes the RMS that crest and level are read against.
    """
    sr = SR
    burst = _decaying_burst(sr, 0.4, 0.05)
    preroll = np.zeros(int(0.1 * sr))
    on_time = np.concatenate([preroll, burst])
    late = np.concatenate([preroll, np.zeros(int(0.25 * sr)), burst])

    a = profile_module.measure_hit(on_time, sr, 38, 100, preroll_s=0.1, gate_s=0.05)
    b = profile_module.measure_hit(late, sr, 38, 100, preroll_s=0.1, gate_s=0.05)

    assert a["onset_ms"] == pytest.approx(0.0, abs=2.0)
    assert b["onset_ms"] == pytest.approx(250.0, abs=2.0)
    assert b["attack_ms"] == pytest.approx(a["attack_ms"], abs=1.0)
    assert b["crest_db"] == pytest.approx(a["crest_db"], abs=0.5)
    assert b["level_db"] == pytest.approx(a["level_db"], abs=0.5)
    assert b["decay_ms"] == pytest.approx(a["decay_ms"], abs=2.0)


def test_a_wash_does_not_move_its_attack_when_ripple_moves_its_loudest_frame():
    """Time to the peak is not a statistic on a cymbal; time to arrival is.

    A crash holds within a couple of dB of its maximum for hundreds of
    milliseconds, so which frame carries the maximum is decided by noise. Two
    renders of the same gesture, differing only in where that ripple puts the
    maximum, have to report the same attack.
    """
    sr = SR
    n = int(sr * 1.2)
    wash = np.random.default_rng(2).normal(0, 0.2, n)
    wash *= np.minimum(1.0, np.arange(n) / (0.008 * sr))  # 8 ms strike
    wash *= np.exp(-np.arange(n) / (2.0 * sr))  # then a long plateau

    def bump_at(seconds: float) -> np.ndarray:
        lift = np.ones(n)
        i = int(seconds * sr)
        lift[i : i + int(0.02 * sr)] = 1.15
        return np.concatenate([np.zeros(int(0.1 * sr)), wash * lift])

    early = profile_module.measure_hit(bump_at(0.02), sr, 49, 100, preroll_s=0.1, gate_s=0.05)
    late = profile_module.measure_hit(bump_at(0.40), sr, 49, 100, preroll_s=0.1, gate_s=0.05)

    assert late["attack_ms"] == pytest.approx(early["attack_ms"], abs=5.0)
    assert early["attack_ms"] < 30.0


def test_a_momentary_dip_does_not_end_a_ring_that_is_still_going():
    """Decay is the last moment above the threshold, not the first moment under it.

    A 2 ms window on a noise wash crosses -20 dB and comes straight back. Read
    as a first crossing, a hi-hat that rings for half a second reports 14 ms.
    """
    sr = SR
    n = int(sr * 1.0)
    ring = np.random.default_rng(3).normal(0, 0.2, n)
    ring *= np.exp(-np.arange(n) / (0.15 * sr))
    ring[int(0.05 * sr) : int(0.052 * sr)] *= 0.001  # one dropout frame
    audio = np.concatenate([np.zeros(int(0.1 * sr)), ring])

    row = profile_module.measure_hit(audio, sr, 46, 100, preroll_s=0.1, gate_s=0.05)

    # exp(-t/0.15) is 20 dB down — a tenth of the amplitude — at 0.15 * ln(10).
    assert row["decay_ms"] == pytest.approx(345.0, abs=30.0)


def test_a_hit_that_swells_keeps_its_onset_rather_than_being_cut_to_its_peak():
    """A crash and a vibraslap peak well after the strike; that is the instrument."""
    sr = SR
    swell = np.random.default_rng(1).normal(0, 0.2, int(sr * 0.9))
    ramp = np.minimum(1.0, np.arange(len(swell)) / (0.3 * sr))
    swell *= ramp * np.exp(-np.arange(len(swell)) / (0.6 * sr))
    audio = np.concatenate([np.zeros(int(0.1 * sr)), swell])

    row = profile_module.measure_hit(audio, sr, 58, 100, preroll_s=0.1, gate_s=0.05)

    assert row["onset_ms"] == pytest.approx(0.0, abs=5.0)
    assert row["attack_ms"] > 100.0


def test_measure_hit_reports_a_strike_and_not_a_fundamental():
    """The pitched columns are absent rather than present and meaningless."""
    sr = SR
    noise = np.random.default_rng(0).normal(0, 0.2, int(sr * 0.4))
    noise *= np.exp(-np.arange(len(noise)) / (0.05 * sr))
    audio = np.concatenate([np.zeros(int(0.1 * sr)), noise])

    row = profile_module.measure_hit(audio, sr, 38, 100, preroll_s=0.1, gate_s=0.05)

    assert row["peak_dbfs"] is not None and row["peak_dbfs"] < 0.0
    assert row["bands_db"] and max(row["bands_db"]) == pytest.approx(0.0)
    for pitched in ("f0_hz", "cents_vs_et", "inharmonicity_b", "partials_db"):
        assert pitched not in row


def test_the_two_captures_with_references_name_their_program_and_phrase_set():
    """These two fields are what stop an instrument being measured as another.

    Named rather than globbed, because the regression is a specific pairing:
    the harpsichord was measured against program 0 for as long as the program
    was a literal in `profile.py`.
    """
    from capture import load_config

    here = Path(__file__).resolve().parent
    for name, program, takes in (("piano", 0, "piano"), ("harpsichord", 6, "harpsichord")):
        cfg = load_config(here / "capture" / f"{name}.json")
        assert cfg["program"] == program
        assert cfg["takes"] == takes


def test_every_shipped_capture_names_the_program_it_answers_with():
    """Read from the file, not from `load_config`, which defaults it to 0.

    A capture that leaves the program out is not measured against nothing, it
    is measured against the piano — which is a plausible profile of the wrong
    instrument rather than a failure anyone would notice.
    """
    here = Path(__file__).resolve().parent / "capture"
    for name in shipped_captures():
        raw = json.loads((here / f"{name}.json").read_text())
        assert isinstance(raw.get("program"), int), f"{name} does not name its GM program"


def test_a_capture_naming_a_phrase_set_names_one_that_exists():
    """A typo here is otherwise found by rendering the whole audition first."""
    from phrases import TAKE_SETS

    here = Path(__file__).resolve().parent / "capture"
    for name in shipped_captures():
        takes = json.loads((here / f"{name}.json").read_text()).get("takes")
        if takes:
            assert takes in TAKE_SETS, f"{name} names phrase set {takes!r}, which does not exist"
