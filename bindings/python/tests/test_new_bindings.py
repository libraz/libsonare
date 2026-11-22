"""Smoke tests for the C-ABI functions surfaced through the Python facade.

Each function is implemented + tested at the C ABI; these guard that the Python
facade wiring (ctypes signature, out-buffer copy, free, re-export) is correct and
returns the documented shape with finite values.
"""

from __future__ import annotations

import dataclasses
import math
from collections.abc import Sequence
from typing import Any, cast

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE, raises_coded

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


def test_spectral_contrast_band_narrower_than_a_bin_is_unmeasurable() -> None:
    # At a 64-point transform the bin is 344.5 Hz, so band 0 (below fmin = 200 Hz)
    # is emptied by the overlap trim and has nothing to average. That reaches the
    # caller as NaN rather than a level borrowed from the bins around it.
    matrix = libsonare.spectral_contrast(
        _tone(), SR, n_fft=64, hop_length=16, n_bands=6, fmin=200.0
    )
    assert matrix.shape[0] == 7
    assert matrix.shape[1] > 0

    assert bool(np.all(np.isnan(matrix[0])))
    # Every band wide enough to hold a bin still carries a number, so the
    # assertion above cannot be passing on an all-NaN matrix.
    assert _finite(matrix[1:])


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
    factors = libsonare.decompose(spec, n_features, n_frames, n_components, n_iter=20)
    assert isinstance(factors, libsonare.DecomposeResult)
    with pytest.raises(TypeError):
        iter(factors)
    w, h = factors.w, factors.h
    assert np.asarray(w).size == n_features * n_components
    assert np.asarray(h).size == n_components * n_frames
    assert _finite(w) and _finite(h)


