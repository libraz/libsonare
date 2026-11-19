"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np
import numpy.typing as npt

from ._errors import _entry, _not_supported, _refuse
from ._ffi import (
    SonareCqtResult,
    SonareInverseResult,
)
from ._runtime import (
    _check,
    _from_c_float_array,
    _from_c_float_matrix,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _resolve_optional_non_negative,
    _resolve_zero_default,
    _to_c_float,
    _to_c_float_array,
    _to_c_float_matrix,
    _to_c_int,
    _to_c_size_t,
    _validate_effect_hop,
)
from .types import CqtResult

# ============================================================================
# Features - Constant-Q / Variable-Q transforms
# ============================================================================


def _cqt_result_from_c(out: SonareCqtResult) -> CqtResult:
    return CqtResult(
        hop_length=int(out.hop_length),
        sample_rate=int(out.sample_rate),
        magnitude=_from_c_float_matrix(out.magnitude, out.n_bins, out.n_frames),
        frequencies=_from_c_float_array(out.frequencies, out.n_bins),
    )


@_entry("cqt")
@_guard_buffer("samples")
def cqt(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the Constant-Q Transform magnitude.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        fmin: Lowest center frequency in Hz (default C1).
        n_bins: Total number of frequency bins (default 84).
        bins_per_octave: Bins per octave (default 12).

    Returns:
        A :class:`CqtResult` with the magnitude of shape ``(n_bins, n_frames)`` and
        the bin frequencies.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_cqt"):
        raise _not_supported("libsonare was built without CQT support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = lib.sonare_cqt(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_int(n_bins, "n_bins"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _cqt_result_from_c(out)
    finally:
        lib.sonare_free_cqt_result(ctypes.byref(out))


def _cqt_variant(
    fn_name: str,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    hop_length: int,
    fmin: float,
    n_bins: int,
    bins_per_octave: int,
) -> CqtResult:
    lib = _get_lib()
    if not hasattr(lib, fn_name):
        raise _not_supported(f"libsonare was built without {fn_name} support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = getattr(lib, fn_name)(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_int(n_bins, "n_bins"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _cqt_result_from_c(out)
    finally:
        lib.sonare_free_cqt_result(ctypes.byref(out))


@_entry("pseudo_cqt")
@_guard_buffer("samples")
def pseudo_cqt(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the pseudo-CQT magnitude approximation."""
    return _cqt_variant("sonare_pseudo_cqt", samples, sr, hop_length, fmin, n_bins, bins_per_octave)


@_entry("hybrid_cqt")
@_guard_buffer("samples")
def hybrid_cqt(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the hybrid CQT magnitude."""
    return _cqt_variant("sonare_hybrid_cqt", samples, sr, hop_length, fmin, n_bins, bins_per_octave)


@_entry("cqt_to_audio")
def cqt_to_audio(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    hop_length: int = 512,
    fmin_hz: float = 32.70319566257483,
    bins_per_octave: int = 12,
    n_iter: int = 32,
) -> np.ndarray:
    """Reconstruct mono audio from a CQT magnitude via Griffin-Lim.

    ``magnitude`` is a 2-D array of shape ``(n_bins, n_frames)``. Returns a float32 array.
    """
    lib = _get_lib()
    c_array, n_bins, n_frames = _to_c_float_matrix(magnitude, arg_name="magnitude")
    length = n_bins * n_frames
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_cqt_to_audio_checked(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(n_bins, "n_bins"),
        _to_c_int(n_frames, "n_frames"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin_hz, "fmin_hz"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        _to_c_int(n_iter, "n_iter"),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        if out:
            lib.sonare_free_floats(out)


# ============================================================================
# Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
# ============================================================================


@_entry("mel_to_stft")
def mel_to_stft(
    mel: npt.ArrayLike,
    sr: int = 22050,
    n_fft: int = 2048,
    fmin: float = 0.0,
    fmax: float | None = None,
    htk: bool = False,
) -> np.ndarray:
    """Approximate inverse of a Mel filterbank (Mel power -> STFT magnitude).

    Args:
        mel: Mel power spectrogram, a 2-D array of shape ``(n_mels, n_frames)``.
        sr: Sample rate that produced ``mel`` in Hz (default 22050).
        n_fft: FFT size of the source STFT; sets output bins to ``n_fft/2 + 1``
            (default 2048).
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (default None = sr/2).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        A float32 array of shape ``(n_fft // 2 + 1, n_frames)`` holding the
        reconstructed STFT magnitude (not squared).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_stft"):
        raise _not_supported("libsonare was built without inverse-reconstruction support")
    c_array, n_mels, n_frames = _to_c_float_matrix(mel, arg_name="mel")
    out = SonareInverseResult()
    if htk:
        if not hasattr(lib, "sonare_mel_to_stft_ex"):
            raise _not_supported("libsonare was built without HTK inverse-reconstruction support")
        rc = lib.sonare_mel_to_stft_ex(
            c_array,
            _to_c_int(n_mels, "n_mels"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(sr, "sr"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_float(fmin, "fmin"),
            _to_c_float(_resolve_zero_default(fmax, "fmax"), "fmax"),
            ctypes.c_int(1),
            ctypes.byref(out),
        )
    else:
        rc = lib.sonare_mel_to_stft(
            c_array,
            _to_c_int(n_mels, "n_mels"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(sr, "sr"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_float(fmin, "fmin"),
            _to_c_float(_resolve_zero_default(fmax, "fmax"), "fmax"),
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return _from_c_float_matrix(out.data, int(out.rows), int(out.n_frames))
    finally:
        lib.sonare_free_inverse_result(ctypes.byref(out))


def _inverse_audio(
    lib: ctypes.CDLL,
    base_name: str,
    extended_name: str,
    c_array: object,
    args: tuple[object, ...],
    n_iter: int,
    htk: bool,
) -> np.ndarray:
    """Call a Griffin-Lim inverse wrapper with shared HTK/output handling."""
    function_name = extended_name if htk else base_name
    if not hasattr(lib, function_name):
        feature = "HTK inverse-reconstruction" if htk else "inverse-reconstruction"
        raise _not_supported(f"libsonare was built without {feature} support")
    htk_arg = (ctypes.c_int(1),) if htk else ()
    with _out_float_array(lib) as (out, out_length):
        _check(
            getattr(lib, function_name)(
                c_array,
                *args,
                *htk_arg,
                _to_c_int(n_iter, "n_iter"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("mel_to_audio")
def mel_to_audio(
    mel: npt.ArrayLike,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int | None = None,
    fmin: float = 0.0,
    fmax: float | None = None,
    n_iter: int = 32,
    htk: bool = False,
) -> np.ndarray:
    """Reconstruct audio from a Mel spectrogram via Griffin-Lim.

    Args:
        mel: Mel power spectrogram, a 2-D array of shape ``(n_mels, n_frames)``.
        sr: Sample rate of the original audio in Hz (default 22050).
        n_fft: FFT size used for reconstruction (default 2048).
        hop_length: Hop length used for reconstruction. Default: ``n_fft / 4``,
            derived by the library.
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (default None = sr/2).
        n_iter: Griffin-Lim iterations (default 32).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        The reconstructed audio samples as a float32 array.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_audio"):
        raise _not_supported("libsonare was built without inverse-reconstruction support")
    c_array, n_mels, n_frames = _to_c_float_matrix(mel, arg_name="mel")
    return _inverse_audio(
        lib,
        "sonare_mel_to_audio",
        "sonare_mel_to_audio_ex",
        c_array,
        (
            _to_c_int(n_mels, "n_mels"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(sr, "sr"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_int(
                0 if hop_length is None else _validate_effect_hop(hop_length),
                "hop_length",
            ),
            _to_c_float(fmin, "fmin"),
            _to_c_float(_resolve_zero_default(fmax, "fmax"), "fmax"),
        ),
        n_iter,
        htk,
    )


@_entry("griffin_lim")
def griffin_lim(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int = 32,
    momentum: float = 0.99,
) -> np.ndarray:
    """Reconstruct audio from an STFT magnitude of shape ``(n_bins, n_frames)``.

    ``n_fft`` is implied by the bin count, ``(n_bins - 1) * 2``; omitted, it is
    inferred, and an explicit value that disagrees raises ``SonareValueError``.
    An omitted ``hop_length`` is ``n_fft / 4``, derived by the library.
    Returns a float32 array.
    """
    lib = _get_lib()
    data, n_bins, n_frames = _to_c_float_matrix(magnitude, arg_name="magnitude")
    implied = (n_bins - 1) * 2
    if n_fft is None:
        n_fft = implied
    elif n_fft != implied:
        raise _refuse(f"n_fft must equal (n_bins - 1) * 2 (got {n_fft} for {n_bins} bins)")
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    length = n_bins * n_frames
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_griffin_lim(
                data,
                _to_c_size_t(length, "length"),
                _to_c_int(n_bins, "n_bins"),
                _to_c_int(n_frames, "n_frames"),
                _to_c_int(n_fft, "n_fft"),
                _to_c_int(hop_length, "hop_length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(n_iter, "n_iter"),
                _to_c_float(momentum, "momentum"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("mfcc_to_mel")
def mfcc_to_mel(
    coefficients: npt.ArrayLike,
    n_mels: int = 128,
    lifter: float = 0.0,
) -> np.ndarray:
    """Invert MFCC coefficients back to a Mel power spectrogram.

    Args:
        coefficients: MFCC matrix, a 2-D array of shape ``(n_mfcc, n_frames)``.
        n_mels: Number of Mel bins to reconstruct (default 128).
        lifter: Cepstral lifter the coefficients were computed with (default 0.0).

    Returns:
        A float32 array of shape ``(n_mels, n_frames)`` with the reconstructed Mel power.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mfcc_to_mel"):
        raise _not_supported("libsonare was built without inverse-reconstruction support")
    c_array, n_mfcc, n_frames = _to_c_float_matrix(coefficients, arg_name="coefficients")
    out = SonareInverseResult()
    if hasattr(lib, "sonare_mfcc_to_mel_ex"):
        rc = lib.sonare_mfcc_to_mel_ex(
            c_array,
            _to_c_int(n_mfcc, "n_mfcc"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(n_mels, "n_mels"),
            _to_c_float(lifter, "lifter"),
            ctypes.byref(out),
        )
    else:
        if lifter != 0.0:
            raise _not_supported("this libsonare build does not support inverse MFCC liftering")
        rc = lib.sonare_mfcc_to_mel(
            c_array,
            _to_c_int(n_mfcc, "n_mfcc"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(n_mels, "n_mels"),
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return _from_c_float_matrix(out.data, int(out.rows), int(out.n_frames))
    finally:
        lib.sonare_free_inverse_result(ctypes.byref(out))


@_entry("mfcc_to_audio")
def mfcc_to_audio(
    coefficients: npt.ArrayLike,
    n_mels: int = 128,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int | None = None,
    fmin: float = 0.0,
    fmax: float | None = None,
    n_iter: int = 32,
    htk: bool = False,
    lifter: float = 0.0,
) -> np.ndarray:
    """Reconstruct audio directly from MFCC via Mel inversion + Griffin-Lim.

    Args:
        coefficients: MFCC matrix, a 2-D array of shape ``(n_mfcc, n_frames)``.
        n_mels: Number of Mel bins (must match the MFCC source config).
        sr: Sample rate of the original audio in Hz (default 22050).
        n_fft: FFT size used for reconstruction (default 2048).
        hop_length: Hop length used for reconstruction. Default: ``n_fft / 4``,
            derived by the library.
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (default None = sr/2).
        n_iter: Griffin-Lim iterations (default 32).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        The reconstructed audio samples as a float32 array.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mfcc_to_audio"):
        raise _not_supported("libsonare was built without inverse-reconstruction support")
    c_array, n_mfcc, n_frames = _to_c_float_matrix(coefficients, arg_name="coefficients")
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    if lifter == 0.0:
        return _inverse_audio(
            lib,
            "sonare_mfcc_to_audio",
            "sonare_mfcc_to_audio_ex",
            c_array,
            (
                _to_c_int(n_mfcc, "n_mfcc"),
                _to_c_int(n_frames, "n_frames"),
                _to_c_int(n_mels, "n_mels"),
                _to_c_int(sr, "sr"),
                _to_c_int(n_fft, "n_fft"),
                _to_c_int(hop_length, "hop_length"),
                _to_c_float(fmin, "fmin"),
                _to_c_float(_resolve_zero_default(fmax, "fmax"), "fmax"),
            ),
            n_iter,
            htk,
        )
    if not hasattr(lib, "sonare_mfcc_to_audio_ex2"):
        raise _not_supported("this libsonare build does not support inverse MFCC liftering")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mfcc_to_audio_ex2(
        c_array,
        _to_c_int(n_mfcc, "n_mfcc"),
        _to_c_int(n_frames, "n_frames"),
        _to_c_int(n_mels, "n_mels"),
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(_resolve_zero_default(fmax, "fmax"), "fmax"),
        ctypes.c_int(1 if htk else 0),
        _to_c_float(lifter, "lifter"),
        _to_c_int(n_iter, "n_iter"),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        lib.sonare_free_floats(out)


@_entry("vqt")
@_guard_buffer("samples")
def vqt(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    gamma: float | None = None,
) -> CqtResult:
    """Compute the Variable-Q Transform magnitude (``gamma`` controls Q).

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        fmin: Lowest center frequency in Hz (default C1).
        n_bins: Total number of frequency bins (default 84).
        bins_per_octave: Bins per octave (default 12).
        gamma: Bandwidth offset. ``None`` (the default) selects the automatic
            ERB-derived value; zero is equivalent to CQT. A negative or
            non-finite value is refused.

    Returns:
        A :class:`CqtResult` with the magnitude of shape ``(n_bins, n_frames)`` and
        the bin frequencies.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_vqt"):
        raise _not_supported("libsonare was built without VQT support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = lib.sonare_vqt(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_int(n_bins, "n_bins"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        _to_c_float(_resolve_optional_non_negative(gamma, "gamma"), "gamma"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _cqt_result_from_c(out)
    finally:
        lib.sonare_free_cqt_result(ctypes.byref(out))


@_entry("vqt_to_audio")
def vqt_to_audio(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    hop_length: int = 512,
    fmin_hz: float = 32.70319566257483,
    bins_per_octave: int = 12,
    gamma: float | None = None,
    n_iter: int = 32,
) -> np.ndarray:
    """Reconstruct mono audio from a VQT magnitude via Griffin-Lim.

    ``magnitude`` is a 2-D array of shape ``(n_bins, n_frames)``. Returns a float32 array.
    """
    lib = _get_lib()
    c_array, n_bins, n_frames = _to_c_float_matrix(magnitude, arg_name="magnitude")
    length = n_bins * n_frames
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_vqt_to_audio_checked(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(n_bins, "n_bins"),
        _to_c_int(n_frames, "n_frames"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin_hz, "fmin_hz"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        _to_c_float(_resolve_optional_non_negative(gamma, "gamma"), "gamma"),
        _to_c_int(n_iter, "n_iter"),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        if out:
            lib.sonare_free_floats(out)
