"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._ffi import (
    SonareChromaResult,
    SonareMelResult,
    SonareMfccResult,
    SonareNoteSegmenterConfig,
    SonareNoteSegmentsResult,
    SonarePitchResult,
    SonareStftResult,
)
from ._runtime import (
    _call_float_transform,
    _check,
    _float_array_result,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _out_float_array,
    _out_int_array,
    _to_c_float_array,
    _validate_samples,
)
from .types import (
    ChromaResult,
    MelSpectrogramResult,
    MfccResult,
    NoteSegment,
    PitchResult,
    StftResult,
)


def stft(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> StftResult:
    """Compute the short-time Fourier transform.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        StftResult with magnitude and power spectrograms.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareStftResult()
    rc = lib.sonare_stft(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        total = out.n_bins * out.n_frames
        return StftResult(
            n_bins=out.n_bins,
            n_frames=out.n_frames,
            n_fft=out.n_fft,
            hop_length=out.hop_length,
            sample_rate=out.sample_rate,
            magnitude=[float(out.magnitude[i]) for i in range(total)],
            power=[float(out.power[i]) for i in range(total)],
        )
    finally:
        lib.sonare_free_stft_result(ctypes.byref(out))


def stft_db(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> tuple[int, int, list[float]]:
    """Compute the STFT in decibels.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        Tuple of (n_bins, n_frames, db_values).
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_n_bins = ctypes.c_int()
    out_n_frames = ctypes.c_int()
    out_db = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_stft_db(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out_n_bins),
        ctypes.byref(out_n_frames),
        ctypes.byref(out_db),
    )
    _check(rc)
    try:
        total = out_n_bins.value * out_n_frames.value
        result = _float_array_result(out_db, total)
        return (out_n_bins.value, out_n_frames.value, result)
    finally:
        total = out_n_bins.value * out_n_frames.value
        if out_db and total > 0:
            lib.sonare_free_floats(out_db)


# ============================================================================
# Features - Mel
# ============================================================================


