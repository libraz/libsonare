"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._runtime import (
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _to_c_float_array,
    _to_c_int_array,
    _validate_samples,
)

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
) -> dict[str, object]:
    """HPSS into harmonic / percussive / residual signals.

    Args:
        samples: Input audio.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Horizontal median filter size (odd, >= 3).
        kernel_percussive: Vertical median filter size (odd, >= 3).

    Returns:
        A dict with ``harmonic`` / ``percussive`` / ``residual`` float32 arrays
        (all the same length) plus ``sampleRate``.
    """
    lib = _get_lib()
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
