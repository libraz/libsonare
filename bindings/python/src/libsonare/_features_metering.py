"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._errors import _entry, _refuse, _unknown_error
from ._ffi import (
    SonareClippingResult,
    SonareDynamicRangeResult,
    SonareLufsResult,
    SonarePhaseScopePoint,
    SonarePhaseScopeResult,
    SonareSpectrumResult,
    SonareVectorscopePoint,
    SonareVectorscopeResult,
    SonareWaveformPeakPyramidResult,
    SonareWaveformPeaksResult,
)
from ._runtime import (
    _C_INT_MAX,
    _SIZE_T_MAX,
    _call_float_array_transform,
    _check,
    _from_c_float_array,
    _get_lib,
    _mode_value,
    _narrow_double,
    _narrow_int,
    _optional_from_c_float_array,
    _out_float_array,
    _resolve_optional_non_negative,
    _resolve_zero_default,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_size_t,
    _to_c_uint16,
    _validate_samples,
    _validate_scalar,
)
from .types import (
    ClippingRegion,
    ClippingReport,
    DynamicRangeReport,
    KeyMode,
    LufsResult,
    LufsSeriesResult,
    PhaseScopeReport,
    SpectrumReport,
    VectorscopeReport,
    WaveformPeaksReport,
)

# The FFT size the library falls back to when ``n_fft`` is omitted. Mirrored here
# so the windowed pre-scan covers exactly the span the call will read; a test
# pins it against the ``n_fft`` the library reports back for an omitted request.
_DEFAULT_SPECTRUM_N_FFT = 2048


def _oversample_factor(oversample_factor: int | None) -> int | float:
    """``None`` is the library default (4); an explicit 0 is refused."""
    return _resolve_zero_default(oversample_factor, "oversample_factor")


def _clipping_args(
    threshold: float | None, min_region_samples: int | None
) -> tuple[int | float, int | float]:
    """The clipping arguments, ``None`` mapped to the C default and 0 refused."""
    return (
        _resolve_zero_default(threshold, "threshold"),
        _resolve_zero_default(min_region_samples, "min_region_samples"),
    )


def _spectrum_args(
    n_fft: int | None, octave_fraction: int | None, db_ref: float | None, db_amin: float | None
) -> tuple[int | float, int | float, int | float, int | float]:
    """The spectrum arguments, ``None`` mapped to the C default and 0 refused."""
    return (
        _resolve_zero_default(n_fft, "n_fft"),
        _resolve_zero_default(octave_fraction, "octave_fraction"),
        _resolve_zero_default(db_ref, "db_ref"),
        _resolve_zero_default(db_amin, "db_amin"),
    )


def _dynamic_range_args(
    window_sec: float | None,
    hop_sec: float | None,
    low_percentile: float | None,
    high_percentile: float | None,
) -> tuple[int | float, int | float, int | float, int | float]:
    """The dynamic-range arguments; percentiles are ranks in [0, 1] with low <= high."""
    for name, value in (("low_percentile", low_percentile), ("high_percentile", high_percentile)):
        if value is not None and not 0.0 <= _narrow_double(value, name) <= 1.0:
            raise _refuse(f"{name} must be in [0, 1]")
    if (
        low_percentile is not None
        and high_percentile is not None
        and low_percentile > high_percentile
    ):
        raise _refuse("low_percentile must not exceed high_percentile")
    return (
        _resolve_zero_default(window_sec, "window_sec"),
        _resolve_zero_default(hop_sec, "hop_sec"),
        _resolve_optional_non_negative(low_percentile, "low_percentile"),
        _resolve_optional_non_negative(high_percentile, "high_percentile"),
    )