def mel_spectrogram(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    fmin: float = 0.0,
    fmax: float = 0.0,
    htk: bool = False,
) -> MelSpectrogramResult:
    """Compute a Mel spectrogram.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).
        fmin: Minimum Mel frequency in Hz (default 0.0 = librosa default).
            Set together with ``fmax`` to round-trip with ``mel_to_stft`` /
            ``mel_to_audio``.
        fmax: Maximum Mel frequency in Hz (default 0.0 = sample_rate / 2).
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        MelSpectrogramResult with power and dB spectrograms.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareMelResult()
    rc = lib.sonare_mel_spectrogram_ex(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(1 if htk else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        total = out.n_mels * out.n_frames
        return MelSpectrogramResult(
            n_mels=out.n_mels,
            n_frames=out.n_frames,
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            power=[float(out.power[i]) for i in range(total)],
            db=[float(out.db[i]) for i in range(total)],
        )
    finally:
        lib.sonare_free_mel_result(ctypes.byref(out))


def mfcc(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 20,
    fmin: float = 0.0,
    fmax: float = 0.0,
    htk: bool = False,
    lifter: float = 0.0,
) -> MfccResult:
    """Compute Mel-frequency cepstral coefficients.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).
        n_mfcc: Number of MFCC coefficients (default 20).
        fmin: Minimum Mel frequency in Hz (default 0.0 = librosa default).
        fmax: Maximum Mel frequency in Hz (default 0.0 = sample_rate / 2).
        htk: Use the HTK Mel formula instead of Slaney (default False).
        lifter: Cepstral liftering coefficient (default 0.0 = no liftering).

    Returns:
        MfccResult with coefficient matrix.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareMfccResult()
    rc = lib.sonare_mfcc_ex(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
        ctypes.c_int(n_mfcc),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(1 if htk else 0),
        ctypes.c_float(lifter),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        total = out.n_mfcc * out.n_frames
        return MfccResult(
            n_mfcc=out.n_mfcc,
            n_frames=out.n_frames,
            coefficients=[float(out.coefficients[i]) for i in range(total)],
        )
    finally:
        lib.sonare_free_mfcc_result(ctypes.byref(out))


# ============================================================================
# Features - Chroma
# ============================================================================


def chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> ChromaResult:
    """Compute STFT chroma features (librosa.feature.chroma_stft).

    The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
    librosa.feature.chroma_stft -- which estimates tuning from the signal when
    none is given -- this does NOT auto-estimate and exposes no tuning argument,
    so sharp/flat (non-A440) recordings smear across pitch classes. Estimate
    tuning separately via :func:`estimate_tuning` if a non-A440 reference matters.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        ChromaResult with chroma features and mean energy.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareChromaResult()
    rc = lib.sonare_chroma(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        total = out.n_chroma * out.n_frames
        return ChromaResult(
            n_chroma=out.n_chroma,
            n_frames=out.n_frames,
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            features=[float(out.features[i]) for i in range(total)],
            mean_energy=[float(out.mean_energy[i]) for i in range(out.n_chroma)],
        )
    finally:
        lib.sonare_free_chroma_result(ctypes.byref(out))


def _chroma_variant(
    fn_name: str,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    hop_length: int,
    n_chroma: int,
    bins_per_octave: int | None = None,
) -> ChromaResult:
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareChromaResult()
    args = [
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_chroma),
    ]
    if bins_per_octave is not None:
        args.append(ctypes.c_int(bins_per_octave))
    args.append(ctypes.byref(out))
    rc = getattr(lib, fn_name)(*args)
    _check(rc)
    try:
        total = out.n_chroma * out.n_frames
        return ChromaResult(
            n_chroma=out.n_chroma,
            n_frames=out.n_frames,
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            features=[float(out.features[i]) for i in range(total)],
            mean_energy=[float(out.mean_energy[i]) for i in range(out.n_chroma)],
        )
    finally:
        lib.sonare_free_chroma_result(ctypes.byref(out))


def chroma_cens(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult:
    """Compute CENS chroma features."""
    return _chroma_variant(
        "sonare_chroma_cens_ex",
        samples,
        sample_rate,
        hop_length,
        n_chroma,
        bins_per_octave,
    )


def chroma_cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult:
    """Compute a constant-Q chromagram (librosa.feature.chroma_cqt)."""
    return _chroma_variant(
        "sonare_chroma_cqt_ex",
        samples,
        sample_rate,
        hop_length,
        n_chroma,
        bins_per_octave,
    )


def bass_chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
) -> ChromaResult:
    """Compute bass-focused chroma features."""
    return _chroma_variant("sonare_bass_chroma", samples, sample_rate, hop_length, n_chroma)


# ============================================================================
# Features - Spectral
# ============================================================================


def spectral_centroid(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> list[float]:
    """Compute the spectral centroid per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        List of spectral centroid values per frame.
    """
    return _call_float_transform(
        "sonare_spectral_centroid",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
    )


def spectral_bandwidth(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> list[float]:
    """Compute the spectral bandwidth per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        List of spectral bandwidth values per frame.
    """
    return _call_float_transform(
        "sonare_spectral_bandwidth",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
    )


def spectral_rolloff(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    roll_percent: float = 0.85,
) -> list[float]:
    """Compute the spectral rolloff per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        roll_percent: Rolloff percentage (default 0.85).

    Returns:
        List of spectral rolloff values per frame.
    """
    return _call_float_transform(
        "sonare_spectral_rolloff",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(roll_percent),
    )


def spectral_flatness(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> list[float]:
    """Compute the spectral flatness per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        List of spectral flatness values per frame.
    """
    return _call_float_transform(
        "sonare_spectral_flatness",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
    )


def zero_crossing_rate(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> list[float]:
    """Compute the zero-crossing rate per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        List of zero-crossing rate values per frame.
    """
    return _call_float_transform(
        "sonare_zero_crossing_rate",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
    )


def rms_energy(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> list[float]:
    """Compute the RMS energy per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        List of RMS energy values per frame.
    """
    return _call_float_transform(
        "sonare_rms_energy",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
    )


# ============================================================================
# Features - Spectral contrast / poly / zero-crossings / tuning
# ============================================================================


def spectral_contrast(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_bands: int = 6,
    fmin: float = 200.0,
    quantile: float = 0.02,
) -> np.ndarray:
    """Compute spectral contrast (librosa.feature.spectral_contrast).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_bands: Number of frequency bands (default 6).
        fmin: Lowest band edge in Hz (default 200.0).
        quantile: Peak/valley quantile (default 0.02).

    Returns:
        A float32 array of shape ``(n_bands + 1, n_frames)`` (matches the
        bare-ndarray convention of ``stft`` / ``mel_spectrogram`` / ``nn_filter``).
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_rows = ctypes.c_int()
    out_cols = ctypes.c_int()
    with _out_float_array(lib) as (out, _out_length):
        rc = lib.sonare_spectral_contrast(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_fft),
            ctypes.c_int(hop_length),
            ctypes.c_int(n_bands),
            ctypes.c_float(fmin),
            ctypes.c_float(quantile),
            ctypes.byref(out),
            ctypes.byref(out_rows),
            ctypes.byref(out_cols),
        )
        _check(rc)
        rows = int(out_rows.value)
        cols = int(out_cols.value)
        return _from_c_float_array(out, rows * cols).reshape(rows, cols)


