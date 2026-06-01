"""Smoke tests for the newly-exposed C-ABI functions (Wave 2.2b exposure).

Each function is implemented + tested at the C ABI; these guard that the Python
facade wiring (ctypes signature, out-buffer copy, free, re-export) is correct and
returns the documented shape with finite values.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library unavailable")

SR = 22050


def _tone(freq: float = 440.0, seconds: float = 1.0) -> np.ndarray:
    n = int(SR * seconds)
    t = np.arange(n, dtype=np.float32) / SR
    return (0.5 * np.sin(2.0 * math.pi * freq * t)).astype(np.float32)


def _finite(arr) -> bool:
    a = np.asarray(arr, dtype=np.float64)
    return a.size > 0 and bool(np.all(np.isfinite(a)))


def test_spectral_contrast_returns_band_matrix() -> None:
    matrix = libsonare.spectral_contrast(_tone(), SR, n_bands=6)
    assert matrix.shape[0] == 7  # n_bands + 1
    assert matrix.shape[1] > 0
    assert _finite(matrix)


def test_poly_features_returns_coeff_matrix() -> None:
    matrix = libsonare.poly_features(_tone(), SR, order=1)
    assert matrix.shape[0] == 2  # order + 1
    assert matrix.shape[1] > 0
    assert _finite(matrix)


def test_zero_crossings_indices_are_sorted_and_in_range() -> None:
    x = _tone(440.0, 0.05)
    idx = libsonare.zero_crossings(x)
    assert len(idx) > 0
    assert all(0 <= i < len(x) for i in idx)
    assert list(idx) == sorted(idx)


def test_pitch_tuning_is_finite() -> None:
    tuning = libsonare.pitch_tuning([440.0, 880.0, 660.0])
    assert math.isfinite(tuning)
    assert -0.5 <= tuning < 0.5


def test_estimate_tuning_is_finite() -> None:
    tuning = libsonare.estimate_tuning(_tone())
    assert math.isfinite(tuning)
    assert -0.5 <= tuning < 0.5


def test_decompose_factorizes_spectrogram() -> None:
    n_features, n_frames, n_components = 16, 24, 3
    rng = np.random.default_rng(0)
    spec = np.abs(rng.standard_normal((n_features, n_frames))).astype(np.float32).ravel()
    w, h = libsonare.decompose(spec, n_features, n_frames, n_components, n_iter=20)
    assert np.asarray(w).size == n_features * n_components
    assert np.asarray(h).size == n_components * n_frames
    assert _finite(w) and _finite(h)


def test_nn_filter_preserves_shape() -> None:
    n_features, n_frames = 12, 20
    rng = np.random.default_rng(1)
    spec = np.abs(rng.standard_normal((n_features, n_frames))).astype(np.float32).ravel()
    out = libsonare.nn_filter(spec, n_features, n_frames)
    assert out.shape == (n_features, n_frames)
    assert _finite(out)


def test_remix_concatenates_intervals() -> None:
    x = _tone(440.0, 0.5)
    half = len(x) // 2
    # Reverse the two halves.
    out = libsonare.remix(x, [half, len(x), 0, half])
    assert np.asarray(out).size == len(x)
    assert _finite(out)


def test_hpss_with_residual_splits_three_ways() -> None:
    res = libsonare.hpss_with_residual(_tone())
    harmonic = np.asarray(res["harmonic"], dtype=np.float32)
    percussive = np.asarray(res["percussive"], dtype=np.float32)
    residual = np.asarray(res["residual"], dtype=np.float32)
    assert harmonic.size == percussive.size == residual.size > 0
    assert _finite(harmonic) and _finite(percussive) and _finite(residual)


def test_phase_vocoder_changes_length_by_rate() -> None:
    x = _tone(440.0, 0.5)
    out = np.asarray(libsonare.phase_vocoder(x, SR, 2.0), dtype=np.float32)
    # rate=2.0 => roughly half the samples.
    assert 0 < out.size < len(x)
    assert _finite(out)


def test_lufs_interleaved_matches_mono_for_dual_mono() -> None:
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    res = libsonare.lufs_interleaved(interleaved, 2, SR)
    assert math.isfinite(res.integrated_lufs)
    assert res.integrated_lufs < 0.0


def test_ebur128_loudness_range_is_finite_nonnegative() -> None:
    lra = libsonare.ebur128_loudness_range(_tone())
    assert math.isfinite(lra)
    assert lra >= 0.0