@_entry("lufs")
def lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> LufsResult:
    """Compute integrated/momentary/short-term LUFS and loudness range."""
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareLufsResult()
    rc = lib.sonare_lufs(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return LufsResult(
        integrated_lufs=float(out.integrated_lufs),
        momentary_lufs=float(out.momentary_lufs),
        short_term_lufs=float(out.short_term_lufs),
        max_momentary_lufs=float(out.max_momentary_lufs),
        max_short_term_lufs=float(out.max_short_term_lufs),
        loudness_range_lu=float(out.loudness_range_lu),
    )


@_entry("momentary_lufs")
def momentary_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> np.ndarray:
    """Compute the per-block momentary LUFS time series as a float32 array."""
    sample_buf = _validate_samples(samples)
    return _call_float_array_transform(
        "sonare_momentary_lufs",
        sample_buf,
        _to_c_int(sample_rate, "sample_rate"),
    )


@_entry("short_term_lufs")
def short_term_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> np.ndarray:
    """Compute the per-block short-term LUFS time series as a float32 array."""
    sample_buf = _validate_samples(samples)
    return _call_float_array_transform(
        "sonare_short_term_lufs",
        sample_buf,
        _to_c_int(sample_rate, "sample_rate"),
    )


@_entry("lufs_interleaved")
def lufs_interleaved(
    samples: Sequence[float] | list[float],
    num_channels: int,
    sample_rate: int,
) -> LufsResult:
    """ITU-R BS.1770-4 multi-channel LUFS over an interleaved buffer.

    Args:
        samples: Interleaved input buffer of ``frames * num_channels`` values.
        num_channels: Channel count (an integer in ``[1, 2147483647]``).
        sample_rate: Sample rate in Hz; K-weighting depends on it.

    Returns:
        A :class:`LufsResult` with integrated/momentary/short-term LUFS and
        loudness range.
    """
    sample_buf = _validate_samples(samples)
    num_channels = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
    lib = _get_lib()
    c_array, total = _to_c_float_array(sample_buf)
    if total % num_channels != 0:
        raise _refuse("samples length must be a multiple of num_channels")
    frames = total // num_channels
    out = SonareLufsResult()
    rc = lib.sonare_lufs_interleaved(
        c_array,
        _to_c_size_t(frames, "frames"),
        _to_c_int(num_channels, "num_channels"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return LufsResult(
        integrated_lufs=float(out.integrated_lufs),
        momentary_lufs=float(out.momentary_lufs),
        short_term_lufs=float(out.short_term_lufs),
        max_momentary_lufs=float(out.max_momentary_lufs),
        max_short_term_lufs=float(out.max_short_term_lufs),
        loudness_range_lu=float(out.loudness_range_lu),
    )


@_entry("lufs_series_interleaved")
def lufs_series_interleaved(
    samples: Sequence[float] | list[float],
    num_channels: int,
    sample_rate: int,
) -> LufsSeriesResult:
    """Momentary and short-term LUFS series for an interleaved multi-channel buffer.

    Measured with ITU-R BS.1770-4 channel summing, so this is not recoverable
    from :func:`momentary_lufs` / :func:`short_term_lufs`: the standard sums the
    K-weighted per-channel block energies rather than mixing per-channel loudness
    in dB. Both series come out of one K-weighting pass. For ``num_channels == 1``
    they match the mono meters element for element.

    Args:
        samples: Interleaved input buffer of ``frames * num_channels`` values.
        num_channels: Channel count (an integer in ``[1, 2147483647]``).
        sample_rate: Sample rate in Hz; K-weighting depends on it.

    Returns:
        A :class:`LufsSeriesResult` holding the 400 ms and 3 s series in LUFS as
        float32 arrays. A signal shorter than a window yields an empty series for
        it, not an error.
    """
    sample_buf = _validate_samples(samples)
    num_channels = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
    lib = _get_lib()
    c_array, total = _to_c_float_array(sample_buf)
    if total % num_channels != 0:
        raise _refuse("samples length must be a multiple of num_channels")
    frames = total // num_channels
    with (
        _out_float_array(lib) as (out_momentary, momentary_length),
        _out_float_array(lib) as (out_short_term, short_term_length),
    ):
        rc = lib.sonare_lufs_series_interleaved(
            c_array,
            _to_c_size_t(frames, "frames"),
            _to_c_int(num_channels, "num_channels"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(out_momentary),
            ctypes.byref(momentary_length),
            ctypes.byref(out_short_term),
            ctypes.byref(short_term_length),
        )
        _check(rc)
        return LufsSeriesResult(
            momentary_lufs=_optional_from_c_float_array(out_momentary, momentary_length.value),
            short_term_lufs=_optional_from_c_float_array(out_short_term, short_term_length.value),
        )


@_entry("ebur128_loudness_range_lu")
def ebur128_loudness_range_lu(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer."""
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_ebur128_loudness_range_lu(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


# ============================================================================
# Metering — offline scalar / true-peak / clipping / dynamic-range meters
# ============================================================================


def _metering_scalar(
    name: str,
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    # ``name`` is the C symbol; validation messages quote the facade spelling
    # the caller actually invoked.
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("metering_peak_db")
def metering_peak_db(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """Sample-peak in dBFS over the buffer."""
    return _metering_scalar("sonare_metering_peak_db", samples, sample_rate)


@_entry("metering_rms_db")
def metering_rms_db(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """RMS level in dBFS over the buffer."""
    return _metering_scalar("sonare_metering_rms_db", samples, sample_rate)


@_entry("metering_silence_ratio")
def metering_silence_ratio(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    threshold_db: float = -45.0,
    frame_length: int = 1024,
    hop_length: int = 256,
) -> float:
    """Fraction of analysis frames whose RMS is below ``threshold_db``."""
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_silence_ratio(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(threshold_db, "threshold_db"),
        _to_c_int(frame_length, "frame_length"),
        _to_c_int(hop_length, "hop_length"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("metering_crest_factor_db")
def metering_crest_factor_db(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """Crest factor in dB (peak_db - rms_db)."""
    return _metering_scalar("sonare_metering_crest_factor_db", samples, sample_rate)


@_entry("metering_crest_factor_db_stereo")
def metering_crest_factor_db_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """Crest factor in dB across both channels of a stereo pair.

    Takes the peak across both channels and the RMS over both together. An
    out-of-phase pair cancels in the ``0.5 * (left + right)`` downmix
    :func:`metering_crest_factor_db` would need, which understates its RMS and
    so overstates the crest factor.
    """
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left_buf)
    right_array, right_length = _to_c_float_array(right_buf)
    if left_length != right_length:
        raise _refuse("left and right channel lengths must match")
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_crest_factor_db_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("metering_dc_offset")
def metering_dc_offset(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """DC offset (mean) of the buffer in linear amplitude."""
    return _metering_scalar("sonare_metering_dc_offset", samples, sample_rate)


@_entry("metering_true_peak_dbtp")
def metering_true_peak_dbtp(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    oversample_factor: int | None = None,
) -> float:
    """Inter-sample (true) peak in dBFS.

    ``oversample_factor`` must be a power of two in [1, 16]; omit it for 4.
    """
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_true_peak_dbtp(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(_oversample_factor(oversample_factor), "oversample_factor"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("metering_detect_clipping")
def metering_detect_clipping(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    threshold: float | None = None,
    min_region_samples: int | None = None,
) -> ClippingReport:
    """Detect contiguous runs of clipped samples.

    ``threshold`` is the absolute level a clipped sample reaches, in ``(0, 1]``.
    Default: 0.999 (the library's). ``min_region_samples`` is the shortest run
    reported. Default: 1 (the library's). An explicit 0 for either raises
    :class:`SonareValueError`; leave it out for the default.
    """
    c_threshold, c_min_region = _clipping_args(threshold, min_region_samples)
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareClippingResult()
    rc = lib.sonare_metering_detect_clipping(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(c_threshold, "threshold"),
        _to_c_size_t(c_min_region, "min_region_samples"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        regions = [
            ClippingRegion(
                start_sample=int(out.regions[i].start_sample),
                end_sample=int(out.regions[i].end_sample),
                length=int(out.regions[i].length),
                peak=float(out.regions[i].peak),
            )
            for i in range(int(out.region_count))
        ]
        return ClippingReport(
            clipped_samples=int(out.clipped_samples),
            clipping_ratio=float(out.clipping_ratio),
            max_clipped_peak=float(out.max_clipped_peak),
            regions=regions,
        )
    finally:
        lib.sonare_free_clipping_result(ctypes.byref(out))


def _stereo_scalar(
    name: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    # ``name`` is the C symbol; validation messages quote the facade spelling
    # the caller actually invoked.
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise _refuse("left and right buffers must have the same length")
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        left_array,
        right_array,
        _to_c_size_t(left_len, "left_len"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("metering_stereo_correlation")
def metering_stereo_correlation(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """Uncentered correlation (cosine similarity) between equal-length channels."""
    return _stereo_scalar(
        "sonare_metering_stereo_correlation",
        left,
        right,
        sample_rate,
    )


@_entry("metering_stereo_width")
def metering_stereo_width(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
) -> float:
    """Stereo width as ``sqrt(side_energy / mid_energy)``.

    This is the side/mid RMS *amplitude* ratio, not the energy ratio: 0 = pure
    mono, ~1 = wide stereo, larger = increasingly decorrelated / out of phase.
    The range is ``[0, +Infinity)`` -- ``inf`` is returned when mid is silent
    but side is not, and 0 when both are silent. Convert to dB with
    ``20 * log10(value)``; ``10 * log10`` would understate the true energy ratio
    by half.
    """
    return _stereo_scalar(
        "sonare_metering_stereo_width",
        left,
        right,
        sample_rate,
    )


def _scope_point_columns(
    points: Any, count: int, point_type: type[ctypes.Structure]
) -> dict[str, np.ndarray]:
    """Split a C array of scope points into one float32 column per field.

    One shared implementation for every scope read-out, for the same reason the
    planar-channel and clip marshalling paths were each collapsed into one: the
    four entry points here were separate per-point attribute walks, and at the
    omitted-``max_points`` default that is one Python-level attribute access per input
    sample per field. Every field of a scope point is a ``c_float`` and the
    struct carries no padding, so the array is a ``(count, len(fields))``
    float32 matrix and one buffer read replaces the walk.

    The column names come from ``point_type._fields_``, so a field added to the
    ctypes mirror flows through here without a second edit. The returned arrays
    are copies -- the caller frees the C result immediately after.
    """
    names = [name for name, *_ in point_type._fields_]
    if ctypes.sizeof(point_type) != 4 * len(names):
        raise _unknown_error(f"{point_type.__name__} is not a packed float32 record")
    if count == 0:
        return {name: np.empty(0, dtype=np.float32) for name in names}
    block = ctypes.cast(points, ctypes.POINTER(ctypes.c_float * (count * len(names))))
    table = np.frombuffer(block.contents, dtype=np.float32).reshape(count, len(names))
    return {name: np.ascontiguousarray(table[:, index]) for index, name in enumerate(names)}


@_entry("metering_vectorscope")
def metering_vectorscope(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    max_points: int | None = None,
) -> VectorscopeReport:
    """Mid/side point series for a (left, right) stereo pair.

    By default emits one point per input sample. Pass a ``max_points`` of at least 1
    to deterministically decimate the point cloud to a display size (matching
    the Node/WASM ``meteringVectorscope`` shape); a value >= the buffer length
    yields full resolution, and 0 or a negative is refused.
    """
    if max_points is not None:
        return metering_vectorscope_decimated(left, right, sample_rate, max_points)
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise _refuse("left and right buffers must have the same length")
    out = SonareVectorscopeResult()
    rc = lib.sonare_metering_vectorscope(
        left_array,
        right_array,
        _to_c_size_t(left_len, "left_len"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        columns = _scope_point_columns(out.points, int(out.point_count), SonareVectorscopePoint)
        return VectorscopeReport(mid=columns["mid"], side=columns["side"])
    finally:
        lib.sonare_free_vectorscope_result(ctypes.byref(out))


@_entry("metering_vectorscope_decimated")
def metering_vectorscope_decimated(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    max_points: int | None = None,
) -> VectorscopeReport:
    """Display-sized mid/side vectorscope for a (left, right) stereo pair.

    ``max_points`` upper-bounds the returned point count; omit it (or pass a
    value >= the buffer length) for one point per sample (identical to
    :func:`metering_vectorscope`), and 0 or a negative is refused. Otherwise
    the input is deterministically decimated, keeping the largest-radius
    sample of each contiguous bucket.
    """
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise _refuse("left and right buffers must have the same length")
    out = SonareVectorscopeResult()
    rc = lib.sonare_metering_vectorscope_decimated(
        left_array,
        right_array,
        _to_c_size_t(left_len, "left_len"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_size_t(_resolve_zero_default(max_points, "max_points"), "max_points"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        columns = _scope_point_columns(out.points, int(out.point_count), SonareVectorscopePoint)
        return VectorscopeReport(mid=columns["mid"], side=columns["side"])
    finally:
        lib.sonare_free_vectorscope_result(ctypes.byref(out))


@_entry("metering_phase_scope")
def metering_phase_scope(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    max_points: int | None = None,
) -> PhaseScopeReport:
    """Phase-scope point series plus summary stats for a stereo pair.

    By default emits one point per input sample. Pass a ``max_points`` of at least 1
    to deterministically decimate the point cloud to a display size (matching
    the Node/WASM ``meteringPhaseScope`` shape); a value >= the buffer length
    yields full resolution, and 0 or a negative is refused. The summary stats
    are always computed over the full-resolution signal.
    """
    if max_points is not None:
        return metering_phase_scope_decimated(left, right, sample_rate, max_points)
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise _refuse("left and right buffers must have the same length")
    out = SonarePhaseScopeResult()
    rc = lib.sonare_metering_phase_scope(
        left_array,
        right_array,
        _to_c_size_t(left_len, "left_len"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        columns = _scope_point_columns(out.points, int(out.point_count), SonarePhaseScopePoint)
        return PhaseScopeReport(
            mid=columns["mid"],
            side=columns["side"],
            radius=columns["radius"],
            angle_rad=columns["angle_rad"],
            correlation=float(out.correlation),
            average_abs_angle_rad=float(out.average_abs_angle_rad),
            max_radius=float(out.max_radius),
        )
    finally:
        lib.sonare_free_phase_scope_result(ctypes.byref(out))


@_entry("metering_phase_scope_decimated")
def metering_phase_scope_decimated(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    max_points: int | None = None,
) -> PhaseScopeReport:
    """Display-sized phase-scope (Lissajous + summary stats) for a stereo pair.

    ``max_points`` upper-bounds the returned point count; omit it (or pass a
    value >= the buffer length) for one point per sample, and 0 or a negative
    is refused. Otherwise the point cloud is deterministically decimated
    (largest-radius sample per bucket). The summary stats are always computed
    over the full-resolution signal.
    """
    left_buf = _validate_samples(left, arg_name="left")
    right_buf = _validate_samples(right, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise _refuse("left and right buffers must have the same length")
    out = SonarePhaseScopeResult()
    rc = lib.sonare_metering_phase_scope_decimated(
        left_array,
        right_array,
        _to_c_size_t(left_len, "left_len"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_size_t(_resolve_zero_default(max_points, "max_points"), "max_points"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        columns = _scope_point_columns(out.points, int(out.point_count), SonarePhaseScopePoint)
        return PhaseScopeReport(
            mid=columns["mid"],
            side=columns["side"],
            radius=columns["radius"],
            angle_rad=columns["angle_rad"],
            correlation=float(out.correlation),
            average_abs_angle_rad=float(out.average_abs_angle_rad),
            max_radius=float(out.max_radius),
        )
    finally:
        lib.sonare_free_phase_scope_result(ctypes.byref(out))


@_entry("metering_spectrum")
def metering_spectrum(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int | None = None,
    apply_octave_smoothing: bool = False,
    octave_fraction: int | None = None,
    db_ref: float | None = None,
    db_amin: float | None = None,
) -> SpectrumReport:
    """Welch-averaged magnitude / power / dB spectrum over a whole mono buffer.

    This is NOT a single-frame snapshot: the signal is split into Hann-windowed,
    50%-overlapping ``n_fft``-length frames whose power spectra are averaged
    across the entire input (Welch's method), so transients are smeared by the
    averaging. For a true single-frame FFT use :func:`metering_spectrum_frame`.

    Omit ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin`` for the
    library defaults (2048 / 3 / 1.0 / kEpsilon); an explicit 0 is refused.
    """
    n_fft_value, octave_fraction_value, db_ref_value, db_amin_value = _spectrum_args(
        n_fft, octave_fraction, db_ref, db_amin
    )
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareSpectrumResult()
    rc = lib.sonare_metering_spectrum(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft_value, "n_fft"),
        ctypes.c_int(1 if apply_octave_smoothing else 0),
        _to_c_int(octave_fraction_value, "octave_fraction"),
        _to_c_float(db_ref_value, "db_ref"),
        _to_c_float(db_amin_value, "db_amin"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.bin_count)
        frequencies = _from_c_float_array(out.frequencies, count)
        magnitude = _from_c_float_array(out.magnitude, count)
        power = _from_c_float_array(out.power, count)
        db = _from_c_float_array(out.db, count)
        return SpectrumReport(
            frequencies=frequencies,
            magnitude=magnitude,
            power=power,
            db=db,
            n_fft=int(out.n_fft),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_spectrum_result(ctypes.byref(out))


@_entry("metering_spectrum_frame")
def metering_spectrum_frame(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    frame_offset: int = 0,
    n_fft: int | None = None,
    apply_octave_smoothing: bool = False,
    octave_fraction: int | None = None,
    db_ref: float | None = None,
    db_amin: float | None = None,
) -> SpectrumReport:
    """True single-frame mono magnitude / power / dB spectrum (one Hann-windowed FFT).

    Unlike :func:`metering_spectrum` (Welch-averaged), this is a single
    ``n_fft``-length FFT for spectrum-analyzer "moment" snapshots. The frame
    spans ``[frame_offset, frame_offset + n_fft)``; samples past the end are
    zero-padded. ``frame_offset`` 0 is the first frame. Omit ``n_fft`` /
    ``octave_fraction`` / ``db_ref`` / ``db_amin`` for the library defaults
    (2048 / 3 / 1.0 / kEpsilon); an explicit 0 is refused.

    The frame is also the only span validated: a non-finite sample inside it is
    rejected, while one outside it neither reaches the FFT nor refuses the call.
    The emptiness and ``sample_rate`` checks still cover the whole buffer. Cost
    per call is therefore set by ``n_fft`` rather than by the length of the
    buffer, so an analyzer may poll a long recording frame by frame.
    """
    # Narrowed before the scan uses them: these two decide which span is read, so
    # a value that is not a usable index has to be refused as such rather than
    # slicing the buffer with it.
    c_frame_offset = _to_c_size_t(frame_offset, "frame_offset")
    n_fft_value, octave_fraction_value, db_ref_value, db_amin_value = _spectrum_args(
        n_fft, octave_fraction, db_ref, db_amin
    )
    c_n_fft = _to_c_int(n_fft_value, "n_fft")
    sample_buf = _validate_samples(
        samples,
        window=(frame_offset, c_n_fft.value or _DEFAULT_SPECTRUM_N_FFT),
    )
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareSpectrumResult()
    rc = lib.sonare_metering_spectrum_frame(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        c_frame_offset,
        c_n_fft,
        ctypes.c_int(1 if apply_octave_smoothing else 0),
        _to_c_int(octave_fraction_value, "octave_fraction"),
        _to_c_float(db_ref_value, "db_ref"),
        _to_c_float(db_amin_value, "db_amin"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.bin_count)
        return SpectrumReport(
            frequencies=_from_c_float_array(out.frequencies, count),
            magnitude=_from_c_float_array(out.magnitude, count),
            power=_from_c_float_array(out.power, count),
            db=_from_c_float_array(out.db, count),
            n_fft=int(out.n_fft),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_spectrum_result(ctypes.byref(out))


def _waveform_peaks_from_c(out: SonareWaveformPeaksResult) -> WaveformPeaksReport:
    count = int(out.channels) * int(out.bucket_count)
    return WaveformPeaksReport(
        min=_from_c_float_array(out.min, count),
        max=_from_c_float_array(out.max, count),
        num_channels=int(out.channels),
        bucket_count=int(out.bucket_count),
        samples_per_bucket=int(out.samples_per_bucket),
    )


@_entry("waveform_peaks")
def waveform_peaks(
    samples: Sequence[float] | list[float],
    num_channels: int,
    *,
    samples_per_bucket: int = 512,
) -> WaveformPeaksReport:
    """Compute per-channel min/max waveform buckets from interleaved audio.

    The returned ``min`` and ``max`` arrays are channel-major:
    ``channel * bucket_count + bucket``.

    A non-finite sample is rejected by index rather than skipped. A bucket whose
    samples are not finite has no min/max to report, and the ``0``/``0`` it
    would otherwise carry is what a waveform display draws as silence.

    Args:
        samples: Interleaved input buffer of ``frames * num_channels`` values.
        num_channels: Channel count (an integer in ``[1, 2147483647]``).
        samples_per_bucket: Bucket width in frames (default 512).
    """
    sample_buf = _validate_samples(samples)
    num_channels = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
    if len(sample_buf) % num_channels != 0:
        raise _refuse("samples length must be a multiple of num_channels")
    samples_per_bucket = _narrow_int(samples_per_bucket, "samples_per_bucket", 1, _SIZE_T_MAX)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareWaveformPeaksResult()
    rc = lib.sonare_waveform_peaks(
        c_array,
        _to_c_size_t(length // num_channels, "length"),
        _to_c_int(num_channels, "num_channels"),
        _to_c_size_t(samples_per_bucket, "samples_per_bucket"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _waveform_peaks_from_c(out)
    finally:
        lib.sonare_free_waveform_peaks_result(ctypes.byref(out))


@_entry("waveform_peak_pyramid")
def waveform_peak_pyramid(
    samples: Sequence[float] | list[float],
    num_channels: int,
    *,
    samples_per_bucket_levels: Sequence[int] = (512, 1024, 2048, 4096),
) -> list[WaveformPeaksReport]:
    """Compute waveform peak buckets for several zoom levels.

    Shares :func:`waveform_peaks`' bucket kernel, so a non-finite sample is
    rejected here on the same rule.

    Args:
        samples: Interleaved input buffer of ``frames * num_channels`` values.
        num_channels: Channel count (an integer in ``[1, 2147483647]``).
        samples_per_bucket_levels: Bucket widths in frames, one per zoom level
            (default ``(512, 1024, 2048, 4096)``).
    """
    sample_buf = _validate_samples(samples)
    # Narrowed per entry rather than coerced, and ahead of the positivity check
    # below: int(512.7) is 512, a width the call then runs at and reports back as
    # the one asked for, and a width past the size_t ceiling would reach the core
    # as the 0 that check refuses under someone else's name.
    levels = [
        _narrow_int(level, f"samples_per_bucket_levels[{index}]", 1, _SIZE_T_MAX)
        for index, level in enumerate(samples_per_bucket_levels)
    ]
    num_channels = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
    if len(sample_buf) % num_channels != 0:
        raise _refuse("samples length must be a multiple of num_channels")
    if not levels:
        raise _refuse("samples_per_bucket_levels must be non-empty")
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    c_levels = (ctypes.c_size_t * len(levels))(*levels)
    out = SonareWaveformPeakPyramidResult()
    rc = lib.sonare_waveform_peak_pyramid(
        c_array,
        _to_c_size_t(length // num_channels, "length"),
        _to_c_int(num_channels, "num_channels"),
        c_levels,
        ctypes.c_size_t(len(levels)),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        view = out.levels
        return [_waveform_peaks_from_c(view[i]) for i in range(int(out.level_count))]
    finally:
        lib.sonare_free_waveform_peak_pyramid_result(ctypes.byref(out))


@_entry("metering_dynamic_range")
def metering_dynamic_range(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    window_sec: float | None = None,
    hop_sec: float | None = None,
    low_percentile: float | None = None,
    high_percentile: float | None = None,
) -> DynamicRangeReport:
    """Sliding-window dynamic range for mono audio (high_percentile - low_percentile, in dB).

    Omit ``window_sec`` / ``hop_sec`` for the library default (window=3 s,
    hop=1 s); an explicit 0 is refused. ``low_percentile`` / ``high_percentile``
    are ranks in [0, 1] with low at most high; omitted, they are 0.10 / 0.95, and
    ``0.0`` is a real request for the minimum-RMS window.
    """
    window_value, hop_value, low_value, high_value = _dynamic_range_args(
        window_sec, hop_sec, low_percentile, high_percentile
    )
    sample_buf = _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareDynamicRangeResult()
    rc = lib.sonare_metering_dynamic_range(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(window_value, "window_sec"),
        _to_c_float(hop_value, "hop_sec"),
        _to_c_float(low_value, "low_percentile"),
        _to_c_float(high_value, "high_percentile"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        windows = _from_c_float_array(out.window_rms_db, int(out.window_count))
        return DynamicRangeReport(
            dynamic_range_db=float(out.dynamic_range_db),
            low_percentile_db=float(out.low_percentile_db),
            high_percentile_db=float(out.high_percentile_db),
            window_rms_db=windows,
        )
    finally:
        lib.sonare_free_dynamic_range_result(ctypes.byref(out))


# ============================================================================
# Editing — 12-TET scale quantizer for pitch correction targets
# ============================================================================


def _scale_scalar(
    name: str, root: int, mode_mask: int, scale_reference_midi: float, midi: float
) -> float:
    lib = _get_lib()
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        _to_c_int(root, "root"),
        _to_c_uint16(mode_mask, "mode_mask"),
        _to_c_float(scale_reference_midi, "scale_reference_midi"),
        _to_c_float(midi, "midi"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_entry("scale_mask_for_mode")
def scale_mask_for_mode(root: int, mode: KeyMode) -> int:
    """Return the 12-bit scale mask of a mode.

    Bit ``i`` is set when the semitone ``i`` above ``root`` belongs to the scale,
    the layout :func:`scale_quantize_midi` and :func:`pitch_correct_timevarying`
    read. The mask is relative to the root, so it is the same for every ``root``,
    which is still validated. ``mode`` is a :data:`KeyMode` token, as
    :func:`detect_key` reports it (``"major"``, ``"minor"``, ``"dorian"``,
    ``"phrygian"``, ``"lydian"``, ``"mixolydian"``, ``"locrian"``); minor is the
    natural minor.

    Example:
        >>> scale_mask_for_mode(0, "major") == 0b101010110101
        True
    """
    lib = _get_lib()
    out = ctypes.c_uint16(0)
    rc = lib.sonare_scale_mask_for_mode(
        _to_c_int(root, "root"), _to_c_int32(_mode_value(mode), "mode"), ctypes.byref(out)
    )
    _check(rc)
    return int(out.value)


def _scale_reference(value: float | None) -> float:
    """Map an absent reference onto the core's 0 (A4).

    MIDI note 0 is a real anchor the core still reads as A4, so an explicit 0 is
    refused until the core takes it literally.
    """
    if value is None:
        return 0.0
    reference = _validate_scalar(value, "scale_reference_midi")
    if reference == 0.0:
        raise _refuse(
            "scale_reference_midi 0 is temporarily unsupported (use None for the A4 default)"
        )
    return reference


@_entry("scale_quantize_midi")
def scale_quantize_midi(
    root: int, mode_mask: int, midi: float, scale_reference_midi: float | None = None
) -> float:
    """Snap a (possibly fractional) MIDI number to the nearest enabled pitch class.

    ``mode_mask`` is a 12-bit mask; bit ``i`` enables the ``i``-th pitch class
    relative to ``root``. For natural C major use ``0b101010110101``.
    ``scale_reference_midi`` anchors the grid; ``None`` is A4 = 69.
    """
    midi = _validate_scalar(midi, "midi")
    reference = _scale_reference(scale_reference_midi)
    return _scale_scalar("sonare_scale_quantize_midi", root, mode_mask, reference, midi)


@_entry("scale_correction_semitones")
def scale_correction_semitones(
    root: int, mode_mask: int, midi: float, scale_reference_midi: float | None = None
) -> float:
    """Return the correction (quantized - input) in semitones.

    ``scale_reference_midi`` as in :func:`scale_quantize_midi`.
    """
    midi = _validate_scalar(midi, "midi")
    reference = _scale_reference(scale_reference_midi)
    return _scale_scalar("sonare_scale_correction_semitones", root, mode_mask, reference, midi)


@_entry("scale_pitch_class_enabled")
def scale_pitch_class_enabled(root: int, mode_mask: int, pitch_class: int) -> bool:
    """Return True if pitch_class (0..11) is enabled by mode_mask relative to root."""
    lib = _get_lib()
    out = ctypes.c_int(0)
    rc = lib.sonare_scale_pitch_class_enabled(
        _to_c_int(root, "root"),
        _to_c_uint16(mode_mask, "mode_mask"),
        _to_c_int(pitch_class, "pitch_class"),
        ctypes.byref(out),
    )
    _check(rc)
    return bool(out.value)
