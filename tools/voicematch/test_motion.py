"""The pitch-motion dimension: its units, the unwarp test that decides where it is gated,
and the one reading per recording it compares."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from knobs import KNOB_CLASSES_PATH
from metrics_motion import (
    COHERENCE_MARGIN,
    DIMENSION,
    ENSEMBLE_CEILING,
    SINGLE_SOURCE_FLOOR,
    CoherenceState,
    coherence_state,
    motion_deltas,
    partial_coherence,
    partial_tracks,
    recording_motion,
    warp_reading,
)
from metrics_partials import measure_tnr_mask
from profile_gate import timbre_pair_deltas
from toneclass import canonical_dimensions

SR = 48000
F0 = 261.626
SECONDS = 3


def _tone(depth_cents_pp: float, rate_hz: float, partials: int = 8) -> np.ndarray:
    """A harmonic tone whose pitch swings `depth_cents_pp` peak to peak at `rate_hz`."""
    t = np.arange(SR * SECONDS) / SR
    dev = 0.5 * depth_cents_pp * np.sin(2.0 * np.pi * rate_hz * t)
    phase = 2.0 * np.pi * np.cumsum(F0 * 2.0 ** (dev / 1200.0)) / SR
    return sum(np.sin(k * phase) / k for k in range(1, partials + 1))


@pytest.mark.parametrize("depth,rate", [(20.0, 5.5), (40.0, 5.0), (80.0, 6.0)])
def test_the_fm_depth_is_peak_to_peak_cents_and_the_rate_is_in_its_span(depth, rate):
    fm = measure_tnr_mask(_tone(depth, rate), SR, F0)["fm"]
    assert fm[0] == pytest.approx(depth, abs=0.5)
    assert fm[1] - 0.5 <= rate <= fm[2] + 0.5


def test_a_still_tone_reads_no_motion():
    assert "fm" not in measure_tnr_mask(_tone(0.0, 5.0), SR, F0)


def _sources(count: int, seed: int = 0, f0: float = F0) -> np.ndarray:
    """`count` voices of 40 c p-p vibrato at independent rates and phases, summed."""
    rng = np.random.default_rng(seed)
    t = np.arange(SR * SECONDS) / SR
    out = np.zeros_like(t)
    for _ in range(count):
        dev = 20.0 * np.sin(2.0 * np.pi * rng.uniform(4.5, 6.5) * t + rng.uniform(0.0, 6.28))
        phase = 2.0 * np.pi * np.cumsum(f0 * 2.0 ** (dev / 1200.0)) / SR
        out += sum(np.sin(k * phase) / k for k in range(1, 9))
    return out / count


def _with_partial_am(depth_db: float, f0: float = F0) -> np.ndarray:
    """A 40 c, 5.5 Hz vibrato whose partials each swing `depth_db` at that rate, phases differing."""
    rng = np.random.default_rng(1)
    t = np.arange(SR * SECONDS) / SR
    dev = 20.0 * np.sin(2.0 * np.pi * 5.5 * t)
    phase = 2.0 * np.pi * np.cumsum(f0 * 2.0 ** (dev / 1200.0)) / SR
    out = np.zeros_like(t)
    for k in range(1, 9):
        swing = 0.5 * rng.uniform(0.33, 1.0) * depth_db
        amp = 10.0 ** (swing * np.sin(2.0 * np.pi * 5.5 * t + rng.uniform(0.0, 6.28)) / 20.0)
        out += amp * np.sin(k * phase) / k
    return out


@pytest.mark.parametrize("f0", [F0, 880.0])
def test_one_source_is_coherent_with_or_without_partial_am_and_several_sources_are_not(f0):
    single = _tone(40.0, 5.5) if f0 == F0 else None
    if single is None:
        t = np.arange(SR * SECONDS) / SR
        phase = (
            2.0
            * np.pi
            * np.cumsum(f0 * 2.0 ** (20.0 * np.sin(2.0 * np.pi * 5.5 * t) / 1200.0))
            / SR
        )
        single = sum(np.sin(k * phase) / k for k in range(1, 9))
    coherent = [partial_coherence(single, SR, f0)]
    coherent += [partial_coherence(_with_partial_am(d, f0), SR, f0) for d in (3.0, 6.0, 9.0)]
    incoherent = [partial_coherence(_sources(n, s, f0), SR, f0) for n in (3, 4, 5) for s in (0, 1)]
    assert min(coherent) > 0.95
    assert max(incoherent) < 0.45
    assert max(incoherent) < ENSEMBLE_CEILING < SINGLE_SOURCE_FLOOR < min(coherent)


def test_unwarping_collapses_one_source_and_not_several():
    assert warp_reading(_tone(20.0, 5.5), SR, F0)["w20_ratio"] < 0.1
    assert warp_reading(_sources(4), SR, F0).get("w20_ratio", 1.0) > 0.75


def _row(cents=None, rate=None, coherence=None, refused=None):
    row: dict = {"timbre": "t"}
    if cents is not None:
        row["fm"] = [cents, rate * 0.9, rate * 1.1]
        row["fm_warp"] = {"refused": refused} if refused else {"w20_ratio": 0.5}
        row["fm_coherence"] = coherence
    return row


def test_a_recording_is_gated_where_its_partials_move_together():
    reading = recording_motion([_row(20.0, 5.0, 0.99), _row(24.0, 5.0, 0.95), _row(22.0, 5.0, 0.9)])
    assert reading["gated"] and reading["cents"] == 22.0 and reading["coherence"] == 0.95
    assert reading["rate_hz"] == pytest.approx(5.0 * np.sqrt(0.99), abs=1e-3)


def test_one_incoherent_note_does_not_ungate_a_coherent_recording():
    rows = [_row(20.0, 5.0, c) for c in (0.9, 0.95, 0.3, 0.92, 0.88)]
    assert recording_motion(rows)["gated"]


@pytest.mark.parametrize("coherence", [0.0, 0.39, ENSEMBLE_CEILING])
def test_a_recording_whose_partials_do_not_follow_the_fundamental_is_not_gated(coherence):
    reading = recording_motion([_row(20.0, 5.0, coherence)] * 3)
    assert not reading["gated"] and reading["coherence_state"] == "ensemble"
    assert "not one source's" in reading["reason"]


@pytest.mark.parametrize("coherence", [0.493, 0.5, 0.6, 0.68])
def test_a_recording_inside_the_gap_is_undecided_and_not_gated(coherence):
    reading = recording_motion([_row(20.0, 5.0, coherence)] * 3)
    assert not reading["gated"] and reading["coherence_state"] == "undecided"
    assert reading["reason"] == (
        f"coherence {coherence:.3f} between the single-source and ensemble ranges"
    )
    assert reading["states"] == {"single_source": 0, "ensemble": 0, "undecided": 3}


def test_the_rows_of_a_gated_recording_are_counted_by_state():
    rows = [_row(20.0, 5.0, c) for c in (0.95, 0.9, 0.9, 0.55, 0.3)]
    reading = recording_motion(rows)
    assert reading["gated"]
    assert reading["states"] == {"single_source": 3, "ensemble": 1, "undecided": 1}


def test_a_recording_is_not_gated_without_motion_or_a_coherence_reading():
    assert recording_motion([_row(), _row()])["gated"] is False
    still = recording_motion([_row(20.0, 5.0, 0.9), _row(), _row()])
    assert not still["gated"] and "1 of 3" in still["reason"]
    old = {"timbre": "t", "fm": [20.0, 4.5, 5.5]}
    assert "no coherence reading" in recording_motion([old, old])["reason"]


def test_the_deltas_are_the_model_over_the_reference_in_doublings_of_depth_and_rate():
    ref = [_row(20.0, 5.0, 0.9)] * 3
    model = [{"fm": [40.0, 2.25, 2.75]}] * 3
    depth, rate = motion_deltas(ref, model)
    assert depth == pytest.approx(1.0)
    assert rate == pytest.approx(np.log2(np.sqrt(2.25 * 2.75) / (5.0 * np.sqrt(0.99))), abs=1e-3)


def test_a_still_model_reads_the_extractors_minimum_depth_and_has_no_rate_delta():
    depth, *rest = motion_deltas([_row(20.0, 5.0, 0.9)] * 3, [{}, {}, {}])
    assert depth == pytest.approx(np.log2(1.0 / 20.0)) and rest == []


def test_nothing_is_compared_where_the_reference_is_not_gated():
    assert motion_deltas([_row(20.0, 5.0, 0.1)] * 3, [{"fm": [20.0, 5.0, 5.0]}]) == []


def test_two_references_are_compared_only_when_both_are_gated():
    a = [{**_row(20.0, 5.0, 0.9), "timbre": "a", "note": n, "velocity": 80} for n in (60, 64)]
    b = [{**_row(30.0, 5.0, 0.9), "timbre": "b", "note": n, "velocity": 80} for n in (60, 64)]
    assert timbre_pair_deltas(a + b, "a", "b")["motion"][0] == pytest.approx(np.log2(20 / 30))
    c = [{**_row(30.0, 5.0, 0.1), "timbre": "b", "note": n, "velocity": 80} for n in (60, 64)]
    assert "motion" not in timbre_pair_deltas(a + c, "a", "b")


def test_the_knob_reach_rules_name_this_dimension():
    reach = json.loads(Path(KNOB_CLASSES_PATH).read_text())["reach"]
    needed = {d for k, r in reach.items() if not k.startswith("_") for d in r["requires"]}
    assert DIMENSION in needed and needed <= {DIMENSION, "stereo"}


@pytest.mark.parametrize("program", [40, 41, 56, 60, 64, 68, 73, 79, 53, 54, 110])
def test_a_class_that_sustains_one_moving_pitch_is_gated_on_motion(program):
    assert DIMENSION in canonical_dimensions(program)


@pytest.mark.parametrize("program", [0, 24, 48, 49, 52, 88, 98, 14])
def test_ensembles_struck_and_plucked_voices_are_not(program):
    assert DIMENSION not in canonical_dimensions(program)


def test_the_bounds_sit_a_margin_beyond_the_readings_they_were_set_from():
    assert SINGLE_SOURCE_FLOOR == pytest.approx(0.749 - COHERENCE_MARGIN)
    assert ENSEMBLE_CEILING == pytest.approx(0.391 + COHERENCE_MARGIN)
    assert ENSEMBLE_CEILING < SINGLE_SOURCE_FLOOR


@pytest.mark.parametrize(
    "value,state",
    [
        (1.0, CoherenceState.SINGLE_SOURCE),
        (SINGLE_SOURCE_FLOOR, CoherenceState.SINGLE_SOURCE),
        (SINGLE_SOURCE_FLOOR - 1e-6, CoherenceState.UNDECIDED),
        (0.5, CoherenceState.UNDECIDED),
        (ENSEMBLE_CEILING + 1e-6, CoherenceState.UNDECIDED),
        (ENSEMBLE_CEILING, CoherenceState.ENSEMBLE),
        (-0.1, CoherenceState.ENSEMBLE),
    ],
)
def test_the_state_changes_exactly_at_each_bound(value, state):
    got, coherence, reason = coherence_state(value)
    assert got is state and coherence == value and reason


def test_the_decision_reads_tracks_notes_and_recordings_alike():
    single = partial_tracks(_tone(40.0, 5.5), SR, F0)
    several = partial_tracks(_sources(4), SR, F0)
    assert coherence_state(single)[0] is CoherenceState.SINGLE_SOURCE
    assert coherence_state(several)[0] is CoherenceState.ENSEMBLE
    # A recording is the median of its notes: one odd note does not move it.
    assert coherence_state([single, single, several])[0] is CoherenceState.SINGLE_SOURCE
    assert coherence_state([0.95, 0.9, 0.2])[1] == pytest.approx(0.9)
    with pytest.raises(ValueError):
        coherence_state([])
