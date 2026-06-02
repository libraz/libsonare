"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._ffi import (
    SonareChromaResult,
    SonareClippingResult,
    SonareCqtResult,
    SonareDynamicRangeResult,
    SonareInverseResult,
    SonareLufsResult,
    SonareMelResult,
    SonareMfccResult,
    SonarePhaseScopePoint,
    SonarePhaseScopeResult,
    SonarePitchResult,
    SonareSpectrumResult,
    SonareStftResult,
    SonareVectorscopePoint,
    SonareVectorscopeResult,
)
from ._runtime import (
    _call_float_transform,
    _check,
    _float_array_result,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _to_c_float_array,
    _validate_samples,
    _validate_scalar,
)
from .types import (
    ChromaResult,
    ClippingRegion,
    ClippingReport,
    CqtResult,
    DynamicRangeReport,
    InverseResult,
    LufsResult,
    MelSpectrogramResult,
    MfccResult,
    PhaseScopeReport,
    PitchResult,
    SpectrumReport,
    StftResult,
    VectorscopeReport,
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
) -> MelSpectrogramResult:
    """Compute a Mel spectrogram.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).

    Returns:
        MelSpectrogramResult with power and dB spectrograms.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareMelResult()
    rc = lib.sonare_mel_spectrogram(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
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
) -> MfccResult:
    """Compute Mel-frequency cepstral coefficients.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).
        n_mfcc: Number of MFCC coefficients (default 20).

    Returns:
        MfccResult with coefficient matrix.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareMfccResult()
    rc = lib.sonare_mfcc(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
        ctypes.c_int(n_mfcc),
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
    """Compute chroma features.

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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_spectral_centroid(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_spectral_bandwidth(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_spectral_rolloff(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(roll_percent),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_spectral_flatness(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_zero_crossing_rate(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_rms_energy(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_floats(out)


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
    out = ctypes.POINTER(ctypes.c_float)()
    out_rows = ctypes.c_int()
    out_cols = ctypes.c_int()
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
    try:
        rows = int(out_rows.value)
        cols = int(out_cols.value)
        return _from_c_float_array(out, rows * cols).reshape(rows, cols)
    finally:
        if out and out_rows.value * out_cols.value > 0:
            lib.sonare_free_floats(out)


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
    out = ctypes.POINTER(ctypes.c_float)()
    out_rows = ctypes.c_int()
    out_cols = ctypes.c_int()
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
    try:
        rows = int(out_rows.value)
        cols = int(out_cols.value)
        return _from_c_float_array(out, rows * cols).reshape(rows, cols)
    finally:
        if out and out_rows.value * out_cols.value > 0:
            lib.sonare_free_floats(out)


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
    out = ctypes.POINTER(ctypes.c_int)()
    out_count = ctypes.c_size_t()
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
    try:
        return _from_c_int_array(out, out_count.value)
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_ints(out)


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
    threshold: float = 0.3,
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
        threshold: YIN threshold (default 0.3).
        fill_na: If True, return 0 for unvoiced f0 frames. If False,
            keep those frames as NaN to match librosa-style pitch tracks.

    Returns:
        PitchResult with f0, voiced probabilities, and statistics.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
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
    threshold: float = 0.3,
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
        threshold: YIN threshold (default 0.3).
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


def lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> LufsResult:
    """Compute integrated/momentary/short-term LUFS and loudness range."""
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
        loudness_range=float(out.loudness_range),
    )


def momentary_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Compute the per-block momentary LUFS time series."""
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
    """Compute the per-block short-term LUFS time series."""
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
        sample_rate: Sample rate in Hz (default 22050).

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
        loudness_range=float(out.loudness_range),
    )


def ebur128_loudness_range(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float:
    """EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer."""
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
    """Pearson correlation in [-1, 1] between two equal-length channels."""
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
    *,
    validate: bool = True,
) -> VectorscopeReport:
    """Per-sample mid/side point series for a (left, right) stereo pair."""
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


def metering_phase_scope(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> PhaseScopeReport:
    """Phase-scope point series plus summary stats for a stereo pair."""
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
    """Single-frame magnitude / power / dB spectrum view.

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


def metering_dynamic_range(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    window_sec: float = 0.0,
    hop_sec: float = 0.0,
    low_percentile: float = 0.0,
    high_percentile: float = 0.0,
    *,
    validate: bool = True,
) -> DynamicRangeReport:
    """Sliding-window dynamic range (high_percentile - low_percentile, in dB).

    Pass 0.0 for any parameter to use the library default
    (window=3 s, hop=1 s, low=0.10, high=0.95).
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


# ============================================================================
# Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
# ============================================================================


def mel_to_stft(
    mel: Sequence[float] | list[float],
    n_mels: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    fmin: float = 0.0,
    fmax: float = 0.0,
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

    Returns:
        An :class:`InverseResult` with the reconstructed STFT power matrix.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_stft"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mel)
    if length != n_mels * n_frames:
        raise ValueError("mel length must equal n_mels * n_frames")
    out = SonareInverseResult()
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

    Returns:
        The reconstructed audio samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mel_to_audio"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mel)
    if length != n_mels * n_frames:
        raise ValueError("mel length must equal n_mels * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mel_to_audio(
        c_array,
        ctypes.c_int(n_mels),
        ctypes.c_int(n_frames),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(n_iter),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def mfcc_to_mel(
    mfcc_coeffs: Sequence[float] | list[float],
    n_mfcc: int,
    n_frames: int,
    n_mels: int = 128,
) -> InverseResult:
    """Invert MFCC coefficients back to a Mel spectrogram (dB scale).

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
        raise ValueError("mfcc_coeffs length must equal n_mfcc * n_frames")
    out = SonareInverseResult()
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

    Returns:
        The reconstructed audio samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mfcc_to_audio"):
        raise RuntimeError("libsonare was built without inverse-reconstruction support")
    c_array, length = _to_c_float_array(mfcc_coeffs)
    if length != n_mfcc * n_frames:
        raise ValueError("mfcc_coeffs length must equal n_mfcc * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mfcc_to_audio(
        c_array,
        ctypes.c_int(n_mfcc),
        ctypes.c_int(n_frames),
        ctypes.c_int(n_mels),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(n_iter),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def vqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    gamma: float = 0.0,
) -> CqtResult:
    """Compute the Variable-Q Transform magnitude (``gamma`` controls Q).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        fmin: Lowest center frequency in Hz (default C1).
        n_bins: Total number of frequency bins (default 84).
        bins_per_octave: Bins per octave (default 12).
        gamma: Bandwidth-offset parameter controlling Q (default 0.0).

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
