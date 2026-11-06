"""The percussion velocity range uses the measured peak level at each hit."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from autofit_test_fixtures import _probe_args
from loss import probe_rows, score_terms
from loss_weights import cli_weights
from patterns import build_pattern
from refit import reweighed
from toneclass import default_weights


def _rows(peaks: dict[int, float | None], *, note: int = 60) -> list[dict]:
    """Rows with both metric sets' required fields and a known peak ladder."""
    rows = []
    for velocity, peak in peaks.items():
        rows.append(
            {
                "note": note,
                "velocity": velocity,
                "peak_dbfs": peak,
                "held_rms_dbfs": -35.0,
                "held_crest_db": 12.0,
                "harmonics_db": [0.0, -6.0, -12.0] + [-24.0] * 9,
                "f0_cents_err": 0.0,
                "tnr_db": 30.0,
                "sustain_slope_db_s": -3.0,
                "release_ms": 80.0,
                "attack_ms": 5.0,
                "attack_fine_ms": 5.0,
                "skeleton": {},
                "attack_hf_db": [],
                "attack_lf_db": [],
                "bands_db": [-6.0] * 25,
                "band_decay_db_s": [None] * 13,
                "decay_ms": 100.0,
                "decay_capped": False,
                "crest_db": 12.0,
                "modal_hz": [],
                "modal_db": [],
            }
        )
    return rows


def test_percussion_and_pitched_reducers_share_gain_invariant_peak_span():
    oracle = _rows({32: -36.0, 64: -28.0, 127: -20.0})
    gain_shifted = _rows({32: -29.0, 64: -21.0, 127: -13.0})
    narrower = _rows({32: -36.0, 64: -32.0, 127: -28.0})

    for percussive in (False, True):
        same = score_terms(gain_shifted, oracle, percussive=percussive)
        wrong = score_terms(narrower, oracle, percussive=percussive)
        assert same["velspan"] == pytest.approx(0.0)
        assert same["velspan_groups"] == 1.0
        assert wrong["velspan"] == pytest.approx(8.0)
        assert wrong["velspan_groups"] == 1.0


def test_percussion_charges_a_missing_model_peak_as_the_span_cap():
    oracle = _rows({32: -30.0, 127: -18.0})
    model = _rows({32: None, 127: -18.0})

    pitched = score_terms(model, oracle)
    terms = score_terms(model, oracle, percussive=True)

    assert terms["velspan"] == pytest.approx(18.0)
    assert terms["velspan_groups"] == 1.0
    assert terms["velspan_absent"] == 1.0
    assert terms["velspan_capped"] == 1.0
    for key in ("velspan", "velspan_groups", "velspan_absent", "velspan_capped"):
        assert terms[key] == pytest.approx(pitched[key])


def test_percussive_cli_keeps_an_explicit_velocity_span_weight():
    args = _probe_args(
        program=0,
        percussive=True,
        has_velocity_spread=True,
        w_velspan=1.25,
    )

    assert cli_weights(args)["velspan"] == pytest.approx(1.25)
    assert "velspan" not in default_weights(0, percussive=True)


def test_drum_velocity_range_trade_reweights_the_velocity_span_term():
    report = {"fit_profile": {"weights": {"velspan": 0.5}}}

    assert reweighed(report, ["drums_module:vel_range p90"], percussive=True) == {"velspan": 1.0}


def _drum_audio(pattern, gains: tuple[float, ...], sr: int) -> np.ndarray:
    """Same decaying strike at every velocity, scaled by a chosen gain curve."""
    n = round((pattern.notes[-1].start + pattern.tail) * sr)
    audio = np.zeros(n, dtype=np.float64)
    rng = np.random.default_rng(417)
    t = np.arange(round(0.18 * sr), dtype=np.float64) / sr
    strike = (0.55 * np.sin(2.0 * np.pi * 185.0 * t) + 0.45 * rng.standard_normal(len(t))) * np.exp(
        -24.0 * t
    )
    strike /= np.max(np.abs(strike))
    for note, gain in zip(pattern.analysis_notes, gains):
        start = round(note.start * sr)
        audio[start : start + len(strike)] += gain * strike
    return audio


def test_audio_probe_rows_charge_a_changed_drum_velocity_gain_curve():
    sr = 12000
    pattern = build_pattern(
        "drum",
        0,
        notes=(38,),
        velocities=(32, 64, 96, 127),
        dur=0.1,
        gap=0.55,
    )
    oracle_audio = _drum_audio(pattern, (0.04, 0.08, 0.18, 0.45), sr)
    shifted_audio = oracle_audio * 0.5
    altered_audio = _drum_audio(pattern, (0.04, 0.10, 0.4, 0.9), sr)
    oracle_rows = probe_rows(oracle_audio, pattern, sr, raw=oracle_audio)
    shifted_rows = probe_rows(shifted_audio, pattern, sr, raw=shifted_audio)
    altered_rows = probe_rows(altered_audio, pattern, sr, raw=altered_audio)

    self_terms = score_terms(oracle_rows, oracle_rows, percussive=True)
    gain_terms = score_terms(shifted_rows, oracle_rows, percussive=True)
    changed_terms = score_terms(altered_rows, oracle_rows, percussive=True)

    assert self_terms["velspan"] == pytest.approx(0.0)
    assert self_terms["velspan_groups"] == 1.0
    assert gain_terms["velspan"] == pytest.approx(0.0)
    assert changed_terms["velspan"] > 4.0
    assert changed_terms["velspan_groups"] == 1.0