def poly_features(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    order: int = 1,
) -> np.ndarray:
    """Fit polynomial coefficients per frame (librosa.feature.poly_features).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        order: Polynomial order (default 1).

    Returns:
        A float32 array of shape ``(order + 1, n_frames)`` (matches the
        bare-ndarray convention of ``stft`` / ``mel_spectrogram`` / ``nn_filter``).
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_rows = ctypes.c_int()
    out_cols = ctypes.c_int()
    with _out_float_array(lib) as (out, _out_length):
        rc = lib.sonare_poly_features(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_fft),
            ctypes.c_int(hop_length),
            ctypes.c_int(order),
            ctypes.byref(out),
            ctypes.byref(out_rows),
            ctypes.byref(out_cols),
        )
        _check(rc)
        rows = int(out_rows.value)
        cols = int(out_cols.value)
        return _from_c_float_array(out, rows * cols).reshape(rows, cols)


def zero_crossings(
    samples: Sequence[float] | list[float],
    threshold: float = 1e-10,
    ref_magnitude: bool = False,
    pad: bool = True,
    zero_pos: bool = True,
) -> np.ndarray:
    """Return zero-crossing sample indices (librosa.zero_crossings).

    Args:
        samples: Input signal.
        threshold: Magnitudes <= threshold are treated as zero (default 1e-10).
        ref_magnitude: Scale ``threshold`` by ``max(|y|)`` (default False).
        pad: Always report index 0 as a zero-crossing (default True).
        zero_pos: Treat the sign of zero as positive (default True).

    Returns:
        A 1-D ``int32`` array of zero-crossing sample indices.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_zero_crossings(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_float(threshold),
            ctypes.c_int(1 if ref_magnitude else 0),
            ctypes.c_int(1 if pad else 0),
            ctypes.c_int(1 if zero_pos else 0),
            ctypes.byref(out),
            ctypes.byref(out_count),
        )
        _check(rc)
        return _from_c_int_array(out, out_count.value)


