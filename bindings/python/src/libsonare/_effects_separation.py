"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from numbers import Integral

import numpy as np

from ._ffi import SonareHpssResult
from ._runtime import (
    ErrorCode,
    SonareError,
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _to_c_float_array,
    _to_c_int_array,
    _validate_samples,
)

_DEFAULT_EFFECT_N_FFT = 2048
_DEFAULT_EFFECT_HOP_LENGTH = 512
_C_INT_MAX = 2**31 - 1


def _validate_effect_fft_options(fn_name: str, n_fft: int, hop_length: int) -> tuple[int, int]:
    if isinstance(n_fft, bool) or not isinstance(n_fft, Integral):
        raise ValueError(f"{fn_name}: n_fft must be an integer")
    if isinstance(hop_length, bool) or not isinstance(hop_length, Integral):
        raise ValueError(f"{fn_name}: hop_length must be an integer")
    n_fft = int(n_fft)
    hop_length = int(hop_length)
    if n_fft <= 0 or n_fft > _C_INT_MAX or (n_fft & (n_fft - 1)) != 0:
        raise ValueError(f"{fn_name}: n_fft must be a positive signed 32-bit power of two")
    if hop_length <= 0 or hop_length > _C_INT_MAX:
        raise ValueError(f"{fn_name}: hop_length must fit in a positive signed 32-bit integer")
    return n_fft, hop_length


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return SonareError(
        int(ErrorCode.NOT_SUPPORTED),
        f"libsonare does not export {symbol}; install a matching native library",
    )