def test_decompose_stems_components_sum_back_to_the_input() -> None:
    x = _tone(440.0, 0.4)
    res = libsonare.decompose_stems(x, SR, n_components=2, n_fft=1024, hop_length=256, n_iter=30)
    components = res.components
    assert len(components) == 2
    assert all(np.asarray(c).shape == x.shape for c in components)
    assert np.asarray(res.w).shape == (1024 // 2 + 1, 2)
    assert np.asarray(res.h).shape[0] == 2
    assert res.sample_rate == SR
    total = sum(np.asarray(c, dtype=np.float32) for c in components)
    interior = slice(1024, len(x) - 1024)
    err = float(np.linalg.norm(total[interior] - x[interior]))
    assert err / float(np.linalg.norm(x[interior])) < 0.05


def test_decompose_stems_rejects_an_out_of_range_mask_power() -> None:
    with pytest.raises(ValueError):
        libsonare.decompose_stems(_tone(440.0, 0.2), SR, mask_power=0.5)


def test_an_omitted_stems_framing_is_the_library_default() -> None:
    """No n_fft / hop_length reaches the core as absent and runs its 2048 / 512 framing."""
    x = _tone(440.0, 0.4)
    common: dict[str, Any] = {"n_components": 2, "n_iter": 10, "init": "nndsvd"}
    omitted = libsonare.decompose_stems(x, SR, **common)
    explicit = libsonare.decompose_stems(x, SR, n_fft=2048, hop_length=512, **common)
    linked = libsonare.decompose_stems_linked([x], SR, **common)
    linked_explicit = libsonare.decompose_stems_linked(
        [x], SR, n_fft=2048, hop_length=512, **common
    )
    for a, b in ((omitted, explicit), (linked, linked_explicit)):
        for key in ("w", "h"):
            np.testing.assert_array_equal(np.asarray(getattr(a, key)), np.asarray(getattr(b, key)))
        for ca, cb in zip(
            cast("Sequence[Any]", a.components),
            cast("Sequence[Any]", b.components),
            strict=True,
        ):
            np.testing.assert_array_equal(np.asarray(ca), np.asarray(cb))
    assert np.asarray(omitted.w).shape == (2048 // 2 + 1, 2)
    with pytest.raises(ValueError, match="hop_length"):
        libsonare.decompose_stems(x, SR, hop_length=0, **common)


@pytest.mark.parametrize(
    ("field", "library_default"),
    [
        ("n_components", 4),
        ("n_iter", 100),
        ("beta", 2.0),
        ("init", "random"),
        ("mask_power", 1.0),
    ],
)
def test_an_omitted_stems_setting_is_the_library_default(
    field: str, library_default: object
) -> None:
    """Leaving a setting out hands the core its own default, which equals the explicit one."""
    x = _tone(440.0, 0.3)
    for fn, signal in ((libsonare.decompose_stems, x), (libsonare.decompose_stems_linked, [x])):
        omitted = fn(signal, SR)  # type: ignore[operator]
        explicit = fn(signal, SR, **{field: library_default})  # type: ignore[operator]
        for key in ("w", "h"):
            np.testing.assert_array_equal(
                np.asarray(getattr(omitted, key)), np.asarray(getattr(explicit, key))
            )


@pytest.mark.parametrize(
    ("kwargs", "field"),
    [
        ({"n_components": 0}, "n_components"),
        ({"n_iter": 0}, "n_iter"),
        ({"mask_power": 0.0}, "mask_power"),
        ({"init": ""}, "init"),
    ],
)
def test_a_stems_sentinel_is_refused_by_name(kwargs: dict[str, Any], field: str) -> None:
    """0 and "" are the C config's default spellings, not values a caller can pass."""
    x = _tone(440.0, 0.2)
    with pytest.raises(SonareValueError, match=field):
        libsonare.decompose_stems(x, SR, **kwargs)
    with pytest.raises(SonareValueError, match=field):
        libsonare.decompose_stems_linked([x], SR, **kwargs)


def _stems_fingerprint(**kwargs: float) -> tuple[int, int, tuple[float, ...]]:
    res = libsonare.decompose_stems(_tone(440.0, 0.37), SR, **kwargs)  # type: ignore[arg-type]
    components = cast("Sequence[Any]", res.components)
    first = np.asarray(components[0], dtype=np.float32)
    return len(components), int(first.size), tuple(float(v) for v in first[:8])


def test_an_explicit_zero_beta_is_itakura_saito_on_decompose_stems() -> None:
    """An explicit beta of 0 is read literally, so it separates apart from the default."""
    assert _stems_fingerprint(beta=0.0) != _stems_fingerprint(beta=2.0)
    assert _stems_fingerprint() == _stems_fingerprint(beta=2.0)


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
    pairs = libsonare.remix_aligned_intervals(
        x, intervals=[[0, 1000], [5000, 5500]], sample_rate=22050
    )
    assert pairs.shape == (2, 2)
    assert pairs[0, 0] >= 0
    assert pairs[0, 1] > pairs[0, 0]
    assert pairs[1, 1] > pairs[1, 0]
    assert pairs[1, 1] <= len(x)


def test_remix_aligned_intervals_leaves_a_sign_changeless_signal_unsnapped() -> None:
    # A DC offset has no zero-crossing to snap to; the interval must survive.
    flat = np.full(4096, 0.25, dtype=np.float32)
    np.testing.assert_array_equal(
        libsonare.remix_aligned_intervals(flat, intervals=[[100, 200]], sample_rate=22050),
        [[100, 200]],
    )


def test_nn_filter_preserves_shape() -> None:
    n_features, n_frames = 12, 20
    rng = np.random.default_rng(1)
    spec = np.abs(rng.standard_normal((n_features, n_frames))).astype(np.float32).ravel()
    out = libsonare.nn_filter(spec, n_features, n_frames)
    assert out.shape == (n_features, n_frames)
    assert _finite(out)


def test_nn_filter_and_decompose_take_none_for_the_library_default() -> None:
    """None is the library's own default; the values pinned here are the core's documented ones."""
    n_features, n_frames = 12, 20
    rng = np.random.default_rng(1)
    spec = np.abs(rng.standard_normal((n_features, n_frames))).astype(np.float32).ravel()
    np.testing.assert_array_equal(
        libsonare.nn_filter(spec, n_features, n_frames),
        libsonare.nn_filter(spec, n_features, n_frames, k=7, width=1),
    )
    for init in (None, "random"):
        omitted = libsonare.decompose(spec, n_features, n_frames, 2, init=init)
        stated = libsonare.decompose(spec, n_features, n_frames, 2, n_iter=50, init=init)
        np.testing.assert_array_equal(omitted.w, stated.w)
        np.testing.assert_array_equal(omitted.h, stated.h)


def test_an_omitted_nmf_init_is_the_library_default_and_an_empty_one_is_refused() -> None:
    """init=None reaches the C ABI as NULL ("random"); "" is an unknown token, not absence."""
    rng = np.random.default_rng(2)
    spec = np.abs(rng.standard_normal((12, 20))).astype(np.float32).ravel()
    omitted = libsonare.decompose(spec, 12, 20, 2, n_iter=5)
    explicit = libsonare.decompose(spec, 12, 20, 2, n_iter=5, init="random")
    np.testing.assert_array_equal(omitted.w, explicit.w)
    np.testing.assert_array_equal(omitted.h, explicit.h)
    with pytest.raises(ValueError, match="init"):
        libsonare.decompose(spec, 12, 20, 2, n_iter=5, init="")  # type: ignore[arg-type]


def test_an_omitted_nn_filter_aggregate_is_the_library_default() -> None:
    """aggregate=None reaches the C ABI as NULL ("mean"); "" is an unknown token, not absence."""
    rng = np.random.default_rng(3)
    spec = np.abs(rng.standard_normal((12, 20))).astype(np.float32).ravel()
    omitted = libsonare.nn_filter(spec, 12, 20)
    np.testing.assert_array_equal(omitted, libsonare.nn_filter(spec, 12, 20, aggregate="mean"))
    assert not np.array_equal(omitted, libsonare.nn_filter(spec, 12, 20, aggregate="max"))
    with pytest.raises(SonareValueError, match="aggregate"):
        libsonare.nn_filter(spec, 12, 20, aggregate="")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("call", "field"),
    [
        (lambda spec: libsonare.nn_filter(spec, 12, 20, k=0), "k"),
        (lambda spec: libsonare.nn_filter(spec, 12, 20, k=-1), "k"),
        (lambda spec: libsonare.nn_filter(spec, 12, 20, width=-1), "width"),
        (lambda spec: libsonare.decompose(spec, 12, 20, 2, n_iter=0), "n_iter"),
    ],
)
def test_a_sentinel_count_is_refused_by_name(call: Any, field: str) -> None:
    """Absence is None; the C ABI's -1 / 0 default spellings are not counts a caller can pass."""
    spec = np.ones(12 * 20, dtype=np.float32)
    with pytest.raises(libsonare.SonareValueError, match=field):
        call(spec)


def test_remix_concatenates_intervals() -> None:
    x = _tone(440.0, 0.5)
    half = len(x) // 2
    # Reverse the two halves.
    out = libsonare.remix(x, [[half, len(x)], [0, half]])
    assert np.asarray(out).size == len(x)
    assert _finite(out)


@pytest.mark.parametrize("intervals", [[0, 100, 200], [0, 100], 5, [[0, 100, 200]]])
def test_remix_refuses_intervals_not_shaped_n_by_2(intervals: Any) -> None:
    x = _tone(440.0, 0.5)
    with raises_coded(match="intervals"):
        libsonare.remix(x, intervals)
    with raises_coded(match="intervals"):
        libsonare.remix_aligned_intervals(x, intervals=intervals, sample_rate=22050)


def test_hpss_with_residual_splits_three_ways() -> None:
    res = libsonare.hpss(_tone(), sample_rate=22050, with_residual=True)
    assert res.residual is not None
    harmonic = np.asarray(res.harmonic, dtype=np.float32)
    percussive = np.asarray(res.percussive, dtype=np.float32)
    residual = np.asarray(res.residual, dtype=np.float32)
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
    with pytest.raises(ValueError, match=r"num_channels must be an integer in \[1, 2147483647\]"):
        libsonare.lufs_interleaved(interleaved, 0, SR)


def test_lufs_interleaved_rejects_non_multiple_length() -> None:
    # An odd-length buffer is not divisible by 2 channels; must raise instead of
    # silently dropping the trailing sample via floor division.
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    truncated = interleaved[:-1]
    with pytest.raises(ValueError, match="samples length must be a multiple of num_channels"):
        libsonare.lufs_interleaved(truncated, 2, SR)


def _noise(seed: int, seconds: float = 4.0, amplitude: float = 0.25) -> np.ndarray:
    """Deterministic white noise; a different ``seed`` is an uncorrelated realization."""
    rng = np.random.default_rng(seed)
    n = int(SR * seconds)
    return (amplitude * rng.uniform(-1.0, 1.0, n)).astype(np.float32)


def test_lufs_series_interleaved_sums_channel_energies() -> None:
    # BS.1770-4 sums the K-weighted per-channel block energies, so two
    # uncorrelated channels at one level read 10*log10(2) above their per-channel
    # mean. A facade that measured each channel and averaged the results in dB
    # would put this difference at exactly 0.
    left = _noise(12345)
    right = _noise(987654321)
    interleaved = np.stack([left, right], axis=1).reshape(-1)

    series = libsonare.lufs_series_interleaved(interleaved, 2, SR)
    assert isinstance(series, libsonare.LufsSeriesResult)
    with pytest.raises(TypeError):
        iter(series)
    momentary, short_term = series.momentary_lufs, series.short_term_lufs
    mono_left = np.asarray(libsonare.momentary_lufs(left, SR), dtype=np.float64)
    mono_right = np.asarray(libsonare.momentary_lufs(right, SR), dtype=np.float64)

    stereo = np.asarray(momentary, dtype=np.float64)
    assert stereo.size == mono_left.size == mono_right.size
    assert _finite(stereo)
    assert np.allclose(stereo - 0.5 * (mono_left + mono_right), 3.0103, atol=0.2)
    # The short-term series is measured over the same buffer at the 3 s window.
    assert len(short_term) > 0
    assert _finite(short_term)


def test_lufs_series_interleaved_matches_mono_meters_for_one_channel() -> None:
    x = _noise(24680, seconds=4.0)
    series = libsonare.lufs_series_interleaved(x, 1, SR)
    momentary, short_term = series.momentary_lufs, series.short_term_lufs
    assert momentary == pytest.approx(libsonare.momentary_lufs(x, SR))
    assert short_term == pytest.approx(libsonare.short_term_lufs(x, SR))


def test_lufs_series_interleaved_returns_empty_series_below_a_window() -> None:
    # 2 s clears the 400 ms momentary window and not the 3 s short-term one; the
    # short window is empty rather than an error.
    x = _noise(13579, seconds=2.0)
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    series = libsonare.lufs_series_interleaved(interleaved, 2, SR)
    momentary, short_term = series.momentary_lufs, series.short_term_lufs
    assert len(momentary) > 0
    assert short_term.size == 0


def test_lufs_series_interleaved_rejects_bad_channel_count() -> None:
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    with pytest.raises(ValueError, match=r"num_channels must be an integer in \[1, 2147483647\]"):
        libsonare.lufs_series_interleaved(interleaved, 0, SR)


def test_lufs_series_interleaved_rejects_non_multiple_length() -> None:
    x = _tone()
    interleaved = np.stack([x, x], axis=1).reshape(-1)
    with pytest.raises(ValueError, match="samples length must be a multiple of num_channels"):
        libsonare.lufs_series_interleaved(interleaved[:-1], 2, SR)


def test_ebur128_loudness_range_is_none_without_a_gated_window() -> None:
    # `_tone()` is shorter than the 3 s short-term window, so no block is gated.
    assert libsonare.ebur128_loudness_range_lu(_tone(), sample_rate=22050) is None


def test_ebur128_loudness_range_is_finite_nonnegative_over_a_long_tone() -> None:
    t = np.arange(5 * 22050) / 22050
    tone = (0.1 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    lra = libsonare.ebur128_loudness_range_lu(tone, sample_rate=22050)
    assert lra is not None
    assert math.isfinite(lra)
    assert lra >= 0.0


def test_pitch_yin_returns_librosa_style_estimates_for_unvoiced_frames() -> None:
    # librosa.yin emits an f0 estimate for every frame and reports voicing separately.
    silence = np.zeros(SR, dtype=np.float32)

    result = libsonare.pitch_yin(silence, SR)
    assert len(result.f0) > 0
    assert all(math.isfinite(v) for v in result.f0)
    assert not any(result.voiced_flag)


def test_pitch_yin_takes_no_fill_na() -> None:
    silence = np.zeros(SR, dtype=np.float32)
    with pytest.raises(TypeError):
        libsonare.pitch_yin(silence, SR, fill_na=True)  # type: ignore[call-arg]
    with libsonare.Audio.from_buffer(silence, SR) as audio, pytest.raises(TypeError):
        audio.pitch_yin(fill_na=True)  # type: ignore[call-arg]


def test_pitch_pyin_fill_na_controls_unvoiced_value() -> None:
    silence = np.zeros(SR, dtype=np.float32)

    nan_res = libsonare.pitch_pyin(silence, SR, fill_na=False)
    assert any(math.isnan(v) for v in nan_res.f0)

    filled = libsonare.pitch_pyin(silence, SR, fill_na=True)
    assert all(math.isfinite(v) for v in filled.f0)


def test_note_segments_preserves_vibrato_and_separates_unvoiced_intervals() -> None:
    segments = libsonare.note_segments(
        [440.0, 445.0, 435.0, 444.0, 436.0, 0.0, 0.0, 660.0, 666.0, 654.0, 665.0, 655.0, 0.0],
        100.0,
        voiced_prob=[1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0],
    )
    assert [(segment.frame_start, segment.frame_end) for segment in segments] == [(0, 5), (7, 12)]
    assert [
        value for segment in segments for value in (segment.start_sec, segment.end_sec)
    ] == pytest.approx([0.0, 0.05, 0.07, 0.12])


def test_analyze_timbre_exposes_timbre_over_time() -> None:
    result = libsonare.analyze_timbre(_tone(seconds=2.0), SR)
    assert len(result.timbre_over_time) > 0
    assert not hasattr(result, "timbreOverTime")
    for frame in result.timbre_over_time:
        assert math.isfinite(frame.brightness)
        assert math.isfinite(frame.warmth)
        assert math.isfinite(frame.density)
        assert math.isfinite(frame.roughness)
        assert math.isfinite(frame.complexity)


def test_stems_and_residual_results_are_records() -> None:
    """The three separation results are frozen records read by attribute, not dicts."""
    x = _tone(440.0, 0.3)
    stems = libsonare.decompose_stems(x, SR, n_components=2, n_iter=5)
    linked = libsonare.decompose_stems_linked([x, x], SR, n_components=2, n_iter=5)
    residual = libsonare.hpss(x, SR, with_residual=True)
    assert isinstance(stems, libsonare.DecomposeStemsResult)
    assert isinstance(linked, libsonare.DecomposeStemsLinkedResult)
    assert isinstance(residual, libsonare.HpssResult)
    assert stems.sample_rate == linked.sample_rate == residual.sample_rate == SR
    assert np.asarray(linked.components[0]).shape == (2, len(x))
    with pytest.raises(dataclasses.FrozenInstanceError):
        residual.sample_rate = 1  # type: ignore[misc]