def pitch_tuning(
    frequencies: Sequence[float] | list[float],
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float:
    """Per-octave tuning offset from detected pitches (librosa.pitch_tuning).

    Args:
        frequencies: Detected pitch frequencies in Hz (non-positive ignored).
        resolution: Tuning resolution in fractions of a bin (default 0.01).
        bins_per_octave: Pitch bins per octave (default 12).

    Returns:
        Tuning offset in fractions of a bin, in ``(-0.5, 0.5]``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(frequencies)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_pitch_tuning(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_float(resolution),
        ctypes.c_int(bins_per_octave),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def estimate_tuning(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float:
    """Global tuning offset of an audio signal (librosa.estimate_tuning).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        resolution: Tuning resolution in fractions of a bin (default 0.01).
        bins_per_octave: Pitch bins per octave (default 12).

    Returns:
        Tuning offset in fractions of a bin, in ``(-0.5, 0.5]``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_estimate_tuning(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(resolution),
        ctypes.c_int(bins_per_octave),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


# ============================================================================
# Features - Pitch
# ============================================================================


def pitch_yin(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult:
    """Estimate fundamental frequency using the YIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        fmin: Minimum frequency in Hz (default 65.0).
        fmax: Maximum frequency in Hz (default 2093.0).
        threshold: YIN threshold (default 0.1).
        fill_na: Retained for API compatibility. YIN always returns a finite
            estimate for each complete frame, as librosa.yin does; use
            ``voiced_flag`` to distinguish threshold crossings.

    Returns:
        PitchResult with f0, voiced probabilities, and statistics.
    """
    lib = _get_lib()
    sample_buf = _validate_samples("pitch_yin", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonarePitchResult()
    rc = lib.sonare_pitch_yin(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_float(threshold),
        ctypes.c_int(1 if fill_na else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.n_frames
        return PitchResult(
            n_frames=n,
            f0=[float(out.f0[i]) for i in range(n)],
            voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
            voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
            median_f0=float(out.median_f0),
            mean_f0=float(out.mean_f0),
        )
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


def pitch_pyin(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult:
    """Estimate fundamental frequency using the pYIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        fmin: Minimum frequency in Hz (default 65.0).
        fmax: Maximum frequency in Hz (default 2093.0).
        threshold: YIN threshold (default 0.1).
        fill_na: If True, return 0 for unvoiced f0 frames. If False,
            keep those frames as NaN to match librosa-style pitch tracks.

    Returns:
        PitchResult with f0, voiced probabilities, and statistics.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonarePitchResult()
    rc = lib.sonare_pitch_pyin(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_float(threshold),
        ctypes.c_int(1 if fill_na else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.n_frames
        return PitchResult(
            n_frames=n,
            f0=[float(out.f0[i]) for i in range(n)],
            voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
            voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
            median_f0=float(out.median_f0),
            mean_f0=float(out.mean_f0),
        )
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


def note_segments(
    f0_hz: Sequence[float] | list[float],
    voiced_prob: Sequence[float] | list[float],
    frame_rate: float,
    *,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
) -> list[NoteSegment]:
    """Segment a monophonic F0 track into stable note regions.

    Zero-Hz F0 frames and voiced probabilities below 0.5 delimit note regions.
    The F0 and probability arrays must have equal non-zero length.
    """
    lib = _get_lib()
    f0_array, f0_count = _to_c_float_array(f0_hz)
    probability_array, probability_count = _to_c_float_array(voiced_prob)
    config = SonareNoteSegmenterConfig(
        1,
        0.0 if segmentation_threshold_cents is None else segmentation_threshold_cents,
        0.0 if min_note_ms is None else min_note_ms,
        0.0 if reference_hz is None else reference_hz,
    )
    out = SonareNoteSegmentsResult()
    rc = lib.sonare_note_segments(
        f0_array,
        ctypes.c_size_t(f0_count),
        probability_array,
        ctypes.c_size_t(probability_count),
        ctypes.c_float(frame_rate),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return [
            NoteSegment(
                frame_start=int(out.segments[i].frame_start),
                frame_end=int(out.segments[i].frame_end),
                start_seconds=float(out.segments[i].start_seconds),
                end_seconds=float(out.segments[i].end_seconds),
                median_cents=float(out.segments[i].median_cents),
            )
            for i in range(out.count)
        ]
    finally:
        lib.sonare_free_note_segments(ctypes.byref(out))
