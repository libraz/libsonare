"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SonareCqtResult,
    SonareInverseResult,
)
from ._runtime import (
    SonareValueError,
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _to_c_float_array,
)
from .types import (
    CqtResult,
    InverseResult,
)

# ============================================================================
# Features - Constant-Q / Variable-Q transforms
# ============================================================================


def _cqt_result_from_c(out: SonareCqtResult) -> CqtResult:
    total = out.n_bins * out.n_frames
    return CqtResult(
        n_bins=int(out.n_bins),
        n_frames=int(out.n_frames),
        hop_length=int(out.hop_length),
        sample_rate=int(out.sample_rate),
        magnitude=[float(out.magnitude[i]) for i in range(total)],
        frequencies=[float(out.frequencies[i]) for i in range(out.n_bins)],
    )


@_guard_buffer("samples")
def cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the Constant-Q Transform magnitude.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        fmin: Lowest center frequency in Hz (default C1).
        n_bins: Total number of frequency bins (default 84).
        bins_per_octave: Bins per octave (default 12).

    Returns:
        A :class:`CqtResult` with the magnitude matrix and bin frequencies.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_cqt"):
        raise RuntimeError("libsonare was built without CQT support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = lib.sonare_cqt(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_int(n_bins),
        ctypes.c_int(bins_per_octave),
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
        raise RuntimeError(f"libsonare was built without {fn_name} support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = getattr(lib, fn_name)(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_int(n_bins),
        ctypes.c_int(bins_per_octave),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _cqt_result_from_c(out)
    finally:
        lib.sonare_free_cqt_result(ctypes.byref(out))


@_guard_buffer("samples")
def pseudo_cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the pseudo-CQT magnitude approximation."""
    return _cqt_variant(
        "sonare_pseudo_cqt", samples, sample_rate, hop_length, fmin, n_bins, bins_per_octave
    )


@_guard_buffer("samples")
def hybrid_cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult:
    """Compute the hybrid CQT magnitude."""
    return _cqt_variant(
        "sonare_hybrid_cqt", samples, sample_rate, hop_length, fmin, n_bins, bins_per_octave
    )


@_guard_buffer("magnitude")
def cqt_to_audio(
    magnitude: Sequence[float] | list[float],
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    bins_per_octave: int = 12,
    n_iter: int = 32,
) -> list[float]:
    """Reconstruct mono audio from row-major CQT magnitude via Griffin-Lim."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(magnitude)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_cqt_to_audio_checked(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(n_bins),
        ctypes.c_int(n_frames),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_int(bins_per_octave),
        ctypes.c_int(n_iter),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out:
            lib.sonare_free_floats(out)


# ============================================================================
# Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
# ============================================================================


@_guard_buffer("mel")
def mel_to_stft(
    mel: Sequence[float] | list[float],
    n_mels: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    fmin: float = 0.0,
    fmax: float = 0.0,
    htk: bool = False,
) -> InverseResult:
    """Approximate inverse of a Mel filterbank (Mel power -> STFT power).

    Args:
        mel: Mel power spectrogram, flattened row-major ``[n_mels x n_frames]``.
        n_mels: Number of Mel bands.
        n_frames: Number of time frames.
        sample_rate: Sample rate that produced ``mel`` in Hz (default 22050).
        n_fft: FFT size of the source STFT; sets output bins to ``n_fft/2 + 1``
            (default 2048).
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (0.0 = sr/2).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        An :class:`InverseResult` with the reconstructed STFT power matrix.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_stft"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mel)
    if length != n_mels * n_frames:
        raise SonareValueError("mel length must equal n_mels * n_frames")
    out = SonareInverseResult()
    if htk:
        if not hasattr(lib, "sonare_mel_to_stft_ex"):
            raise RuntimeError("libsonare was built without HTK inverse-reconstruction support")
        rc = lib.sonare_mel_to_stft_ex(
            c_array,
            ctypes.c_int(n_mels),
            ctypes.c_int(n_frames),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_fft),
            ctypes.c_float(fmin),
            ctypes.c_float(fmax),
            ctypes.c_int(1),
            ctypes.byref(out),
        )
    else:
        rc = lib.sonare_mel_to_stft(
            c_array,
            ctypes.c_int(n_mels),
            ctypes.c_int(n_frames),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_fft),
            ctypes.c_float(fmin),
            ctypes.c_float(fmax),
            ctypes.byref(out),
        )
    _check(rc)
    try:
        total = out.rows * out.n_frames
        return InverseResult(
            rows=int(out.rows),
            n_frames=int(out.n_frames),
            data=[float(out.data[i]) for i in range(total)],
        )
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
) -> list[float]:
    """Call a Griffin-Lim inverse wrapper with shared HTK/output handling."""
    function_name = extended_name if htk else base_name
    if not hasattr(lib, function_name):
        feature = "HTK inverse-reconstruction" if htk else "inverse-reconstruction"
        raise RuntimeError(f"libsonare was built without {feature} support")
    htk_arg = (ctypes.c_int(1),) if htk else ()
    with _out_float_array(lib) as (out, out_length):
        _check(
            getattr(lib, function_name)(
                c_array,
                *args,
                *htk_arg,
                ctypes.c_int(n_iter),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


@_guard_buffer("mel")
def mel_to_audio(
    mel: Sequence[float] | list[float],
    n_mels: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 0.0,
    fmax: float = 0.0,
    n_iter: int = 32,
    htk: bool = False,
) -> list[float]:
    """Reconstruct audio from a Mel spectrogram via Griffin-Lim.

    Args:
        mel: Mel power spectrogram, flattened row-major ``[n_mels x n_frames]``.
        n_mels: Number of Mel bands.
        n_frames: Number of time frames.
        sample_rate: Sample rate of the original audio in Hz (default 22050).
        n_fft: FFT size used for reconstruction (default 2048).
        hop_length: Hop length used for reconstruction (default 512).
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (0.0 = sr/2).
        n_iter: Griffin-Lim iterations (default 32).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        The reconstructed audio samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_audio"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mel)
    if length != n_mels * n_frames:
        raise SonareValueError("mel length must equal n_mels * n_frames")
    return _inverse_audio(
        lib,
        "sonare_mel_to_audio",
        "sonare_mel_to_audio_ex",
        c_array,
        (
            ctypes.c_int(n_mels),
            ctypes.c_int(n_frames),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_fft),
            ctypes.c_int(hop_length),
            ctypes.c_float(fmin),
            ctypes.c_float(fmax),
        ),
        n_iter,
        htk,
    )


@_guard_buffer("magnitude")
def griffin_lim(
    magnitude: Sequence[float] | list[float],
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_iter: int = 32,
    momentum: float = 0.99,
) -> list[float]:
    """Reconstruct audio from a row-major STFT magnitude matrix."""
    lib = _get_lib()
    data, length = _to_c_float_array(magnitude)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_griffin_lim(
                data,
                ctypes.c_size_t(length),
                ctypes.c_int(n_bins),
                ctypes.c_int(n_frames),
                ctypes.c_int(n_fft),
                ctypes.c_int(hop_length),
                ctypes.c_int(sample_rate),
                ctypes.c_int(n_iter),
                ctypes.c_float(momentum),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


@_guard_buffer("mfcc_coeffs")
def mfcc_to_mel(
    mfcc_coeffs: Sequence[float] | list[float],
    n_mfcc: int,
    n_frames: int,
    n_mels: int = 128,
    lifter: float = 0.0,
) -> InverseResult:
    """Invert MFCC coefficients back to a Mel power spectrogram.

    Args:
        mfcc_coeffs: MFCC matrix, flattened row-major ``[n_mfcc x n_frames]``.
        n_mfcc: Number of MFCCs.
        n_frames: Number of time frames.
        n_mels: Number of Mel bins to reconstruct (default 128).

    Returns:
        An :class:`InverseResult` with the reconstructed Mel power matrix.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mfcc_to_mel"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mfcc_coeffs)
    if length != n_mfcc * n_frames:
        raise SonareValueError("mfcc_coeffs length must equal n_mfcc * n_frames")
    out = SonareInverseResult()
    if hasattr(lib, "sonare_mfcc_to_mel_ex"):
        rc = lib.sonare_mfcc_to_mel_ex(
            c_array,
            ctypes.c_int(n_mfcc),
            ctypes.c_int(n_frames),
            ctypes.c_int(n_mels),
            ctypes.c_float(lifter),
            ctypes.byref(out),
        )
    else:
        if lifter != 0.0:
            raise RuntimeError("this libsonare build does not support inverse MFCC liftering")
        rc = lib.sonare_mfcc_to_mel(
            c_array,
            ctypes.c_int(n_mfcc),
            ctypes.c_int(n_frames),
            ctypes.c_int(n_mels),
            ctypes.byref(out),
        )
    _check(rc)
    try:
        total = out.rows * out.n_frames
        return InverseResult(
            rows=int(out.rows),
            n_frames=int(out.n_frames),
            data=[float(out.data[i]) for i in range(total)],
        )
    finally:
        lib.sonare_free_inverse_result(ctypes.byref(out))


@_guard_buffer("mfcc_coeffs")
def mfcc_to_audio(
    mfcc_coeffs: Sequence[float] | list[float],
    n_mfcc: int,
    n_frames: int,
    n_mels: int = 128,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 0.0,
    fmax: float = 0.0,
    n_iter: int = 32,
    htk: bool = False,
    lifter: float = 0.0,
) -> list[float]:
    """Reconstruct audio directly from MFCC via Mel inversion + Griffin-Lim.

    Args:
        mfcc_coeffs: MFCC matrix, flattened row-major ``[n_mfcc x n_frames]``.
        n_mfcc: Number of MFCCs.
        n_frames: Number of time frames.
        n_mels: Number of Mel bins (must match the MFCC source config).
        sample_rate: Sample rate of the original audio in Hz (default 22050).
        n_fft: FFT size used for reconstruction (default 2048).
        hop_length: Hop length used for reconstruction (default 512).
        fmin: Minimum Mel frequency in Hz (0.0 for librosa default).
        fmax: Maximum Mel frequency in Hz (0.0 = sr/2).
        n_iter: Griffin-Lim iterations (default 32).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        The reconstructed audio samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mfcc_to_audio"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mfcc_coeffs)
    if length != n_mfcc * n_frames:
        raise SonareValueError("mfcc_coeffs length must equal n_mfcc * n_frames")
    if lifter == 0.0:
        return _inverse_audio(
            lib,
            "sonare_mfcc_to_audio",
            "sonare_mfcc_to_audio_ex",
            c_array,
            (
                ctypes.c_int(n_mfcc),
                ctypes.c_int(n_frames),
                ctypes.c_int(n_mels),
                ctypes.c_int(sample_rate),
                ctypes.c_int(n_fft),
                ctypes.c_int(hop_length),
                ctypes.c_float(fmin),
                ctypes.c_float(fmax),
            ),
            n_iter,
            htk,
        )
    if not hasattr(lib, "sonare_mfcc_to_audio_ex2"):
        raise RuntimeError("this libsonare build does not support inverse MFCC liftering")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mfcc_to_audio_ex2(
        c_array,
        ctypes.c_int(n_mfcc),
        ctypes.c_int(n_frames),
        ctypes.c_int(n_mels),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(1 if htk else 0),
        ctypes.c_float(lifter),
        ctypes.c_int(n_iter),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return [float(out[i]) for i in range(out_length.value)]
    finally:
        lib.sonare_free_floats(out)


@_guard_buffer("samples")
def vqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    gamma: float = -1.0,
) -> CqtResult:
    """Compute the Variable-Q Transform magnitude (``gamma`` controls Q).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        fmin: Lowest center frequency in Hz (default C1).
        n_bins: Total number of frequency bins (default 84).
        bins_per_octave: Bins per octave (default 12).
        gamma: Bandwidth offset. Negative selects the automatic ERB-derived
            value; zero is equivalent to CQT (default -1.0).

    Returns:
        A :class:`CqtResult` with the magnitude matrix and bin frequencies.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_vqt"):
        raise RuntimeError("libsonare was built without VQT support")
    c_array, length = _to_c_float_array(samples)
    out = SonareCqtResult()
    rc = lib.sonare_vqt(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_int(n_bins),
        ctypes.c_int(bins_per_octave),
        ctypes.c_float(gamma),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _cqt_result_from_c(out)
    finally:
        lib.sonare_free_cqt_result(ctypes.byref(out))


@_guard_buffer("magnitude")
def vqt_to_audio(
    magnitude: Sequence[float] | list[float],
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    bins_per_octave: int = 12,
    gamma: float = -1.0,
    n_iter: int = 32,
) -> list[float]:
    """Reconstruct mono audio from row-major VQT magnitude via Griffin-Lim."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(magnitude)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_vqt_to_audio_checked(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(n_bins),
        ctypes.c_int(n_frames),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_int(bins_per_octave),
        ctypes.c_float(gamma),
        ctypes.c_int(n_iter),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out:
            lib.sonare_free_floats(out)
