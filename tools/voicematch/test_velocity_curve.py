"""The velocity curve keeps per-note response shape after gain removal."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit_resolve
import render_rows
from autofit_test_fixtures import _probe_args
from fit_quality import quality_report
from loss import probe_rows, score_terms
from loss_aggregate import robust_score_terms
from loss_cells import resolved_weights as cell_resolved_weights
from loss_dimensions import DYN_MIN_VELOCITY_SPREAD
from loss_weights import TERM_UNITS, LossWeights, cli_weights
from metrics_signal import DB_FLOOR
from patterns import build_pattern


def _rows(peaks: dict[int, float | None], *, note: int = 60) -> list[dict]:
    """Complete pitched/percussion rows with a controlled peak-level curve."""
    return [
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
        for velocity, peak in peaks.items()
    ]


def test_middle_velocity_error_is_visible_when_velocity_span_matches():
    oracle = _rows({32: -20.0, 64: -13.0, 127: -10.0})
    wrong_middle = _rows({32: -20.0, 64: -20.0, 127: -10.0})
    reversed_curve = _rows({32: -10.0, 64: -13.0, 127: -20.0})

    narrow = score_terms(wrong_middle, oracle)
    reversed_terms = score_terms(reversed_curve, oracle)
    percussion = score_terms(wrong_middle, oracle, percussive=True)

    assert narrow["velspan"] == pytest.approx(0.0)
    assert reversed_terms["velspan"] == pytest.approx(0.0)
    assert narrow["velcurve"] == pytest.approx(7.0 / 3.0)
    assert reversed_terms["velcurve"] == pytest.approx(20.0 / 3.0)
    assert narrow["velcurve_cells"] == 3.0
    assert narrow["velcurve_groups"] == 1.0
    for key in (
        "velcurve",
        "velcurve_cells",
        "velcurve_groups",
        "velcurve_capped",
        "velcurve_absent",
    ):
        assert percussion[key] == pytest.approx(narrow[key])


def test_curve_removes_note_gain_and_caps_missing_model_cells_on_reference_denominator():
    oracle = _rows({32: -30.0, 64: -22.0, 127: -18.0})
    gain_shifted = _rows({32: -24.0, 64: -16.0, 127: -12.0})
    missing_middle = _rows({32: -30.0, 64: None, 127: -18.0})
    counts = score_terms(missing_middle, oracle)

    assert score_terms(gain_shifted, oracle)["velcurve"] == pytest.approx(0.0)
    assert counts["velcurve"] == pytest.approx(6.0)
    assert counts["velcurve_cells"] == 3.0
    assert counts["velcurve_groups"] == 1.0
    assert counts["velcurve_absent"] == 1.0
    assert counts["velcurve_capped"] == 1.0


def test_curve_removes_different_gain_offsets_per_note():
    oracle = [
        *_rows({32: -30.0, 64: -22.0, 127: -18.0}, note=60),
        *_rows({32: -25.0, 64: -17.0, 127: -13.0}, note=72),
    ]
    model = [
        *_rows({32: -24.0, 64: -16.0, 127: -12.0}, note=60),
        *_rows({32: -34.0, 64: -26.0, 127: -22.0}, note=72),
    ]

    for percussive in (False, True):
        terms = score_terms(model, oracle, percussive=percussive)
        robust = robust_score_terms(model, oracle, percussive=percussive)
        assert terms["velcurve"] == pytest.approx(0.0)
        assert terms["velcurve_groups"] == 2.0
        assert terms["velcurve_cells"] == 6.0
        assert robust["velcurve"] == pytest.approx(0.0)
        assert robust["velcurve_mean"] == pytest.approx(0.0)
        assert robust["velcurve_upper_tail"] == pytest.approx(0.0)


@pytest.mark.parametrize(
    ("model_peaks", "missing_velocities", "expected_full", "expected_dropped"),
    [
        ({32: -130.0, 64: -30.0, 96: -20.0, 127: -10.0}, (32,), 9.5, 9.5),
        ({32: 70.0, 64: -30.0, 96: -40.0, 127: -50.0}, (32,), 9.5, 9.5),
        ({32: -130.0, 64: -30.0, 96: -20.0, 127: -10.0}, (32, 127), 9.5, 11.5),
    ],
    ids=("negative-outlier", "positive-outlier", "two-dropped-cells"),
)
def test_missing_peak_cannot_improve_curve_by_shifting_a_capped_offset(
    model_peaks, missing_velocities, expected_full, expected_dropped
):
    oracle = _rows({32: -30.0, 64: -30.0, 96: -30.0, 127: -30.0})
    dropped_peaks = dict(model_peaks)
    for velocity in missing_velocities:
        dropped_peaks[velocity] = None
    full = _rows(model_peaks)
    dropped = _rows(dropped_peaks)

    full_terms = score_terms(full, oracle)
    dropped_terms = score_terms(dropped, oracle)

    assert full_terms["velcurve"] == pytest.approx(expected_full)
    assert dropped_terms["velcurve"] == pytest.approx(expected_dropped)
    assert dropped_terms["velcurve"] >= full_terms["velcurve"]
    assert full_terms["velcurve_cells"] == dropped_terms["velcurve_cells"] == 4.0
    assert full_terms["velcurve_absent"] == 0.0
    assert dropped_terms["velcurve_absent"] == float(len(missing_velocities))


def test_oracle_missing_peaks_are_unknown_and_both_metric_sets_share_curve_metadata():
    oracle = _rows({32: None, 64: -22.0, 127: -18.0})
    same_curve = _rows({32: -44.0, 64: -16.0, 127: -12.0})

    pitched = score_terms(same_curve, oracle)
    percussion = score_terms(same_curve, oracle, percussive=True)

    assert pitched["velcurve"] == pytest.approx(0.0)
    assert pitched["velcurve_cells"] == 2.0
    assert pitched["velcurve_groups"] == 1.0
    assert pitched["velcurve_absent"] == 0.0
    for key in (
        "velcurve",
        "velcurve_cells",
        "velcurve_groups",
        "velcurve_absent",
        "velcurve_capped",
    ):
        assert percussion[key] == pytest.approx(pitched[key])
    assert all(np.isscalar(value) and np.isfinite(value) for value in percussion.values())


def test_reference_floor_is_unknown_and_candidate_floor_is_absent():
    floor_db = 20.0 * np.log10(DB_FLOOR)
    finite_oracle = _rows({32: -30.0, 64: -22.0, 127: -18.0})
    floor_reference = _rows({32: floor_db, 64: -22.0, 127: -18.0})
    floor_candidate = _rows({32: floor_db, 64: floor_db, 127: floor_db})

    unknown = score_terms(_rows({32: -5.0, 64: -16.0, 127: -12.0}), floor_reference)
    silent = score_terms(floor_candidate, finite_oracle)

    assert unknown["velcurve"] == pytest.approx(0.0)
    assert unknown["velcurve_cells"] == 2.0
    assert unknown["velcurve_absent"] == 0.0
    assert silent["velcurve"] == pytest.approx(18.0)
    assert silent["velcurve_cells"] == 3.0
    assert silent["velcurve_absent"] == 3.0


def test_raw_silence_from_probe_rows_is_charged_as_absent_model_peaks():
    from loss_dimensions import _velcurve_terms

    sr = 12000
    pattern = build_pattern(
        "velocity",
        0,
        note=60,
        velocities=(32, 64, 96, 127),
        dur=0.45,
        gap=0.2,
    )
    oracle_audio = _velocity_audio(pattern, (-30.0, -22.0, -15.0, -8.0), sr)
    silent_audio = np.zeros_like(oracle_audio)
    oracle_rows = probe_rows(oracle_audio, pattern, sr, raw=oracle_audio)
    silent_rows = probe_rows(silent_audio, pattern, sr, raw=silent_audio)
    counts: dict[str, float] = {}

    value, cells = _velcurve_terms(silent_rows, oracle_rows, counts=counts)

    assert all(row["peak_dbfs"] == pytest.approx(20.0 * np.log10(DB_FLOOR)) for row in silent_rows)
    assert value == pytest.approx(18.0)
    assert cells == 4
    assert counts["absent"] == 4.0


def test_upper_tail_uses_equal_note_groups_and_zero_tail_fraction_preserves_mean():
    oracle = [
        row
        for note in range(60, 64)
        for row in _rows({32: -20.0, 64: -13.0, 127: -10.0}, note=note)
    ]
    model = [dict(row) for row in oracle]
    for row in model:
        if row["note"] == 60 and row["velocity"] == 64:
            row["peak_dbfs"] = -31.0

    mean = score_terms(model, oracle)
    robust = robust_score_terms(model, oracle, tail_fraction=0.25)
    legacy = robust_score_terms(model, oracle, tail_fraction=0.0)

    assert mean["velcurve"] == pytest.approx(1.5)
    assert robust["velcurve_mean"] == pytest.approx(1.5)
    assert robust["velcurve_upper_tail"] == pytest.approx(6.0)
    assert robust["velcurve_worst"] == pytest.approx(6.0)
    assert robust["velcurve_worst_note"] == 60
    assert "velcurve_worst_row" not in robust
    assert "velcurve_worst_velocity" not in robust
    assert robust["velcurve"] == pytest.approx(6.0)
    assert legacy == mean
    weights = LossWeights({"velcurve": 1.0})
    weights.calibrate(score_terms(oracle, oracle))
    assert TERM_UNITS["velcurve"] == 1.0
    assert weights.term_contributions(robust)["velcurve"] == pytest.approx(6.0)
    report = quality_report(robust, weights, baseline_terms=mean)
    assert report["terms"]["velcurve"]["residual_units"] == pytest.approx(6.0)
    assert report["terms"]["velcurve"]["worst_condition"]["note"] == 60


@pytest.mark.parametrize(("velocities", "eligible"), [((100, 115), False), ((100, 116), True)])
def test_probe_velocity_axis_uses_the_full_minimum_spread(monkeypatch, velocities, eligible):
    monkeypatch.setattr(
        autofit_resolve,
        "check_oracle_rig",
        lambda *_args, **_kwargs: None,
    )
    monkeypatch.setattr(render_rows, "render_rate", lambda _corpus: 48000)
    args = _probe_args(
        pattern="velocity",
        notes="60",
        velocities=",".join(str(v) for v in velocities),
        w_velcurve=1.0,
    )
    pattern = build_pattern(
        "velocity",
        0,
        note=60,
        velocities=velocities,
    )

    if eligible:
        autofit_resolve.resolve_probe(args)
        assert args.has_velocity_spread is True
    else:
        with pytest.raises(ValueError, match="below the required 16"):
            autofit_resolve.resolve_probe(args)
        assert args.has_velocity_spread is False
    cell_weights = cell_resolved_weights({"program": 0}, [], pattern, sample_rate=48000)
    assert ("dyn" in cell_weights) is eligible
    assert ("velspan" in cell_weights) is eligible
    assert max(velocities) - min(velocities) == (16 if eligible else 15)
    assert DYN_MIN_VELOCITY_SPREAD == 16.0


def test_velocity_curve_weight_is_explicit_and_zero_by_default():
    default = cli_weights(_probe_args(pattern="velocity", has_velocity_spread=True))
    explicit = cli_weights(
        _probe_args(pattern="velocity", has_velocity_spread=True, w_velcurve=0.75)
    )

    assert "velcurve" not in default
    assert explicit["velcurve"] == pytest.approx(0.75)


def _velocity_audio(pattern, peak_dbfs: tuple[float, ...], sr: int) -> np.ndarray:
    """Harmonic tone bursts whose per-hit peak follows the supplied curve."""
    n = round((pattern.notes[-1].start + pattern.tail) * sr)
    audio = np.zeros(n, dtype=np.float64)
    f0 = 261.625565
    for note, peak_db in zip(pattern.analysis_notes, peak_dbfs):
        start = round(note.start * sr)
        count = round(note.dur * sr)
        t = np.arange(count, dtype=np.float64) / sr
        ramp = np.minimum(1.0, t / 0.015)
        tone = np.sin(2.0 * np.pi * f0 * t)
        tone += 0.25 * np.sin(2.0 * np.pi * 2.0 * f0 * t)
        tone += 0.12 * np.sin(2.0 * np.pi * 3.0 * f0 * t)
        audio[start : start + count] = 10.0 ** (peak_db / 20.0) * ramp * tone
    return audio


def test_audio_probe_rows_detects_changed_middle_curve_with_same_endpoint_span():
    sr = 12000
    pattern = build_pattern(
        "velocity",
        0,
        note=60,
        velocities=(32, 64, 96, 127),
        dur=0.45,
        gap=0.2,
    )
    oracle_audio = _velocity_audio(pattern, (-30.0, -22.0, -15.0, -8.0), sr)
    changed_audio = _velocity_audio(pattern, (-30.0, -30.0, -15.0, -8.0), sr)
    oracle_rows = probe_rows(oracle_audio, pattern, sr, raw=oracle_audio)
    changed_rows = probe_rows(changed_audio, pattern, sr, raw=changed_audio)

    terms = score_terms(changed_rows, oracle_rows)

    assert terms["velspan"] == pytest.approx(0.0, abs=0.1)
    assert terms["velcurve"] > 1.0
    assert terms["velcurve_cells"] == 4.0
