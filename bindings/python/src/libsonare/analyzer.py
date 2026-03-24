"""High-level analysis functions wrapping the libsonare C quick API."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SONARE_OK,
    SonareAnalysisResult,
    SonareChromaResult,
    SonareHpssResult,
    SonareKey,
    SonareMelResult,
    SonareMfccResult,
    SonarePitchResult,
    SonareStftResult,
    load_library,
)
from .types import (
    AnalysisResult,
    ChromaResult,
    HpssResult,
    Key,
    MelSpectrogramResult,
    MfccResult,
    Mode,
    PitchClass,
    PitchResult,
    StftResult,
    TimeSignature,
)

_lib: ctypes.CDLL | None = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure."""
    if rc != SONARE_OK:
        lib = _get_lib()
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


def _to_c_float_array(
    samples: Sequence[float] | list[float],
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array."""
    length = len(samples)
    c_array = (ctypes.c_float * length)(*samples)
    return c_array, length


def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected BPM value.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.byref(out_bpm))
    _check(rc)
    return float(out_bpm.value)


def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> Key:
    """Detect the musical key of audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected Key with root, mode, and confidence.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_key = SonareKey()
    rc = lib.sonare_detect_key(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.byref(out_key))
    _check(rc)
    return Key(
        root=PitchClass(out_key.root),
        mode=Mode(out_key.mode),
        confidence=float(out_key.confidence),
    )


def detect_beats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect beat positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of beat times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_beats(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    count = out_count.value
    result = [float(out_times[i]) for i in range(count)]
    if out_times and count > 0:
        lib.sonare_free_floats(out_times)
    return result


def detect_onsets(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect onset positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of onset times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_onsets(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    count = out_count.value
    result = [float(out_times[i]) for i in range(count)]
    if out_times and count > 0:
        lib.sonare_free_floats(out_times)
    return result


def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        AnalysisResult with BPM, key, time signature, and beat times.

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAnalysisResult()
    rc = lib.sonare_analyze(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                            ctypes.byref(out))
    _check(rc)

    beat_times = [float(out.beat_times[i]) for i in range(out.beat_count)]

    result = AnalysisResult(
        bpm=float(out.bpm),
        bpm_confidence=float(out.bpm_confidence),
        key=Key(
            root=PitchClass(out.key.root),
            mode=Mode(out.key.mode),
            confidence=float(out.key.confidence),
        ),
        time_signature=TimeSignature(
            numerator=int(out.time_signature.numerator),
            denominator=int(out.time_signature.denominator),
            confidence=float(out.time_signature.confidence),
        ),
        beat_times=beat_times,
    )

    lib.sonare_free_result(ctypes.byref(out))
    return result


def version() -> str:
    """Return the libsonare version string."""
    lib = _get_lib()
    v = lib.sonare_version()
    return v.decode("utf-8") if v else ""


# ============================================================================
# Effects
# ============================================================================


def hpss(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
) -> HpssResult:
    """Perform harmonic-percussive source separation.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Harmonic median filter kernel size.
        kernel_percussive: Percussive median filter kernel size.

    Returns:
        HpssResult with harmonic and percussive components.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                         ctypes.c_int(kernel_harmonic), ctypes.c_int(kernel_percussive),
                         ctypes.byref(out))
    _check(rc)
    n = out.length
    result = HpssResult(
        harmonic=[float(out.harmonic[i]) for i in range(n)],
        percussive=[float(out.percussive[i]) for i in range(n)],
        length=int(n),
        sample_rate=int(out.sample_rate),
    )
    lib.sonare_free_hpss_result(ctypes.byref(out))
    return result


def harmonic(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Extract the harmonic component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of harmonic component samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_harmonic(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                             ctypes.byref(out), ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def percussive(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Extract the percussive component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of percussive component samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_percussive(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.byref(out), ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def time_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    rate: float = 1.0,
) -> list[float]:
    """Time-stretch audio without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        rate: Stretch factor (>1 speeds up, <1 slows down).

    Returns:
        List of time-stretched samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_time_stretch(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                                 ctypes.c_float(rate), ctypes.byref(out),
                                 ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def pitch_shift(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    semitones: float = 0.0,
) -> list[float]:
    """Shift the pitch of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        semitones: Number of semitones to shift (positive = up, negative = down).

    Returns:
        List of pitch-shifted samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_pitch_shift(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                                ctypes.c_float(semitones), ctypes.byref(out),
                                ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = -3.0,
) -> list[float]:
    """Normalize audio to a target dB level.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Target peak level in dB (default -3.0).

    Returns:
        List of normalized samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_normalize(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                              ctypes.c_float(target_db), ctypes.byref(out),
                              ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def trim(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold_db: float = -60.0,
) -> list[float]:
    """Trim silence from the beginning and end of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Silence threshold in dB (default -60.0).

    Returns:
        List of trimmed samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_trim(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                         ctypes.c_float(threshold_db), ctypes.byref(out),
                         ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


# ============================================================================
# Features - Spectrogram
# ============================================================================


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
    rc = lib.sonare_stft(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                         ctypes.c_int(n_fft), ctypes.c_int(hop_length), ctypes.byref(out))
    _check(rc)
    total = out.n_bins * out.n_frames
    result = StftResult(
        n_bins=out.n_bins,
        n_frames=out.n_frames,
        n_fft=out.n_fft,
        hop_length=out.hop_length,
        sample_rate=out.sample_rate,
        magnitude=[float(out.magnitude[i]) for i in range(total)],
        power=[float(out.power[i]) for i in range(total)],
    )
    lib.sonare_free_stft_result(ctypes.byref(out))
    return result


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
    rc = lib.sonare_stft_db(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                            ctypes.c_int(n_fft), ctypes.c_int(hop_length),
                            ctypes.byref(out_n_bins), ctypes.byref(out_n_frames),
                            ctypes.byref(out_db))
    _check(rc)
    total = out_n_bins.value * out_n_frames.value
    result = [float(out_db[i]) for i in range(total)]
    if out_db and total > 0:
        lib.sonare_free_floats(out_db)
    return (out_n_bins.value, out_n_frames.value, result)


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
    rc = lib.sonare_mel_spectrogram(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                                    ctypes.c_int(n_fft), ctypes.c_int(hop_length),
                                    ctypes.c_int(n_mels), ctypes.byref(out))
    _check(rc)
    total = out.n_mels * out.n_frames
    result = MelSpectrogramResult(
        n_mels=out.n_mels,
        n_frames=out.n_frames,
        sample_rate=out.sample_rate,
        hop_length=out.hop_length,
        power=[float(out.power[i]) for i in range(total)],
        db=[float(out.db[i]) for i in range(total)],
    )
    lib.sonare_free_mel_result(ctypes.byref(out))
    return result


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
    rc = lib.sonare_mfcc(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                         ctypes.c_int(n_fft), ctypes.c_int(hop_length),
                         ctypes.c_int(n_mels), ctypes.c_int(n_mfcc), ctypes.byref(out))
    _check(rc)
    total = out.n_mfcc * out.n_frames
    result = MfccResult(
        n_mfcc=out.n_mfcc,
        n_frames=out.n_frames,
        coefficients=[float(out.coefficients[i]) for i in range(total)],
    )
    lib.sonare_free_mfcc_result(ctypes.byref(out))
    return result


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
    rc = lib.sonare_chroma(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                           ctypes.c_int(n_fft), ctypes.c_int(hop_length), ctypes.byref(out))
    _check(rc)
    total = out.n_chroma * out.n_frames
    result = ChromaResult(
        n_chroma=out.n_chroma,
        n_frames=out.n_frames,
        sample_rate=out.sample_rate,
        hop_length=out.hop_length,
        features=[float(out.features[i]) for i in range(total)],
        mean_energy=[float(out.mean_energy[i]) for i in range(out.n_chroma)],
    )
    lib.sonare_free_chroma_result(ctypes.byref(out))
    return result


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
    rc = lib.sonare_spectral_centroid(c_array, ctypes.c_size_t(length),
                                      ctypes.c_int(sample_rate), ctypes.c_int(n_fft),
                                      ctypes.c_int(hop_length), ctypes.byref(out),
                                      ctypes.byref(out_count))
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
    rc = lib.sonare_spectral_bandwidth(c_array, ctypes.c_size_t(length),
                                        ctypes.c_int(sample_rate), ctypes.c_int(n_fft),
                                        ctypes.c_int(hop_length), ctypes.byref(out),
                                        ctypes.byref(out_count))
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
    rc = lib.sonare_spectral_rolloff(c_array, ctypes.c_size_t(length),
                                      ctypes.c_int(sample_rate), ctypes.c_int(n_fft),
                                      ctypes.c_int(hop_length), ctypes.c_float(roll_percent),
                                      ctypes.byref(out), ctypes.byref(out_count))
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
    rc = lib.sonare_spectral_flatness(c_array, ctypes.c_size_t(length),
                                       ctypes.c_int(sample_rate), ctypes.c_int(n_fft),
                                       ctypes.c_int(hop_length), ctypes.byref(out),
                                       ctypes.byref(out_count))
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
    rc = lib.sonare_zero_crossing_rate(c_array, ctypes.c_size_t(length),
                                        ctypes.c_int(sample_rate), ctypes.c_int(frame_length),
                                        ctypes.c_int(hop_length), ctypes.byref(out),
                                        ctypes.byref(out_count))
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
    rc = lib.sonare_rms_energy(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.c_int(frame_length), ctypes.c_int(hop_length),
                               ctypes.byref(out), ctypes.byref(out_count))
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
    rc = lib.sonare_pitch_yin(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                              ctypes.c_int(frame_length), ctypes.c_int(hop_length),
                              ctypes.c_float(fmin), ctypes.c_float(fmax),
                              ctypes.c_float(threshold), ctypes.byref(out))
    _check(rc)
    n = out.n_frames
    result = PitchResult(
        n_frames=n,
        f0=[float(out.f0[i]) for i in range(n)],
        voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
        voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
        median_f0=float(out.median_f0),
        mean_f0=float(out.mean_f0),
    )
    lib.sonare_free_pitch_result(ctypes.byref(out))
    return result


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
    rc = lib.sonare_pitch_pyin(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.c_int(frame_length), ctypes.c_int(hop_length),
                               ctypes.c_float(fmin), ctypes.c_float(fmax),
                               ctypes.c_float(threshold), ctypes.byref(out))
    _check(rc)
    n = out.n_frames
    result = PitchResult(
        n_frames=n,
        f0=[float(out.f0[i]) for i in range(n)],
        voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
        voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
        median_f0=float(out.median_f0),
        mean_f0=float(out.mean_f0),
    )
    lib.sonare_free_pitch_result(ctypes.byref(out))
    return result


# ============================================================================
# Core - Conversion
# ============================================================================


def hz_to_mel(hz: float) -> float:
    """Convert frequency in Hz to Mel scale."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_mel(ctypes.c_float(hz)))


def mel_to_hz(mel: float) -> float:
    """Convert Mel scale value to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_mel_to_hz(ctypes.c_float(mel)))


def hz_to_midi(hz: float) -> float:
    """Convert frequency in Hz to MIDI note number."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_midi(ctypes.c_float(hz)))


def midi_to_hz(midi: float) -> float:
    """Convert MIDI note number to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_midi_to_hz(ctypes.c_float(midi)))


def hz_to_note(hz: float) -> str:
    """Convert frequency in Hz to note name (e.g. 'A4')."""
    lib = _get_lib()
    result = lib.sonare_hz_to_note(ctypes.c_float(hz))
    return result.decode("utf-8") if result else ""


def note_to_hz(note: str) -> float:
    """Convert note name (e.g. 'A4') to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_note_to_hz(note.encode("utf-8")))


def frames_to_time(frames: int, sr: int = 22050, hop_length: int = 512) -> float:
    """Convert frame count to time in seconds."""
    lib = _get_lib()
    return float(lib.sonare_frames_to_time(ctypes.c_int(frames), ctypes.c_int(sr),
                                            ctypes.c_int(hop_length)))


def time_to_frames(time: float, sr: int = 22050, hop_length: int = 512) -> int:
    """Convert time in seconds to frame count."""
    lib = _get_lib()
    return int(lib.sonare_time_to_frames(ctypes.c_float(time), ctypes.c_int(sr),
                                          ctypes.c_int(hop_length)))


# ============================================================================
# Core - Resample
# ============================================================================


def resample(
    samples: Sequence[float] | list[float],
    src_sr: int,
    target_sr: int,
) -> list[float]:
    """Resample audio to a different sample rate.

    Args:
        samples: Audio samples.
        src_sr: Source sample rate in Hz.
        target_sr: Target sample rate in Hz.

    Returns:
        List of resampled audio samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_resample(c_array, ctypes.c_size_t(length), ctypes.c_int(src_sr),
                             ctypes.c_int(target_sr), ctypes.byref(out),
                             ctypes.byref(out_length))
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result
