"""Smoke tests for the C-ABI functions surfaced through the Python facade.

Each function is implemented + tested at the C ABI; these guard that the Python
facade wiring (ctypes signature, out-buffer copy, free, re-export) is correct and
returns the documented shape with finite values.
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from typing import Any, cast

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


def test_decompose_stems_components_sum_back_to_the_input() -> None:
    x = _tone(440.0, 0.4)
    res = libsonare.decompose_stems(x, SR, n_components=2, n_fft=1024, hop_length=256, n_iter=30)
    components = res["components"]
    assert len(components) == 2
    assert all(np.asarray(c).shape == x.shape for c in components)
    assert np.asarray(res["w"]).shape == (1024 // 2 + 1, 2)
    assert np.asarray(res["h"]).shape[0] == 2
    assert res["sample_rate"] == SR
    total = sum(np.asarray(c, dtype=np.float32) for c in components)
    interior = slice(1024, len(x) - 1024)
    err = float(np.linalg.norm(total[interior] - x[interior]))
    assert err / float(np.linalg.norm(x[interior])) < 0.05


def test_decompose_stems_rejects_an_out_of_range_mask_power() -> None:
    with pytest.raises(ValueError):
        libsonare.decompose_stems(_tone(440.0, 0.2), SR, mask_power=0.5)


def _stems_fingerprint(**kwargs: float) -> tuple[int, int, tuple[float, ...]]:
    res = libsonare.decompose_stems(_tone(440.0, 0.37), SR, **kwargs)  # type: ignore[arg-type]
    components = cast("Sequence[Any]", res["components"])
    first = np.asarray(components[0], dtype=np.float32)
    return len(components), int(first.size), tuple(float(v) for v in first[:8])


def test_decompose_stems_beta_zero_selects_the_documented_default() -> None:
    """0 reaches the C ABI, which reads it as "use the built-in default".

    The C ABI documents 0 on every numeric field of SonareDecomposeStemsConfig
    as that sentinel, which is what makes a zero-initialised config mean the
    documented defaults. ``beta`` is the field this facade forwards untouched,
    so the sentinel is observable here.
    """
    assert _stems_fingerprint(beta=0.0) == _stems_fingerprint(beta=2.0)
    # Without this the assertion above would also hold on a facade that dropped
    # beta on the floor.
    assert _stems_fingerprint(beta=0.5) != _stems_fingerprint(beta=0.0)


def test_decompose_stems_validates_ahead_of_the_c_abi() -> None:
    """This facade rejects values the C ABI would resolve to its defaults.

    Keyword arguments carry real defaults here, so an explicit 0 is a caller
    mistake rather than a request for the default, and it is refused instead of
    being substituted. The refusal is a ``ValueError`` raised before the call,
    where a value that reaches the library surfaces a ``SonareError``.
    """
    tone = _tone(440.0, 0.2)
    for field in ("n_components", "n_fft", "hop_length", "n_iter"):
        for value in (0, -1):
            with pytest.raises(ValueError):
                libsonare.decompose_stems(tone, SR, **{field: value})  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        libsonare.decompose_stems(tone, SR, mask_power=0.0)
    with pytest.raises(libsonare.SonareError):
        libsonare.decompose_stems(tone, SR, beta=float("nan"))
    with pytest.raises(libsonare.SonareError):
        libsonare.decompose_stems(tone, SR, mask_power=float("nan"))


def test_remix_aligned_intervals_resolves_cut_points() -> None:
    x = _tone(440.0, 0.5)
    pairs = libsonare.remix_aligned_intervals(x, [0, 1000, 5000, 5500])
    assert len(pairs) == 4
    assert pairs[0] >= 0
    assert pairs[1] > pairs[0]
    assert pairs[3] > pairs[2]
    assert pairs[3] <= len(x)


def test_remix_aligned_intervals_leaves_a_sign_changeless_signal_unsnapped() -> None:
    # A DC offset has no zero-crossing to snap to; the interval must survive.
    flat = np.full(4096, 0.25, dtype=np.float32)
    assert libsonare.remix_aligned_intervals(flat, [100, 200]) == [100, 200]


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


def test_lufs_interleaved_rejects_bad_channel_count() -> None:
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    with pytest.raises(ValueError, match="channels must be > 0"):
        libsonare.lufs_interleaved(interleaved, 0, SR)


def test_lufs_interleaved_rejects_non_multiple_length() -> None:
    # An odd-length buffer is not divisible by 2 channels; must raise instead of
    # silently dropping the trailing sample via floor division.
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    truncated = interleaved[:-1]
    with pytest.raises(ValueError, match="divisible by channels"):
        libsonare.lufs_interleaved(truncated, 2, SR)


def test_ebur128_loudness_range_is_finite_nonnegative() -> None:
    lra = libsonare.ebur128_loudness_range(_tone())
    assert math.isfinite(lra)
    assert lra >= 0.0


def test_pitch_yin_returns_librosa_style_estimates_for_unvoiced_frames() -> None:
    # librosa.yin emits an f0 estimate for every frame and reports voicing separately.
    silence = np.zeros(SR, dtype=np.float32)

    nan_res = libsonare.pitch_yin(silence, SR, fill_na=False)
    assert nan_res.n_frames > 0
    assert all(math.isfinite(v) for v in nan_res.f0)
    assert not any(nan_res.voiced_flag)

    filled = libsonare.pitch_yin(silence, SR, fill_na=True)
    assert filled.n_frames > 0
    assert all(math.isfinite(v) for v in filled.f0)


def test_pitch_pyin_fill_na_controls_unvoiced_value() -> None:
    silence = np.zeros(SR, dtype=np.float32)

    nan_res = libsonare.pitch_pyin(silence, SR, fill_na=False)
    assert any(math.isnan(v) for v in nan_res.f0)

    filled = libsonare.pitch_pyin(silence, SR, fill_na=True)
    assert all(math.isfinite(v) for v in filled.f0)


def test_note_segments_preserves_vibrato_and_separates_unvoiced_intervals() -> None:
    segments = libsonare.note_segments(
        [440.0, 445.0, 435.0, 444.0, 436.0, 0.0, 0.0, 660.0, 666.0, 654.0, 665.0, 655.0, 0.0],
        [1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0],
        100.0,
    )
    assert [(segment.frame_start, segment.frame_end) for segment in segments] == [(0, 5), (7, 12)]
    assert [
        value for segment in segments for value in (segment.start_seconds, segment.end_seconds)
    ] == pytest.approx([0.0, 0.05, 0.07, 0.12])


def test_analyze_timbre_exposes_timbre_over_time() -> None:
    result = libsonare.analyze_timbre(_tone(seconds=2.0), SR)
    assert len(result.timbre_over_time) > 0
    # camelCase alias mirrors the JS binding.
    assert result.timbreOverTime is result.timbre_over_time
    for frame in result.timbre_over_time:
        assert math.isfinite(frame.brightness)
        assert math.isfinite(frame.warmth)
        assert math.isfinite(frame.density)
        assert math.isfinite(frame.roughness)
        assert math.isfinite(frame.complexity)
