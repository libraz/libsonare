"""The per-partial decay ruler must use signal, and score its whole range."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from loss import (
    TERM_UNITS,
    band_min_note_s,
    band_min_note_samples_for,
    loss_terms,
    probe_rows,
    skeleton_note,
)
from loss_cells import resolved_weights
from metrics import midi_to_hz
from metrics_skeleton import skeleton_frame_s
from patterns import Pattern
from smf import Note


def _exponential(rate_db_s: float, *, noise: float = 0.0, truncate_db: float | None = None):
    sr = 8000
    t = np.arange(8 * sr) / sr
    y = np.sin(2 * np.pi * 440.0 * t) * 10.0 ** (-rate_db_s * t / 20.0)
    if truncate_db is not None:
        y[t > truncate_db / rate_db_s] = 0.0
    if noise:
        y += np.random.default_rng(20261106).normal(0.0, noise, len(y))
    return y, sr


@pytest.mark.parametrize("rate", [20.0, 40.0])
def test_clean_exponential_decay_keeps_its_measured_slope(rate):
    y, sr = _exponential(rate)
    row = skeleton_note(y, sr, Note(69, 96, 0.0, 8.0), n_harm=1)

    if rate == 20.0:
        assert row["tail_db_s"][0] == pytest.approx(-rate, abs=1.0)
    else:
        # The 60 dB partial-relative range ends before the aftersound band;
        # this partial has no tail measurement rather than a floor slope.
        assert row["tail_db_s"][0] is None


def test_recording_noise_after_a_fast_decay_is_reported_as_no_tail_measurement():
    sr = 12000
    t = np.arange(8 * sr) / sr
    rng = np.random.default_rng(20260920)
    y = np.zeros_like(t)
    for harmonic in range(1, 7):
        y += (1.0 / harmonic) * np.sin(2 * np.pi * 440.0 * harmonic * t) * np.exp(-4.0 * t)
    y += rng.normal(0.0, 1e-5, len(y))
    row = skeleton_note(y, sr, Note(69, 96, 0.0, 8.0), n_harm=6)

    assert row["tail_db_s"] == [None] * 6


def test_a_missing_partial_drowned_in_noise_stays_absent():
    sr = 12000
    t = np.arange(8 * sr) / sr
    signal = np.sin(2 * np.pi * 440.0 * t) * np.exp(-0.5 * t)
    noise = np.random.default_rng(20260920).normal(0.0, 1e-5, len(t))
    row = skeleton_note(signal + noise, sr, Note(69, 96, 0.0, 8.0), n_harm=12)

    assert row["tail_db_s"][0] is not None
    assert row["tail_db_s"][1:] == [None] * 11


def test_digital_truncation_does_not_turn_a_clean_decay_into_a_floor_slope():
    y, sr = _exponential(20.0, truncate_db=90.0)
    row = skeleton_note(y, sr, Note(69, 96, 0.0, 8.0), n_harm=1)

    assert row["tail_db_s"][0] == pytest.approx(-20.0, abs=1.0)


def _upper_partial_swap(sr=12000):
    t = np.arange(8 * sr) / sr
    model = np.zeros_like(t)
    oracle = np.zeros_like(t)
    for harmonic in range(1, 9):
        f = 440.0 * harmonic
        base = (1.0 / harmonic) * np.exp(-0.2 * t)
        if harmonic <= 6:
            model += base * np.sin(2 * np.pi * f * t)
            oracle += base * np.sin(2 * np.pi * f * t)
            continue
        before = 0.30 if harmonic == 7 else 0.01
        after = 0.01 if harmonic == 7 else 0.30
        swapped = np.where(
            t < 2.0,
            before,
            np.where(t > 2.1, after, before + (after - before) * (t - 2.0) / 0.1),
        )
        tone = np.sin(2 * np.pi * f * t)
        model += before * base * tone
        oracle += swapped * base * tone
    return model, oracle, sr


def _calibration_string(note, h2_rate=10.0, *, phases=(0.0, 0.7, 1.1), gain=1.0, detune=0.0):
    sr = 48000
    t = np.arange(8 * sr) / sr
    f0 = 440.0 * 2.0 ** ((note - 69) / 12.0)
    y = np.zeros_like(t)
    for harmonic, (amp, rate, phase) in enumerate(
        zip((1.0, 0.05, 0.2), (2.0, h2_rate, 4.0), phases), start=1
    ):
        f = f0 * harmonic * (1.0 + detune if harmonic == 2 else 1.0)
        y += gain * amp * 10.0 ** (-rate * t / 20.0) * np.sin(2.0 * np.pi * f * t + phase)
    return y, sr


@pytest.mark.parametrize("note", [21, 33, 69])
@pytest.mark.parametrize("phases", [(0.0, 0.7, 1.1), (0.3, 1.9, 2.2)])
def test_adaptive_joint_measurement_recovers_close_partials(note, phases):
    y, sr = _calibration_string(note, phases=phases)
    row = skeleton_note(y, sr, Note(note, 96, 0.0, 8.0))

    assert row["init_db"][1] == pytest.approx(20.0 * np.log10(0.05), abs=0.1)
    assert row["early_db_s"][:3] == pytest.approx([-2.0, -10.0, -4.0], abs=0.25)
    assert row["tail_db_s"][:3] == pytest.approx([-2.0, -10.0, -4.0], abs=0.25)
    assert row["tail_db_s"][3:] == [None] * 9


def test_target_decay_is_unique_best_on_a_withheld_note_and_phase():
    note = 24
    phases = (0.8, 2.3, 0.4)
    oracle_audio, sr = _calibration_string(note, h2_rate=10.0, phases=phases)
    oracle_skeleton = skeleton_note(oracle_audio, sr, Note(note, 96, 0.0, 8.0))
    base_row = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
    }
    oracle_row = {**base_row, "skeleton": oracle_skeleton}
    scores = {}
    for candidate_rate in (2.0, 6.0, 10.0, 14.0):
        candidate_audio, _ = _calibration_string(
            note,
            h2_rate=candidate_rate,
            phases=(2.4, 0.2, 2.8),
            gain=3.7,
        )
        model_skeleton = skeleton_note(candidate_audio, sr, Note(note, 96, 0.0, 8.0))
        terms = loss_terms([{**base_row, "skeleton": model_skeleton}], [oracle_row])
        assert terms is not None
        scores[candidate_rate] = terms["tail"]

    assert scores[10.0] == pytest.approx(0.0, abs=1e-3)
    assert scores[10.0] < min(scores[2.0], scores[6.0], scores[14.0])


def test_close_detuned_partial_remains_finite_and_self_matching():
    y, sr = _calibration_string(21, phases=(0.5, 1.7, 2.4), detune=0.0005)
    skeleton = skeleton_note(y, sr, Note(21, 96, 0.0, 8.0))
    row = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
        "skeleton": skeleton,
    }

    assert skeleton["tail_db_s"][1] is not None
    terms = loss_terms([row], [row])
    assert terms is not None and terms["tail"] == pytest.approx(0.0)


@pytest.mark.parametrize("sample_rate", [12000, 44100, 48000])
@pytest.mark.parametrize("note_number", [21, 69])
def test_four_frame_boundary_matches_integer_render_samples(sample_rate, note_number):
    assert band_min_note_s("tail_db_s") == pytest.approx(2.07)
    note = Note(note_number, 96, 0.0, 8.0)
    required = band_min_note_samples_for("tail_db_s", note, sample_rate)
    frame_s = skeleton_frame_s(midi_to_hz(note_number))
    advertised = band_min_note_s("tail_db_s", frame_s=frame_s, sample_rate=sample_rate)
    assert advertised == pytest.approx(required / sample_rate)

    f0 = midi_to_hz(note_number)
    exact_t = np.arange(required) / sample_rate
    exact_audio = np.sin(2.0 * np.pi * f0 * exact_t) * 10.0 ** (-2.0 * exact_t / 20.0)
    exact_note = Note(note_number, 96, 0.0, required / sample_rate)
    exact_row = skeleton_note(exact_audio, sample_rate, exact_note)
    assert exact_row["tail_db_s"][0] is not None

    below_n = required - 1
    below_t = np.arange(below_n) / sample_rate
    below_audio = np.sin(2.0 * np.pi * f0 * below_t) * 10.0 ** (-2.0 * below_t / 20.0)
    below_note = Note(note_number, 96, 0.0, below_n / sample_rate)
    below_row = skeleton_note(below_audio, sample_rate, below_note)
    assert below_row["tail_db_s"][0] is None

    for candidate_note, expected in ((exact_note, True), (below_note, False)):
        pattern = Pattern("sustain", [candidate_note], [candidate_note], tail=0.0)
        weights = resolved_weights(
            {"program": 0, "pattern": "sustain"},
            [{"note": note_number, "velocity": 96}],
            pattern,
            sample_rate=sample_rate,
        )
        assert ("tail" in weights) is expected


def test_probe_scores_late_decay_changes_in_partials_seven_and_eight():
    model, oracle, sr = _upper_partial_swap()
    note = Note(69, 96, 0.0, 8.0)
    pattern = Pattern("sustain", [note], [note], tail=0.0)
    model_row = probe_rows(model, pattern, sr)[0]
    oracle_row = probe_rows(oracle, pattern, sr)[0]

    assert model_row["skeleton"]["tail_db_s"][6:] != pytest.approx(
        oracle_row["skeleton"]["tail_db_s"][6:]
    )
    terms = loss_terms([model_row], [oracle_row])
    assert terms is not None
    assert terms["harm"] == pytest.approx(0.0, abs=1e-8)
    assert terms["init"] == pytest.approx(0.0, abs=1e-8)
    assert terms["tail"] > 0.01


def test_decay_loss_units_follow_the_full_twelve_partial_ladder():
    assert TERM_UNITS["init"] == 12.0
    assert TERM_UNITS["slope"] == pytest.approx(2.4)
    assert TERM_UNITS["tail"] == pytest.approx(1.2)


def test_one_error_per_measured_partial_fills_each_decay_term_unit():
    ladder = [0.0, *([-6.0] * 11)]
    row = {
        "harmonics_db": ladder,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
        "skeleton": {
            "init_db": [1.0] * 12,
            "early_db_s": [1.0] * 12,
            "late_db_s": [1.0] * 12,
            "tail_db_s": [1.0] * 12,
        },
    }
    reference = {
        **row,
        "skeleton": {
            "init_db": [0.0] * 12,
            "early_db_s": [0.0] * 12,
            "late_db_s": [0.0] * 12,
            "tail_db_s": [0.0] * 12,
        },
    }

    terms = loss_terms([row], [reference])

    assert terms is not None
    for name in ("init", "slope", "tail"):
        assert terms[name] == pytest.approx(TERM_UNITS[name])