def _validate_hpss_kernel(fn_name: str, value: int, arg_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise ValueError(f"{fn_name}: {arg_name} must be an integer")
    value = int(value)
    if value <= 0 or value > _C_INT_MAX or value % 2 == 0:
        raise ValueError(f"{fn_name}: {arg_name} must be a positive odd signed 32-bit integer")
    return value


# ============================================================================
# Effects - decomposition / separation
# ============================================================================


def decompose(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int = 50,
    beta: float = 2.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Non-negative matrix factorisation (librosa.decompose.decompose).

    Args:
        s: Input spectrogram, flattened row-major ``[n_features x n_frames]``.
        n_features: Feature dimension (rows). Must be > 0.
        n_frames: Number of time frames. Must be > 0.
        n_components: Target number of components (k). Must be > 0.
        n_iter: Number of multiplicative-update iterations (default 50).
        beta: Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).

    Returns:
        Tuple ``(w, h)`` of float32 arrays: ``w`` is the component matrix of
        shape ``(n_features, n_components)`` and ``h`` is the activation matrix
        of shape ``(n_components, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise ValueError("s length must equal n_features * n_frames")
    with (
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose(
                c_array,
                ctypes.c_int(n_features),
                ctypes.c_int(n_frames),
                ctypes.c_int(n_components),
                ctypes.c_int(n_iter),
                ctypes.c_float(beta),
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        w = _from_c_float_array(out_w, out_w_length.value).reshape(n_features, n_components)
        h = _from_c_float_array(out_h, out_h_length.value).reshape(n_components, n_frames)
        return (w, h)


def decompose_with_init(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int = 50,
    beta: float = 2.0,
    init: str = "random",
) -> tuple[np.ndarray, np.ndarray]:
    """NMF with a selectable initialiser (librosa.decompose.decompose, ``init``).

    Identical to :func:`decompose` but exposes the initialisation strategy:
    ``"random"`` (default) or ``"nndsvd"`` (the SVD-based warm start, which tends
    to converge in fewer iterations).

    Returns:
        Tuple ``(w, h)`` of float32 arrays: ``w`` of shape
        ``(n_features, n_components)`` and ``h`` of shape
        ``(n_components, n_frames)``.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_with_init"):
        raise RuntimeError("libsonare was built without sonare_decompose_with_init")
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise ValueError("s length must equal n_features * n_frames")
    with (
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose_with_init(
                c_array,
                ctypes.c_int(n_features),
                ctypes.c_int(n_frames),
                ctypes.c_int(n_components),
                ctypes.c_int(n_iter),
                ctypes.c_float(beta),
                init.encode("utf-8") if init else None,
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        w = _from_c_float_array(out_w, out_w_length.value).reshape(n_features, n_components)
        h = _from_c_float_array(out_h, out_h_length.value).reshape(n_components, n_frames)
        return (w, h)


def nn_filter(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    aggregate: str = "mean",
    k: int = 7,
    width: int = 1,
) -> np.ndarray:
    """Nearest-neighbour spectrogram filter (librosa.decompose.nn_filter).

    Args:
        s: Input spectrogram, flattened row-major ``[n_features x n_frames]``.
        n_features: Feature dimension (rows). Must be > 0.
        n_frames: Number of time frames. Must be > 0.
        aggregate: Aggregator: ``"mean"``, ``"median"``, ``"min"`` or ``"max"``.
        k: Number of nearest neighbours (default 7).
        width: Time exclusion half-width (default 1).

    Returns:
        The smoothed spectrogram as a float32 array of shape
        ``(n_features, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise ValueError("s length must equal n_features * n_frames")
    aggregate_bytes = aggregate.encode("utf-8") if aggregate else None
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_nn_filter(
                c_array,
                ctypes.c_int(n_features),
                ctypes.c_int(n_frames),
                aggregate_bytes,
                ctypes.c_int(k),
                ctypes.c_int(width),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value).reshape(n_features, n_frames)


def remix(
    samples: Sequence[float] | list[float],
    intervals: Sequence[int] | list[int],
    sample_rate: int = 22050,
    align_zeros: bool = False,
) -> np.ndarray:
    """Reorder / concatenate a signal by interval slices (librosa.effects.remix).

    Args:
        samples: Input signal.
        intervals: Flat sequence of ``(start, end)`` pairs (even length).
        sample_rate: Sample rate in Hz (default 22050).
        align_zeros: Snap slice boundaries to zero-crossings (default False).

    Returns:
        The remixed signal as a 1-D float32 array.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    intervals_array, n_ints = _to_c_int_array(intervals)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_remix(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                intervals_array,
                ctypes.c_size_t(n_ints // 2),
                ctypes.c_int(1 if align_zeros else 0),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


def hpss_with_residual(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    hard_mask: bool = False,
    *,
    validate: bool = True,
) -> dict[str, object]:
    """HPSS into harmonic / percussive / residual signals.

    Args:
        samples: Input audio.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Horizontal median filter size (positive odd integer).
        kernel_percussive: Vertical median filter size (positive odd integer).
        n_fft: FFT size used for analysis/synthesis (default 2048).
        hop_length: Hop size used for analysis/synthesis (default 512).
        hard_mask: Use binary harmonic/percussive masks (default ``False``).
        validate: Reject empty / NaN / Inf input (default ``True``).

    Returns:
        A dict with ``harmonic`` / ``percussive`` / ``residual`` float32 arrays
        (all the same length) plus ``sampleRate``.
    """
    _validate_samples("hpss_with_residual", samples, validate=validate)
    n_fft, hop_length = _validate_effect_fft_options("hpss_with_residual", n_fft, hop_length)
    kernel_harmonic = _validate_hpss_kernel(
        "hpss_with_residual", kernel_harmonic, "kernel_harmonic"
    )
    kernel_percussive = _validate_hpss_kernel(
        "hpss_with_residual", kernel_percussive, "kernel_percussive"
    )
    if not isinstance(hard_mask, bool):
        raise ValueError("hpss_with_residual: hard_mask must be a bool")

    lib = _get_lib()
    use_soft_mask = 0 if hard_mask else 1
    if not hasattr(lib, "sonare_hpss_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != _DEFAULT_EFFECT_HOP_LENGTH or hard_mask:
            raise _unsupported_effect_symbol("sonare_hpss_ex")
        if not hasattr(lib, "sonare_hpss_with_residual"):
            raise _unsupported_effect_symbol("sonare_hpss_with_residual")
        return _hpss_with_residual_legacy(
            lib, samples, sample_rate, kernel_harmonic, kernel_percussive
        )

    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    residual = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_hpss_ex(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(kernel_harmonic),
        ctypes.c_int(kernel_percussive),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(use_soft_mask),
        ctypes.c_int(1),
        ctypes.byref(out),
        ctypes.byref(residual),
    )
    try:
        _check(rc)
        n = int(out.length)
        return {
            "harmonic": _from_c_float_array(out.harmonic, n),
            "percussive": _from_c_float_array(out.percussive, n),
            "residual": _from_c_float_array(residual, n),
            "sampleRate": int(out.sample_rate),
        }
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))
        if residual:
            lib.sonare_free_floats(residual)


def _hpss_with_residual_legacy(
    lib: ctypes.CDLL,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int,
    kernel_percussive: int,
) -> dict[str, object]:
    """Call the pre-extended three-way HPSS entry point."""
    c_array, length = _to_c_float_array(samples)
    with (
        _out_float_array(lib) as (out_harmonic, out_length),
        _out_float_array(lib) as (out_percussive, _),
        _out_float_array(lib) as (out_residual, _),
    ):
        out_sample_rate = ctypes.c_int()
        _check(
            lib.sonare_hpss_with_residual(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                ctypes.c_int(kernel_harmonic),
                ctypes.c_int(kernel_percussive),
                ctypes.byref(out_harmonic),
                ctypes.byref(out_percussive),
                ctypes.byref(out_residual),
                ctypes.byref(out_length),
                ctypes.byref(out_sample_rate),
            )
        )
        n = out_length.value
        return {
            "harmonic": _from_c_float_array(out_harmonic, n),
            "percussive": _from_c_float_array(out_percussive, n),
            "residual": _from_c_float_array(out_residual, n),
            "sampleRate": int(out_sample_rate.value),
        }


def phase_vocoder(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    rate: float = 1.0,
    n_fft: int = 2048,
    hop_length: int = 512,
    *,
    validate: bool = True,
) -> np.ndarray:
    """Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).

    Args:
        samples: Input audio.
        sample_rate: Sample rate in Hz (default 22050).
        rate: Time stretch rate (< 1.0 slower, > 1.0 faster). Must be > 0.
        n_fft: FFT size used for analysis/synthesis (default 2048).
        hop_length: Hop length used for analysis/synthesis (default 512).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        The time-stretched audio as a 1-D float32 array.
    """
    _validate_samples("phase_vocoder", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_phase_vocoder(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                ctypes.c_float(rate),
                ctypes.c_int(n_fft),
                ctypes.c_int(hop_length),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)
