"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._ffi import (
    SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
    SONARE_COMPRESSOR_DETECTOR_PEAK,
    SONARE_COMPRESSOR_DETECTOR_RMS,
    SONARE_DECRACKLE_MODE_MEDIAN,
    SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,
    SONARE_DENOISE_MODE_LOG_MMSE,
    SONARE_DENOISE_MODE_MMSE_STSA,
    SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,
    SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA,
    SONARE_DENOISE_NOISE_ESTIMATOR_MCRA,
    SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
    SONARE_TRIM_SILENCE_MODE_LUFS_GATED,
    SONARE_TRIM_SILENCE_MODE_PEAK,
    SonareCompressorConfig,
    SonareDeclickConfig,
    SonareDeclickStereoResult,
    SonareDeclipConfig,
    SonareDeclipStereoResult,
    SonareDecrackleConfig,
    SonareDecrackleStereoResult,
    SonareDehumConfig,
    SonareDenoiseClassicalConfig,
    SonareDereverbClassicalConfig,
    SonareGateConfig,
    SonareRoomEstimate,
    SonareTransientShaperConfig,
    SonareTrimSilenceConfig,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    ErrorCode,
    SonareError,
    SonareValueError,
    _as_float32_buffer,
    _check,
    _float_array_result,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _int_refusal,
    _narrow_int,
    _out_float_array,
    _require_power_of_two,
    _resolve_enum,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
    _validate_scalar,
)
from .types import (
    ClickDetection,
    ClipDetection,
    CrackleDetection,
    DeclickReport,
    DeclickStereoResult,
    DeclipReport,
    DeclipStereoResult,
    DecrackleReport,
    DecrackleStereoResult,
    DereverbClassicalConfig,
    RoomEstimate,
)

_DEFAULT_EFFECT_FRAME_LENGTH = 2048
_DEFAULT_EFFECT_HOP_LENGTH = 512


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return SonareError(
        int(ErrorCode.NOT_SUPPORTED),
        f"libsonare does not export {symbol}; install a matching native library",
    )


