"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

from ._runtime import *  # noqa: F403


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
        result = [float(out_db[i]) for i in range(total)]
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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    result = [float(out[i]) for i in range(out_count.value)]
    if out and out_count.value > 0:
        lib.sonare_free_floats(out)
    return result


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
) -> LufsResult:
    """Compute integrated/momentary/short-term LUFS and loudness range."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
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
) -> list[float]:
    """Compute the per-block momentary LUFS time series."""
    return _call_float_transform(
        "sonare_momentary_lufs",
        samples,
        ctypes.c_int(sample_rate),
    )


def short_term_lufs(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Compute the per-block short-term LUFS time series."""
    return _call_float_transform(
        "sonare_short_term_lufs",
        samples,
        ctypes.c_int(sample_rate),
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


def cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566,
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
    c_array, _length = _to_c_float_array(mel)
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
    c_array, _length = _to_c_float_array(mel)
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
        return [float(out[i]) for i in range(out_length.value)]
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
    c_array, _length = _to_c_float_array(mfcc_coeffs)
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
    c_array, _length = _to_c_float_array(mfcc_coeffs)
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
        return [float(out[i]) for i in range(out_length.value)]
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def vqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566,
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
