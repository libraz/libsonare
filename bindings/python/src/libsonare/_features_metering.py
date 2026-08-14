"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

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
    _call_float_transform,
    _check,
    _from_c_float_array,
    _get_lib,
    _to_c_float_array,
    _validate_samples,
    _validate_scalar,
)
from .types import (
    ClippingRegion,
    ClippingReport,
    DynamicRangeReport,
    LufsResult,
    PhaseScopeReport,
    SpectrumReport,
    VectorscopeReport,
    WaveformPeaksReport,
)


def lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> LufsResult:
    """Compute integrated/momentary/short-term LUFS and loudness range.

    Pass the buffer's actual ``sample_rate``: the default (22050) is non-standard
    for audio, and ITU-R BS.1770 K-weighting is sample-rate dependent, so a wrong
    rate yields wrong loudness.
    """
    sample_buf = _validate_samples("lufs", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareLufsResult()
    rc = lib.sonare_lufs(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    return LufsResult(
        integrated_lufs=float(out.integrated_lufs),
        momentary_lufs=float(out.momentary_lufs),
        short_term_lufs=float(out.short_term_lufs),
        max_momentary_lufs=float(out.max_momentary_lufs),
        max_short_term_lufs=float(out.max_short_term_lufs),
        loudness_range=float(out.loudness_range),
    )


def momentary_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Compute the per-block momentary LUFS time series.

    Pass the buffer's actual ``sample_rate``: the default (22050) is non-standard
    and K-weighting is sample-rate dependent.
    """
    sample_buf = _validate_samples("momentary_lufs", samples, validate=validate)
    return _call_float_transform(
        "sonare_momentary_lufs",
        sample_buf,
        ctypes.c_int(sample_rate),
    )


def short_term_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Compute the per-block short-term LUFS time series.

    Pass the buffer's actual ``sample_rate``: the default (22050) is non-standard
    and K-weighting is sample-rate dependent.
    """
    sample_buf = _validate_samples("short_term_lufs", samples, validate=validate)
    return _call_float_transform(
        "sonare_short_term_lufs",
        sample_buf,
        ctypes.c_int(sample_rate),
    )


def lufs_interleaved(
    samples: Sequence[float] | list[float],
    channels: int,
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> LufsResult:
    """ITU-R BS.1770-4 multi-channel LUFS over an interleaved buffer.

    Args:
        samples: Interleaved input buffer of ``frames * channels`` values.
        channels: Channel count (must be > 0).
        sample_rate: Sample rate in Hz. The default (22050) is non-standard for
            audio; pass the buffer's actual rate, as K-weighting is sample-rate
            dependent and a wrong rate yields wrong loudness.

    Returns:
        A :class:`LufsResult` with integrated/momentary/short-term LUFS and
        loudness range.
    """
    sample_buf = _validate_samples("lufs_interleaved", samples, validate=validate)
    if channels <= 0:
        raise ValueError("lufs_interleaved: channels must be > 0")
    lib = _get_lib()
    c_array, total = _to_c_float_array(sample_buf)
    if total % channels != 0:
        raise ValueError(
            "lufs_interleaved: interleaved samples length must be divisible by channels"
        )
    frames = total // channels
    out = SonareLufsResult()
    rc = lib.sonare_lufs_interleaved(
        c_array,
        ctypes.c_size_t(frames),
        ctypes.c_int(channels),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    return LufsResult(
        integrated_lufs=float(out.integrated_lufs),
        momentary_lufs=float(out.momentary_lufs),
        short_term_lufs=float(out.short_term_lufs),
        max_momentary_lufs=float(out.max_momentary_lufs),
        max_short_term_lufs=float(out.max_short_term_lufs),
        loudness_range=float(out.loudness_range),
    )


def ebur128_loudness_range(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer.

    Pass the buffer's actual ``sample_rate``: the default (22050) is non-standard
    and K-weighting is sample-rate dependent.
    """
    sample_buf = _validate_samples("ebur128_loudness_range", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_ebur128_loudness_range(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
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
    *,
    validate: bool = True,
) -> float:
    sample_buf = _validate_samples(name, samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out)
    )
    _check(rc)
    return float(out.value)


def metering_peak_db(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """Sample-peak in dBFS over the buffer."""
    return _metering_scalar("sonare_metering_peak_db", samples, sample_rate, validate=validate)


def metering_rms_db(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """RMS level in dBFS over the buffer."""
    return _metering_scalar("sonare_metering_rms_db", samples, sample_rate, validate=validate)


def metering_silence_ratio(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold_db: float = -45.0,
    frame_length: int = 1024,
    hop_length: int = 256,
    *,
    validate: bool = True,
) -> float:
    """Fraction of analysis frames whose RMS is below ``threshold_db``."""
    sample_buf = _validate_samples("metering_silence_ratio", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_silence_ratio(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(threshold_db),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def metering_crest_factor_db(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """Crest factor in dB (peak_db - rms_db)."""
    return _metering_scalar(
        "sonare_metering_crest_factor_db", samples, sample_rate, validate=validate
    )


def metering_crest_factor_db_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """Crest factor in dB across both channels of a stereo pair.

    Takes the peak across both channels and the RMS over both together. An
    out-of-phase pair cancels in the ``0.5 * (left + right)`` downmix
    :func:`metering_crest_factor_db` would need, which understates its RMS and
    so overstates the crest factor.
    """
    left_buf = _validate_samples("metering_crest_factor_db_stereo", left, validate=validate)
    right_buf = _validate_samples("metering_crest_factor_db_stereo", right, validate=validate)
    lib = _get_lib()
    if not hasattr(lib, "sonare_metering_crest_factor_db_stereo"):
        raise RuntimeError("libsonare was built without the stereo crest factor meter")
    left_array, left_length = _to_c_float_array(left_buf)
    right_array, right_length = _to_c_float_array(right_buf)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_crest_factor_db_stereo(
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def metering_dc_offset(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """DC offset (mean) of the buffer in linear amplitude."""
    return _metering_scalar("sonare_metering_dc_offset", samples, sample_rate, validate=validate)


def metering_true_peak_db(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    oversample_factor: int = 4,
    *,
    validate: bool = True,
) -> float:
    """Inter-sample (true) peak in dBFS.

    ``oversample_factor`` must be a power of two in [1, 16]; pass 0 for the
    library default (4).
    """
    sample_buf = _validate_samples("metering_true_peak_db", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_metering_true_peak_db(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(oversample_factor),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def metering_detect_clipping(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold: float = 0.999,
    min_region_samples: int = 1,
    *,
    validate: bool = True,
) -> ClippingReport:
    """Detect contiguous runs of clipped samples."""
    if not isinstance(min_region_samples, int) or min_region_samples < 0:
        raise ValueError("min_region_samples must be a non-negative integer")
    sample_buf = _validate_samples("metering_detect_clipping", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareClippingResult()
    rc = lib.sonare_metering_detect_clipping(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(threshold),
        ctypes.c_size_t(min_region_samples),
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
    *,
    validate: bool = True,
) -> float:
    left_buf = _validate_samples(name, left, validate=validate, arg_name="left")
    right_buf = _validate_samples(name, right, validate=validate, arg_name="right")
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise ValueError(f"{name}: left and right buffers must have the same length")
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        left_array,
        right_array,
        ctypes.c_size_t(left_len),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def metering_stereo_correlation(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """Uncentered correlation (cosine similarity) between equal-length channels."""
    return _stereo_scalar(
        "sonare_metering_stereo_correlation",
        left,
        right,
        sample_rate,
        validate=validate,
    )


def metering_stereo_width(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """Side / mid energy ratio: 0 = pure mono, ~1 = wide stereo."""
    return _stereo_scalar(
        "sonare_metering_stereo_width",
        left,
        right,
        sample_rate,
        validate=validate,
    )


def metering_vectorscope(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    max_points: int = 0,
    *,
    validate: bool = True,
) -> VectorscopeReport:
    """Mid/side point series for a (left, right) stereo pair.

    By default emits one point per input sample. Pass ``max_points`` to
    deterministically decimate the point cloud to a display size (matching the
    Node/WASM ``meteringVectorscope`` shape); ``0`` (or a value >= the buffer
    length) yields full resolution.
    """
    if max_points:
        return metering_vectorscope_decimated(
            left, right, sample_rate, max_points, validate=validate
        )
    left_buf = _validate_samples("metering_vectorscope", left, validate=validate, arg_name="left")
    right_buf = _validate_samples(
        "metering_vectorscope", right, validate=validate, arg_name="right"
    )
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise ValueError("metering_vectorscope: left and right buffers must have the same length")
    out = SonareVectorscopeResult()
    rc = lib.sonare_metering_vectorscope(
        left_array,
        right_array,
        ctypes.c_size_t(left_len),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.point_count)
        if count == 0:
            mid = np.empty(0, dtype=np.float32)
            side = np.empty(0, dtype=np.float32)
        else:
            arr_type = SonareVectorscopePoint * count
            view = arr_type.from_address(ctypes.addressof(out.points.contents))
            mid = np.empty(count, dtype=np.float32)
            side = np.empty(count, dtype=np.float32)
            for i in range(count):
                mid[i] = view[i].mid
                side[i] = view[i].side
        return VectorscopeReport(mid=mid, side=side)
    finally:
        lib.sonare_free_vectorscope_result(ctypes.byref(out))


def metering_vectorscope_decimated(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    max_points: int = 0,
    *,
    validate: bool = True,
) -> VectorscopeReport:
    """Display-sized mid/side vectorscope for a (left, right) stereo pair.

    ``max_points`` upper-bounds the returned point count; pass 0 (or a value
    >= the buffer length) for one point per sample (identical to
    :func:`metering_vectorscope`). Otherwise the input is deterministically
    decimated, keeping the largest-radius sample of each contiguous bucket.
    """
    left_buf = _validate_samples(
        "metering_vectorscope_decimated", left, validate=validate, arg_name="left"
    )
    right_buf = _validate_samples(
        "metering_vectorscope_decimated", right, validate=validate, arg_name="right"
    )
    lib = _get_lib()
    if not hasattr(lib, "sonare_metering_vectorscope_decimated"):
        raise RuntimeError("libsonare was built without sonare_metering_vectorscope_decimated")
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise ValueError(
            "metering_vectorscope_decimated: left and right buffers must have the same length"
        )
    out = SonareVectorscopeResult()
    rc = lib.sonare_metering_vectorscope_decimated(
        left_array,
        right_array,
        ctypes.c_size_t(left_len),
        ctypes.c_int(sample_rate),
        ctypes.c_size_t(max_points),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.point_count)
        if count == 0:
            mid = np.empty(0, dtype=np.float32)
            side = np.empty(0, dtype=np.float32)
        else:
            arr_type = SonareVectorscopePoint * count
            view = arr_type.from_address(ctypes.addressof(out.points.contents))
            mid = np.empty(count, dtype=np.float32)
            side = np.empty(count, dtype=np.float32)
            for i in range(count):
                mid[i] = view[i].mid
                side[i] = view[i].side
        return VectorscopeReport(mid=mid, side=side)
    finally:
        lib.sonare_free_vectorscope_result(ctypes.byref(out))


def metering_phase_scope(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    max_points: int = 0,
    *,
    validate: bool = True,
) -> PhaseScopeReport:
    """Phase-scope point series plus summary stats for a stereo pair.

    By default emits one point per input sample. Pass ``max_points`` to
    deterministically decimate the point cloud to a display size (matching the
    Node/WASM ``meteringPhaseScope`` shape); ``0`` (or a value >= the buffer
    length) yields full resolution. The summary stats are always computed over
    the full-resolution signal.
    """
    if max_points:
        return metering_phase_scope_decimated(
            left, right, sample_rate, max_points, validate=validate
        )
    left_buf = _validate_samples("metering_phase_scope", left, validate=validate, arg_name="left")
    right_buf = _validate_samples(
        "metering_phase_scope", right, validate=validate, arg_name="right"
    )
    lib = _get_lib()
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise ValueError("metering_phase_scope: left and right buffers must have the same length")
    out = SonarePhaseScopeResult()
    rc = lib.sonare_metering_phase_scope(
        left_array,
        right_array,
        ctypes.c_size_t(left_len),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.point_count)
        if count == 0:
            mid = np.empty(0, dtype=np.float32)
            side = np.empty(0, dtype=np.float32)
            radius = np.empty(0, dtype=np.float32)
            angle = np.empty(0, dtype=np.float32)
        else:
            arr_type = SonarePhaseScopePoint * count
            view = arr_type.from_address(ctypes.addressof(out.points.contents))
            mid = np.empty(count, dtype=np.float32)
            side = np.empty(count, dtype=np.float32)
            radius = np.empty(count, dtype=np.float32)
            angle = np.empty(count, dtype=np.float32)
            for i in range(count):
                mid[i] = view[i].mid
                side[i] = view[i].side
                radius[i] = view[i].radius
                angle[i] = view[i].angle_rad
        return PhaseScopeReport(
            mid=mid,
            side=side,
            radius=radius,
            angle_rad=angle,
            correlation=float(out.correlation),
            average_abs_angle_rad=float(out.average_abs_angle_rad),
            max_radius=float(out.max_radius),
        )
    finally:
        lib.sonare_free_phase_scope_result(ctypes.byref(out))


def metering_phase_scope_decimated(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    max_points: int = 0,
    *,
    validate: bool = True,
) -> PhaseScopeReport:
    """Display-sized phase-scope (Lissajous + summary stats) for a stereo pair.

    ``max_points`` upper-bounds the returned point count; pass 0 (or a value
    >= the buffer length) for one point per sample. Otherwise the point cloud is
    deterministically decimated (largest-radius sample per bucket). The summary
    stats are always computed over the full-resolution signal.
    """
    left_buf = _validate_samples(
        "metering_phase_scope_decimated", left, validate=validate, arg_name="left"
    )
    right_buf = _validate_samples(
        "metering_phase_scope_decimated", right, validate=validate, arg_name="right"
    )
    lib = _get_lib()
    if not hasattr(lib, "sonare_metering_phase_scope_decimated"):
        raise RuntimeError("libsonare was built without sonare_metering_phase_scope_decimated")
    left_array, left_len = _to_c_float_array(left_buf)
    right_array, right_len = _to_c_float_array(right_buf)
    if left_len != right_len:
        raise ValueError(
            "metering_phase_scope_decimated: left and right buffers must have the same length"
        )
    out = SonarePhaseScopeResult()
    rc = lib.sonare_metering_phase_scope_decimated(
        left_array,
        right_array,
        ctypes.c_size_t(left_len),
        ctypes.c_int(sample_rate),
        ctypes.c_size_t(max_points),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = int(out.point_count)
        if count == 0:
            mid = np.empty(0, dtype=np.float32)
            side = np.empty(0, dtype=np.float32)
            radius = np.empty(0, dtype=np.float32)
            angle = np.empty(0, dtype=np.float32)
        else:
            arr_type = SonarePhaseScopePoint * count
            view = arr_type.from_address(ctypes.addressof(out.points.contents))
            mid = np.empty(count, dtype=np.float32)
            side = np.empty(count, dtype=np.float32)
            radius = np.empty(count, dtype=np.float32)
            angle = np.empty(count, dtype=np.float32)
            for i in range(count):
                mid[i] = view[i].mid
                side[i] = view[i].side
                radius[i] = view[i].radius
                angle[i] = view[i].angle_rad
        return PhaseScopeReport(
            mid=mid,
            side=side,
            radius=radius,
            angle_rad=angle,
            correlation=float(out.correlation),
            average_abs_angle_rad=float(out.average_abs_angle_rad),
            max_radius=float(out.max_radius),
        )
    finally:
        lib.sonare_free_phase_scope_result(ctypes.byref(out))


def metering_spectrum(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 0,
    apply_octave_smoothing: bool = False,
    octave_fraction: int = 0,
    db_ref: float = 0.0,
    db_amin: float = 0.0,
    *,
    validate: bool = True,
) -> SpectrumReport:
    """Welch-averaged magnitude / power / dB spectrum over a whole mono buffer.

    This is NOT a single-frame snapshot: the signal is split into Hann-windowed,
    50%-overlapping ``n_fft``-length frames whose power spectra are averaged
    across the entire input (Welch's method), so transients are smeared by the
    averaging. For a true single-frame FFT use :func:`metering_spectrum_frame`.

    Pass 0 for ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin`` to use
    the library defaults (2048 / 3 / 1.0 / kEpsilon).
    """
    sample_buf = _validate_samples("metering_spectrum", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareSpectrumResult()
    rc = lib.sonare_metering_spectrum(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(1 if apply_octave_smoothing else 0),
        ctypes.c_int(octave_fraction),
        ctypes.c_float(db_ref),
        ctypes.c_float(db_amin),
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


def metering_spectrum_frame(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_offset: int = 0,
    n_fft: int = 0,
    apply_octave_smoothing: bool = False,
    octave_fraction: int = 0,
    db_ref: float = 0.0,
    db_amin: float = 0.0,
    *,
    validate: bool = True,
) -> SpectrumReport:
    """True single-frame mono magnitude / power / dB spectrum (one Hann-windowed FFT).

    Unlike :func:`metering_spectrum` (Welch-averaged), this is a single
    ``n_fft``-length FFT for spectrum-analyzer "moment" snapshots. The frame
    spans ``[frame_offset, frame_offset + n_fft)``; samples past the end are
    zero-padded. Pass 0 for ``frame_offset`` for the first frame and 0 for
    ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin`` for the library
    defaults (2048 / 3 / 1.0 / kEpsilon).
    """
    sample_buf = _validate_samples("metering_spectrum_frame", samples, validate=validate)
    lib = _get_lib()
    if not hasattr(lib, "sonare_metering_spectrum_frame"):
        raise RuntimeError("libsonare was built without sonare_metering_spectrum_frame")
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareSpectrumResult()
    rc = lib.sonare_metering_spectrum_frame(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_size_t(frame_offset),
        ctypes.c_int(n_fft),
        ctypes.c_int(1 if apply_octave_smoothing else 0),
        ctypes.c_int(octave_fraction),
        ctypes.c_float(db_ref),
        ctypes.c_float(db_amin),
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
        channels=int(out.channels),
        bucket_count=int(out.bucket_count),
        samples_per_bucket=int(out.samples_per_bucket),
    )


def waveform_peaks(
    samples: Sequence[float] | list[float],
    channels: int,
    *,
    samples_per_bucket: int = 512,
    validate: bool = True,
) -> WaveformPeaksReport:
    """Compute per-channel min/max waveform buckets from interleaved audio.

    The returned ``min`` and ``max`` arrays are channel-major:
    ``channel * bucket_count + bucket``.

    Args:
        samples: Interleaved input buffer of ``frames * channels`` values.
        channels: Channel count (must be > 0).
        samples_per_bucket: Bucket width in frames (default 512).
    """
    sample_buf = _validate_samples("waveform_peaks", samples, validate=validate)
    if channels <= 0 or len(sample_buf) % channels != 0:
        raise ValueError("waveform_peaks: samples length must be a multiple of channels")
    if samples_per_bucket <= 0:
        raise ValueError("waveform_peaks: samples_per_bucket must be > 0")
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareWaveformPeaksResult()
    rc = lib.sonare_waveform_peaks(
        c_array,
        ctypes.c_size_t(length // channels),
        ctypes.c_int(channels),
        ctypes.c_size_t(samples_per_bucket),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _waveform_peaks_from_c(out)
    finally:
        lib.sonare_free_waveform_peaks_result(ctypes.byref(out))


def waveform_peak_pyramid(
    samples: Sequence[float] | list[float],
    channels: int,
    *,
    samples_per_bucket_levels: Sequence[int] = (512, 1024, 2048, 4096),
    validate: bool = True,
) -> list[WaveformPeaksReport]:
    """Compute waveform peak buckets for several zoom levels.

    Args:
        samples: Interleaved input buffer of ``frames * channels`` values.
        channels: Channel count (must be > 0).
        samples_per_bucket_levels: Bucket widths in frames, one per zoom level
            (default ``(512, 1024, 2048, 4096)``).
    """
    sample_buf = _validate_samples("waveform_peak_pyramid", samples, validate=validate)
    levels = [int(level) for level in samples_per_bucket_levels]
    if channels <= 0 or len(sample_buf) % channels != 0:
        raise ValueError("waveform_peak_pyramid: samples length must be a multiple of channels")
    if not levels or any(level <= 0 for level in levels):
        raise ValueError(
            "waveform_peak_pyramid: samples_per_bucket_levels must be non-empty and > 0"
        )
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    c_levels = (ctypes.c_size_t * len(levels))(*levels)
    out = SonareWaveformPeakPyramidResult()
    rc = lib.sonare_waveform_peak_pyramid(
        c_array,
        ctypes.c_size_t(length // channels),
        ctypes.c_int(channels),
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


def metering_dynamic_range(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    window_sec: float = 0.0,
    hop_sec: float = 0.0,
    low_percentile: float = -1.0,
    high_percentile: float = -1.0,
    *,
    validate: bool = True,
) -> DynamicRangeReport:
    """Sliding-window dynamic range for mono audio (high_percentile - low_percentile, in dB).

    Pass 0.0 for ``window_sec`` / ``hop_sec`` to use the library default
    (window=3 s, hop=1 s). For ``low_percentile`` / ``high_percentile`` a
    NEGATIVE value (the default ``-1.0``) selects the library default percentiles
    (low=0.10, high=0.95); ``0.0`` is a real request for the 0th percentile (the
    minimum-RMS window), not the default.
    """
    sample_buf = _validate_samples("metering_dynamic_range", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareDynamicRangeResult()
    rc = lib.sonare_metering_dynamic_range(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(window_sec),
        ctypes.c_float(hop_sec),
        ctypes.c_float(low_percentile),
        ctypes.c_float(high_percentile),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        windows = [float(out.window_rms_db[i]) for i in range(int(out.window_count))]
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
    name: str, root: int, mode_mask: int, reference_midi: float, midi: float
) -> float:
    lib = _get_lib()
    out = ctypes.c_float(0.0)
    rc = getattr(lib, name)(
        ctypes.c_int(root),
        ctypes.c_uint16(mode_mask),
        ctypes.c_float(reference_midi),
        ctypes.c_float(midi),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def scale_quantize_midi(
    root: int, mode_mask: int, midi: float, reference_midi: float = 0.0
) -> float:
    """Snap a (possibly fractional) MIDI number to the nearest enabled pitch class.

    ``mode_mask`` is a 12-bit mask; bit ``i`` enables the ``i``-th pitch class
    relative to ``root``. For natural C major use ``0b101010110101``.
    ``reference_midi`` selects the anchor (default A4 = 69).
    """
    midi = _validate_scalar("scale_quantize_midi", midi, "midi")
    reference_midi = _validate_scalar("scale_quantize_midi", reference_midi, "reference_midi")
    return _scale_scalar("sonare_scale_quantize_midi", root, mode_mask, reference_midi, midi)


def scale_correction_semitones(
    root: int, mode_mask: int, midi: float, reference_midi: float = 0.0
) -> float:
    """Return the correction (quantized - input) in semitones."""
    midi = _validate_scalar("scale_correction_semitones", midi, "midi")
    reference_midi = _validate_scalar(
        "scale_correction_semitones", reference_midi, "reference_midi"
    )
    return _scale_scalar("sonare_scale_correction_semitones", root, mode_mask, reference_midi, midi)


def scale_pitch_class_enabled(root: int, mode_mask: int, pitch_class: int) -> bool:
    """Return True if pitch_class (0..11) is enabled by mode_mask relative to root."""
    lib = _get_lib()
    out = ctypes.c_int(0)
    rc = lib.sonare_scale_pitch_class_enabled(
        ctypes.c_int(root),
        ctypes.c_uint16(mode_mask),
        ctypes.c_int(pitch_class),
        ctypes.byref(out),
    )
    _check(rc)
    return bool(out.value)