def _trim_length(value: int, arg_name: str) -> int:
    """Narrow one ``trim`` window length, refusing anything a C ``int`` would wrap.

    The type and range halves are :func:`_narrow_int`, the check every ``_to_c_*``
    reader runs; positive is this argument's own.
    """
    domain = "must fit in a positive signed 32-bit integer"
    try:
        length = _narrow_int(value, arg_name, _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal("trim", value, arg_name, domain) from exc
    if length <= 0:
        raise SonareValueError(f"trim: {arg_name} {domain}")
    return length


def _validate_trim_options(frame_length: int, hop_length: int) -> tuple[int, int]:
    return _trim_length(frame_length, "frame_length"), _trim_length(hop_length, "hop_length")


def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = 0.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Normalize audio to a target dB level.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Finite target peak at or below 0 dBFS (default 0.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of normalized samples.
    """
    _validate_samples("normalize", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_normalize(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_float(target_db, "target_db"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _float_array_result(out, out_length.value)


def normalize_rms(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = -20.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Normalize audio by RMS level to a target dBFS value.

    This is the RMS counterpart to :func:`normalize`, using the native
    ``sonare_normalize_rms`` entry point and hard-clipping the result to
    ``[-1, 1]``.  A legacy library that lacks this symbol cannot provide the
    same operation, so it raises ``SonareError(NotSupported)``.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Finite RMS target at or below 0 dBFS (default -20.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.
    """
    _validate_samples("normalize_rms", samples, validate=validate)
    target_db = _validate_scalar("normalize_rms", target_db, "target_db")
    target_db_c = ctypes.c_float(target_db).value
    if not np.isfinite(target_db_c):
        raise SonareValueError("normalize_rms: target_db must fit in a finite float32 value")
    if target_db_c > 0.0:
        raise SonareValueError("normalize_rms: target_db must be at or below 0 dBFS")
    lib = _get_lib()
    if not hasattr(lib, "sonare_normalize_rms"):
        raise _unsupported_effect_symbol("sonare_normalize_rms")
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_normalize_rms(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.c_float(target_db_c),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


_DENOISE_MODE_NAMES = {
    "logmmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "log_mmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "lsa": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "mmsestsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "mmse_stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "spectralsubtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "spectral_subtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "ss": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
}

_DENOISE_ESTIMATOR_NAMES = {
    "quantile": SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,  # noqa: F405
    "mcra": SONARE_DENOISE_NOISE_ESTIMATOR_MCRA,  # noqa: F405
    "imcra": SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA,  # noqa: F405
}


def _coerce_denoise_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DENOISE_MODE_NAMES,
        "denoise mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _coerce_denoise_estimator(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DENOISE_ESTIMATOR_NAMES,
        "denoise noise estimator",
        underscore=True,
        strip=True,
        validate_int=True,
    )


@_guard_buffer("samples")
def mastering_repair_declick(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> np.ndarray:
    """Offline LPC-based declicker.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.
    """
    if max_click_samples <= 0:
        raise SonareValueError("max_click_samples must be positive")
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=int(max_click_samples),
        lpc_order=int(lpc_order),
        residual_ratio=float(residual_ratio),
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_mastering_repair_declick(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


def _extract_declick_report(raw: Any) -> DeclickReport:
    detected = raw.detected
    return DeclickReport(
        detected=ClickDetection(
            count=int(detected.count),
            rejected=int(detected.rejected),
            longest_run_samples=int(detected.longest_run_samples),
            per_second=float(detected.per_second),
        ),
        repaired_runs=int(raw.repaired_runs),
        repaired_samples=int(raw.repaired_samples),
        linked_runs=int(raw.linked_runs),
        lpc_model_used=bool(raw.lpc_model_used),
    )


@_guard_buffer("left", "right")
def mastering_repair_declick_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> DeclickStereoResult:
    """Declicks a stereo pair, repairing the union of both channels' runs.

    A common-mode click repaired on one side only would move the stereo
    image, so a run either channel's detector selects is repaired in both.
    Only the selection is shared: each channel's fill comes from its own
    samples and its own LPC model, which is why the two reports can differ.
    Merged runs can leave a repaired region longer than ``max_click_samples``
    -- that cap governs what may be selected, not how far a selection reaches
    once both channels agree.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        :class:`DeclickStereoResult` with the declicked channels and each
        channel's own detection/repair report.
    """
    if max_click_samples <= 0:
        raise SonareValueError("max_click_samples must be positive")
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=int(max_click_samples),
        lpc_order=int(lpc_order),
        residual_ratio=float(residual_ratio),
    )
    out = SonareDeclickStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_declick_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DeclickStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_declick_report(out.left_report),
            right_report=_extract_declick_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDeclickStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


@_guard_buffer("samples")
def mastering_repair_denoise_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> np.ndarray:
    """Offline STFT-domain classical denoiser.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, or ``"imcra"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        reduction_db: Deepest attenuation the mask may apply, in dB, >= 0
            (default 26.0).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``mode`` / ``noise_estimator`` cannot be resolved.
        RuntimeError: If the C call rejects the request (e.g. non-power-of-two
        ``n_fft`` or non-positive ``hop_length``).
    """
    # The core requires a power of two here (denoise_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    _require_power_of_two(n_fft, "n_fft")
    if hop_length <= 0:
        raise SonareValueError("hop_length must be positive")

    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        dd_alpha=float(dd_alpha),
        reduction_db=float(reduction_db),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_mastering_repair_denoise_classical(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


_DECRACKLE_MODE_NAMES = {
    "median": SONARE_DECRACKLE_MODE_MEDIAN,  # noqa: F405
    "waveletshrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet_shrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
}

_TRIM_SILENCE_MODE_NAMES = {
    "peak": SONARE_TRIM_SILENCE_MODE_PEAK,  # noqa: F405
    "lufsgated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs_gated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
}


def _coerce_decrackle_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DECRACKLE_MODE_NAMES,
        "decrackle mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _coerce_trim_silence_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _TRIM_SILENCE_MODE_NAMES,
        "trim_silence mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _run_repair(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
) -> np.ndarray:
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    with _out_float_array(lib) as (out, out_length):
        rc = lib_fn(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


@_guard_buffer("samples")
def mastering_repair_declip(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> np.ndarray:
    """Offline LPC-based declipper.

    Only clipped runs of at most 512 consecutive samples are reconstructed with the
    LPC solver. The cap is a fixed sample count: it is not derived from ``lpc_order``,
    from ``sample_rate``, or from any other argument, so its duration depends on the
    rate (~10.7 ms at 48 kHz). A longer run is filled with cubic / linear interpolation
    instead, which keeps the solver's dense matrices bounded by the cap rather than by
    the input. Exceeding the cap silently changes the reconstruction method rather than
    raising: ``lpc_order``, ``iterations`` and ``lpc_blend`` have no effect on the
    interpolated run.
    """
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=int(lpc_order),
        iterations=int(iterations),
        lpc_blend=float(lpc_blend),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_declip, samples, sample_rate, config)


def _extract_declip_report(raw: Any) -> DeclipReport:
    detected = raw.detected
    return DeclipReport(
        detected=ClipDetection(
            sample_count=int(detected.sample_count),
            sample_fraction=float(detected.sample_fraction),
            run_count=int(detected.run_count),
            longest_run_samples=int(detected.longest_run_samples),
        ),
        lpc_reconstructed_runs=int(raw.lpc_reconstructed_runs),
        interpolated_runs=int(raw.interpolated_runs),
        repaired_samples=int(raw.repaired_samples),
        linked_runs=int(raw.linked_runs),
    )


@_guard_buffer("left", "right")
def mastering_repair_declip_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> DeclipStereoResult:
    """Declips a stereo pair over the union of both channels' clipped runs.

    Each channel reconstructs the whole of every union run it has at least
    one clipped sample in; a channel with none in a run is left untouched
    there -- reconstructing unclipped audio to match the other side would
    replace real samples with an estimate, so this is not
    :func:`mastering_repair_declick_stereo`'s behaviour. A clipped plateau
    present in only one channel therefore produces no linking at all: linking
    needs both channels clipped in the same region with different extents.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        clip_threshold: Amplitude above which a sample is considered clipped
            (default 0.98).
        lpc_order: LPC order used for prediction (default 36).
        iterations: LPC reconstruction iterations (default 2).
        lpc_blend: LPC vs interpolation blend (default 0.65).

    Returns:
        :class:`DeclipStereoResult` with the declipped channels and each
        channel's own detection/repair report.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=int(lpc_order),
        iterations=int(iterations),
        lpc_blend=float(lpc_blend),
    )
    out = SonareDeclipStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_declip_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DeclipStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_declip_report(out.left_report),
            right_report=_extract_declip_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDeclipStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


@_guard_buffer("samples")
def mastering_repair_decrackle(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> np.ndarray:
    """Offline crackle suppressor (median or wavelet-shrinkage)."""
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        levels=int(levels),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_decrackle, samples, sample_rate, config)


def _extract_decrackle_report(raw: Any) -> DecrackleReport:
    detected = raw.detected
    return DecrackleReport(
        detected=CrackleDetection(
            sample_count=int(detected.sample_count),
            sample_fraction=float(detected.sample_fraction),
            per_second=float(detected.per_second),
        ),
        replaced_samples=int(raw.replaced_samples),
        detail_coefficients=int(raw.detail_coefficients),
        shrunk_coefficients=int(raw.shrunk_coefficients),
        noise_sigma=float(raw.noise_sigma),
    )


@_guard_buffer("left", "right")
def mastering_repair_decrackle_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> DecrackleStereoResult:
    """Decrackles a stereo pair, each channel on its own.

    Crackle is surface damage landing at different instants in each channel,
    so unlike :func:`mastering_repair_declick_stereo` and
    :func:`mastering_repair_declip_stereo` there is no shared run for the two
    channels to agree about: this is two independent mono passes over a
    shared, validated config, and each channel's report is its own.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Median mode: deviation threshold. Wavelet mode: a cap on
            the BayesShrink threshold, which only binds when the configured
            value is below what BayesShrink computes from the signal itself
            (default 0.4).
        mode: ``"median"`` (default) or ``"waveletShrinkage"``; an integer in
            ``SONARE_DECRACKLE_MODE_*`` is also accepted.
        levels: Wavelet mode: number of Haar decomposition levels (default 4).

    Returns:
        :class:`DecrackleStereoResult` with the decrackled channels and each
        channel's own detection/repair report.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        levels=int(levels),
    )
    out = SonareDecrackleStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_decrackle_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DecrackleStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_decrackle_report(out.left_report),
            right_report=_extract_decrackle_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDecrackleStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


@_guard_buffer("samples")
def mastering_repair_dehum(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
) -> np.ndarray:
    """Offline mains-hum remover."""
    config = SonareDehumConfig(  # noqa: F405
        fundamental_hz=float(fundamental_hz),
        harmonics=int(harmonics),
        q=float(q),
        adaptive=1 if adaptive else 0,
        search_range_hz=float(search_range_hz),
        adaptation=float(adaptation),
        frame_size=int(frame_size),
        pll_bandwidth=float(pll_bandwidth),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_dehum, samples, sample_rate, config)


@_guard_buffer("samples")
def mastering_repair_dereverb_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> np.ndarray:
    """Offline classical dereverberator (spectral subtraction + optional WPE)."""
    # The core requires a power of two here (dereverb_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    _require_power_of_two(n_fft, "n_fft")
    if hop_length <= 0 or hop_length > n_fft:
        raise SonareValueError("hop_length must be in (0, n_fft]")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=int(wpe_iterations),
        wpe_taps=int(wpe_taps),
        wpe_strength=float(wpe_strength),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_dereverb_classical, samples, sample_rate, config
    )


def mastering_repair_dereverb_config_for_room(
    estimate: RoomEstimate,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbClassicalConfig:
    """Point a dereverb config at a measured room.

    The pair to :func:`libsonare.estimate_room`, which measures a recording
    blind. Returns a complete config for
    :func:`mastering_repair_dereverb_classical`, so the caller does not have to
    know which reverberation-time band to use or how the late delay relates to
    room size::

        estimate = libsonare.estimate_room(samples, sr)
        config = libsonare.mastering_repair_dereverb_config_for_room(estimate)
        clean = libsonare.mastering_repair_dereverb_classical(samples, sr, **config)

    What the room decides is *where* the tail is. Exactly two keys come back
    changed from what was passed in:

    * ``t60_sec`` -- the mid-frequency reverberation time, the average of the
      500 Hz and 1 kHz octaves an ISO 3382 room is quoted by.
    * ``late_delay_ms`` -- Polack's mixing time, sqrt(volume) in milliseconds,
      past which the response is a diffuse tail rather than separable early
      reflections.

    When NEITHER mid band converged, ``t60_sec`` falls back to the average of
    whatever bands did, so a low-band-only estimate configures something rather
    than nothing -- that value is no longer a mid-frequency figure. Only
    ``volume``, ``rt60_bands`` and the band count are read; the rest of the
    estimate is ignored.

    How *much* to remove is taste rather than measurement, so ``attenuation``,
    ``threshold``, ``over_subtraction`` and ``spectral_floor`` are never
    written; they come back as the float32 image of what was passed in, which
    is the value the dereverb call would have used anyway. A measurement that
    did not converge leaves its own key
    alone, so a partial estimate still configures the half it measured; a
    low-``confidence`` estimate is still applied, because whether to trust it is
    the caller's call.

    Args:
        estimate: The measured room, from :func:`libsonare.estimate_room`. Only
            ``volume`` and ``rt60_bands`` are read.
        threshold, attenuation, n_fft, hop_length, t60_sec, late_delay_ms,
            over_subtraction, spectral_floor, wpe_enabled, wpe_iterations,
            wpe_taps, wpe_strength: The config to point at the room. Every one
            defaults to the library's own dereverb default, matching
            :func:`mastering_repair_dereverb_classical`, so calling this with
            only ``estimate`` returns a config that is ready to run. The C ABI
            underneath reads and writes the whole config and takes every field
            literally -- it has no "zero means default" rule -- which is why
            these defaults are spelled out here instead of left at zero.

    Returns:
        A complete :class:`~libsonare.types.DereverbClassicalConfig` to splat
        into :func:`mastering_repair_dereverb_classical`.
    """
    lib = _get_lib()
    symbol = "sonare_mastering_repair_dereverb_apply_room_estimate"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    if estimate is None:
        raise SonareValueError(
            "mastering_repair_dereverb_config_for_room: estimate must not be None"
        )
    bands, band_count = _to_c_float_array(estimate.rt60_bands, arg_name="estimate.rt60_bands")
    c_estimate = SonareRoomEstimate(
        volume=float(estimate.volume),
        rt60_bands=ctypes.cast(bands, ctypes.POINTER(ctypes.c_float)),
        band_count=band_count,
    )
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=int(wpe_iterations),
        wpe_taps=int(wpe_taps),
        wpe_strength=float(wpe_strength),
    )
    _check(
        getattr(lib, symbol)(ctypes.byref(c_estimate), ctypes.byref(config)),
    )
    return DereverbClassicalConfig(
        threshold=float(config.threshold),
        attenuation=float(config.attenuation),
        n_fft=int(config.n_fft),
        hop_length=int(config.hop_length),
        t60_sec=float(config.t60_sec),
        late_delay_ms=float(config.late_delay_ms),
        over_subtraction=float(config.over_subtraction),
        spectral_floor=float(config.spectral_floor),
        wpe_enabled=bool(config.wpe_enabled),
        wpe_iterations=int(config.wpe_iterations),
        wpe_taps=int(config.wpe_taps),
        wpe_strength=float(config.wpe_strength),
    )


@_guard_buffer("samples")
def mastering_repair_trim_silence(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> np.ndarray:
    """Offline silence trimmer (peak threshold or LUFS-gated)."""
    if padding_samples < 0:
        raise SonareValueError("padding_samples must be non-negative")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        padding_samples=int(padding_samples),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_trim_silence, samples, sample_rate, config
    )


def trim(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold_db: float = -60.0,
    frame_length: int = _DEFAULT_EFFECT_FRAME_LENGTH,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    *,
    validate: bool = True,
) -> list[float]:
    """Trim silence from the beginning and end of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Silence threshold in dB (default -60.0).
        frame_length: RMS analysis frame length in samples (default 2048).
        hop_length: RMS analysis hop length in samples (default 512).
        validate: Reject empty / NaN / Inf input (default ``True``).

    Returns:
        List of trimmed samples.
    """
    _validate_samples("trim", samples, validate=validate)
    frame_length, hop_length = _validate_trim_options(frame_length, hop_length)
    lib = _get_lib()
    if not hasattr(lib, "sonare_trim_ex"):
        if frame_length != _DEFAULT_EFFECT_FRAME_LENGTH or hop_length != _DEFAULT_EFFECT_HOP_LENGTH:
            raise _unsupported_effect_symbol("sonare_trim_ex")
        if not hasattr(lib, "sonare_trim"):
            raise _unsupported_effect_symbol("sonare_trim")
        fn_name = "sonare_trim"
    else:
        fn_name = "sonare_trim_ex"
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        if fn_name == "sonare_trim_ex":
            rc = lib.sonare_trim_ex(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(threshold_db, "threshold_db"),
                _to_c_int(frame_length, "frame_length"),
                _to_c_int(hop_length, "hop_length"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        else:
            rc = lib.sonare_trim(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(threshold_db, "threshold_db"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        _check(rc)
        return _float_array_result(out, out_length.value)


# ---------------------------------------------------------------------------
# Offline mastering dynamics processors
# ---------------------------------------------------------------------------


_COMPRESSOR_DETECTOR_NAMES: dict[str, int] = {
    "peak": SONARE_COMPRESSOR_DETECTOR_PEAK,
    "rms": SONARE_COMPRESSOR_DETECTOR_RMS,
    "log_rms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
    "logrms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
}


def _coerce_compressor_detector(value: int | str) -> int:
    return _resolve_enum(
        value,
        _COMPRESSOR_DETECTOR_NAMES,
        "compressor detector",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _run_dynamics(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
    *,
    fn_name: str = "mastering_dynamics",
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Invoke a dynamics ``(samples, sr, &config, &out, &out_length, &out_latency)``
    C call and return ``(output ndarray, latency_samples)``.
    """
    lib = _get_lib()
    in_buf = _validate_samples(fn_name, samples, validate=validate)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    latency = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib_fn(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(latency),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value), int(latency.value)


def mastering_dynamics_compressor(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: int | str = "rms",
    sidechain_hpf_enabled: bool = False,
    sidechain_hpf_hz: float = 100.0,
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline feed-forward compressor.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Compression threshold in dB (default -18).
        ratio: Compression ratio (clamped to >= 1; default 2.0).
        attack_ms: Attack time in milliseconds (default 10).
        release_ms: Release time in milliseconds (default 100).
        knee_db: Soft-knee width in dB (default 0).
        makeup_gain_db: Static makeup gain in dB (default 0).
        auto_makeup: Whether to enable automatic makeup gain (default False).
        detector: Detector mode, either an alias (``"peak"``, ``"rms"``,
            ``"log_rms"``) or an integer from
            :data:`SONARE_COMPRESSOR_DETECTOR_PEAK` and friends (default ``"rms"``).
        sidechain_hpf_enabled: Whether to high-pass-filter the sidechain
            (default False).
        sidechain_hpf_hz: Sidechain HPF cutoff in Hz (default 100).
        pdr_time_ms: Program-dependent release time in ms (default 0).
        pdr_release_scale: PDR release multiplier (default 1.0).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``. The output is a
        ``numpy.ndarray`` of ``float32`` with ``shape == samples.shape``.
    """
    config = SonareCompressorConfig(
        threshold_db=float(threshold_db),
        ratio=float(ratio),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        knee_db=float(knee_db),
        makeup_gain_db=float(makeup_gain_db),
        auto_makeup=1 if auto_makeup else 0,
        detector=_coerce_compressor_detector(detector),
        sidechain_hpf_enabled=1 if sidechain_hpf_enabled else 0,
        sidechain_hpf_hz=float(sidechain_hpf_hz),
        pdr_time_ms=float(pdr_time_ms),
        pdr_release_scale=float(pdr_release_scale),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_compressor,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_compressor",
        validate=validate,
    )


def mastering_dynamics_gate(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_hpf_hz: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline noise gate.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Open-state threshold in dB (default -50).
        attack_ms: Attack time in milliseconds (default 2).
        release_ms: Release time in milliseconds (default 80).
        range_db: Closed-state attenuation in dB (default -80).
        hold_ms: Minimum open time in milliseconds (default 0).
        close_threshold_db: Hysteresis close-threshold in dB; clamped to
            ``<= threshold_db`` (default -50).
        key_hpf_hz: Sidechain HPF cutoff in Hz (default 0 = disabled).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareGateConfig(
        threshold_db=float(threshold_db),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        range_db=float(range_db),
        hold_ms=float(hold_ms),
        close_threshold_db=float(close_threshold_db),
        key_hpf_hz=float(key_hpf_hz),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_gate,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_gate",
        validate=validate,
    )


def mastering_dynamics_transient_shaper(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    attack_gain_db: float = 3.0,
    sustain_gain_db: float = 0.0,
    fast_attack_ms: float = 0.0,
    fast_release_ms: float = 20.0,
    slow_attack_ms: float = 15.0,
    slow_release_ms: float = 200.0,
    sensitivity: float = 1.0,
    max_gain_db: float = 12.0,
    gain_smoothing_ms: float = 0.0,
    lookahead_ms: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline envelope-difference transient shaper.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        attack_gain_db: Attack-band gain in dB (default +3).
        sustain_gain_db: Sustain-band gain in dB (default 0).
        fast_attack_ms: Fast-envelope attack time in ms (default 0).
        fast_release_ms: Fast-envelope release time in ms (default 20).
        slow_attack_ms: Slow-envelope attack time in ms (default 15).
        slow_release_ms: Slow-envelope release time in ms (default 200).
        sensitivity: Envelope difference sensitivity (clamped >= 0; default 1).
        max_gain_db: Safety clamp on applied gain in dB (default 12).
        gain_smoothing_ms: Gain-signal smoothing time in ms
            (default 0 = disabled).
        lookahead_ms: Lookahead time in ms (default 0 = disabled).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareTransientShaperConfig(
        attack_gain_db=float(attack_gain_db),
        sustain_gain_db=float(sustain_gain_db),
        fast_attack_ms=float(fast_attack_ms),
        fast_release_ms=float(fast_release_ms),
        slow_attack_ms=float(slow_attack_ms),
        slow_release_ms=float(slow_release_ms),
        sensitivity=float(sensitivity),
        max_gain_db=float(max_gain_db),
        gain_smoothing_ms=float(gain_smoothing_ms),
        lookahead_ms=float(lookahead_ms),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_transient_shaper,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_transient_shaper",
        validate=validate,
    )
