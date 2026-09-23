"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._ffi import (
    SonareChromaResult,
    SonareMelResult,
    SonareMfccResult,
    SonareReassignedSpectrogramResult,
    SonareStftResult,
)
from ._runtime import (
    SonareValueError,
    _call_float_transform,
    _check,
    _float_array_result,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _out_int_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _validate_samples,
)
from .types import (
    ChromaResult,
    MelSpectrogramResult,
    MfccResult,
    ReassignedSpectrogramResult,
    StftResult,
)


def tone(
    frequency: float = 440.0,
    sample_rate: int = 22050,
    duration: float = 1.0,
    phase: float = 0.0,
    amplitude: float = 1.0,
) -> list[float]:
    """Generate a sine tone."""
    lib = _get_lib()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_tone(
                _to_c_float(frequency, "frequency"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(duration, "duration"),
                _to_c_float(phase, "phase"),
                _to_c_float(amplitude, "amplitude"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


def chirp(
    fmin: float = 440.0,
    fmax: float = 880.0,
    sample_rate: int = 22050,
    duration: float = 1.0,
    linear: bool = True,
) -> list[float]:
    """Generate a linear or exponential chirp."""
    lib = _get_lib()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_chirp(
                _to_c_float(fmin, "fmin"),
                _to_c_float(fmax, "fmax"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(duration, "duration"),
                1 if linear else 0,
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


def clicks(
    times: Sequence[float] | list[float],
    sample_rate: int = 22050,
    length: int = 0,
    frequency: float = 1000.0,
    click_duration: float = 0.1,
) -> list[float]:
    """Generate decaying sine clicks at times in seconds."""
    lib = _get_lib()
    data, count = _to_c_float_array(times, arg_name="times")
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_clicks(
                data,
                _to_c_size_t(count, "times length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(length, "length"),
                _to_c_float(frequency, "frequency"),
                _to_c_float(click_duration, "click_duration"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


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
    sample_buf = _validate_samples("stft", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareStftResult()
    rc = lib.sonare_stft(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
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


@_guard_buffer("samples")
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
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
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
    sample_buf = _validate_samples("mel_spectrogram", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareMelResult()
    rc = lib.sonare_mel_spectrogram_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(fmax, "fmax"),
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
    sample_buf = _validate_samples("mfcc", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareMfccResult()
    rc = lib.sonare_mfcc_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
        _to_c_int(n_mfcc, "n_mfcc"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(fmax, "fmax"),
        ctypes.c_int(1 if htk else 0),
        _to_c_float(lifter, "lifter"),
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


@_guard_buffer("features")
def mel_delta(
    features: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    width: int = 9,
) -> list[float]:
    """Compute first-order regression deltas of a row-major feature matrix."""
    if n_features <= 0 or n_frames <= 0 or len(features) != n_features * n_frames:
        raise SonareValueError("mel_delta: feature matrix length must equal n_features * n_frames")
    if width < 3 or width % 2 == 0:
        raise SonareValueError("mel_delta: width must be an odd integer of at least 3")
    lib = _get_lib()
    c_array, _ = _to_c_float_array(features)
    out = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_mel_delta(
        c_array,
        _to_c_int(n_features, "n_features"),
        _to_c_int(n_frames, "n_frames"),
        _to_c_int(width, "width"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _float_array_result(out, n_features * n_frames)
    finally:
        if out:
            lib.sonare_free_floats(out)


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
    none is given -- this does NOT auto-estimate and takes no tuning argument.
    A tuning offset from :func:`estimate_tuning` is applied through
    :func:`analyze` (and to chords through :func:`detect_chords`).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        ChromaResult with chroma features and mean energy.
    """
    lib = _get_lib()
    sample_buf = _validate_samples("chroma", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareChromaResult()
    rc = lib.sonare_chroma(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
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
    *,
    wrapper_name: str,
) -> ChromaResult:
    # ``fn_name`` is the C symbol; ``wrapper_name`` is the public Python name the
    # caller used, so the preflight blames the entry point rather than the ABI.
    lib = _get_lib()
    sample_buf = _validate_samples(wrapper_name, samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareChromaResult()
    args: list[object] = [
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(n_chroma, "n_chroma"),
    ]
    if bins_per_octave is not None:
        args.append(_to_c_int(bins_per_octave, "bins_per_octave"))
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
    """Compute CENS chroma features.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12).
        bins_per_octave: Constant-Q bins per octave (default 36). Must be a
            positive multiple of ``n_chroma``: each pitch class takes the mean of
            a whole number of CQT bins, and a resolution that does not divide by
            them has no fold, so 18 is refused against the default 12 classes.

    Returns:
        ChromaResult with chroma features and mean energy.
    """
    return _chroma_variant(
        "sonare_chroma_cens_ex",
        samples,
        sample_rate,
        hop_length,
        n_chroma,
        bins_per_octave,
        wrapper_name="chroma_cens",
    )


def chroma_cqt(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult:
    """Compute a constant-Q chromagram (librosa.feature.chroma_cqt).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12).
        bins_per_octave: Constant-Q bins per octave (default 36). Must be a
            positive multiple of ``n_chroma``: each pitch class takes the mean of
            a whole number of CQT bins, and a resolution that does not divide by
            them has no fold, so 18 is refused against the default 12 classes.

    Returns:
        ChromaResult with chroma features and mean energy.
    """
    return _chroma_variant(
        "sonare_chroma_cqt_ex",
        samples,
        sample_rate,
        hop_length,
        n_chroma,
        bins_per_octave,
        wrapper_name="chroma_cqt",
    )


def bass_chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
) -> ChromaResult:
    """Compute bass-focused chroma features.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12). The constant-Q resolution
            is the library's own here rather than an argument, and every pitch
            class takes the mean of a whole number of its bins, so a class count
            that does not divide 36 -- 5 among them -- is refused.

    Returns:
        ChromaResult with chroma features and mean energy.
    """
    return _chroma_variant(
        "sonare_bass_chroma",
        samples,
        sample_rate,
        hop_length,
        n_chroma,
        wrapper_name="bass_chroma",
    )


# ============================================================================
# Features - Spectral
# ============================================================================


@_guard_buffer("samples")
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
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


@_guard_buffer("samples")
def spectral_bandwidth(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    p: float = 2.0,
) -> list[float]:
    """Compute the spectral bandwidth per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        p: Positive Minkowski exponent (default 2.0).

    Returns:
        List of spectral bandwidth values per frame.
    """
    return _call_float_transform(
        "sonare_spectral_bandwidth_ex",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(p, "p"),
    )


@_guard_buffer("samples")
def spectral_flux(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    lag: int = 1,
) -> list[float]:
    """Compute the unsigned L1 spectral-flux envelope."""
    return _call_float_transform(
        "sonare_spectral_flux",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(lag, "lag"),
    )


def onset_backtrack(
    events: Sequence[int] | list[int], energy: Sequence[float] | list[float]
) -> np.ndarray:
    """Backtrack onset event indices to local energy minima."""
    lib = _get_lib()
    event_array, event_count = _to_c_int_array(events, "events")
    energy_array, energy_count = _to_c_float_array(energy, arg_name="energy")
    with _out_int_array(lib) as (out, out_count):
        _check(
            lib.sonare_onset_backtrack(
                event_array,
                _to_c_size_t(event_count, "event_count"),
                energy_array,
                _to_c_size_t(energy_count, "energy_count"),
                ctypes.byref(out),
                ctypes.byref(out_count),
            )
        )
        return _from_c_int_array(out, out_count.value)


@_guard_buffer("samples")
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
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(roll_percent, "roll_percent"),
    )


@_guard_buffer("samples")
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
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


@_guard_buffer("samples")
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
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(frame_length, "frame_length"),
        _to_c_int(hop_length, "hop_length"),
    )


@_guard_buffer("samples")
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
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(frame_length, "frame_length"),
        _to_c_int(hop_length, "hop_length"),
    )


# ============================================================================
# Features - Spectral contrast / poly / zero-crossings
# ============================================================================


@_guard_buffer("samples")
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

    Note:
        Band 0 spans ``[0, fmin]``, so an ``fmin`` below one analysis bin
        (``sample_rate / n_fft``) leaves it empty after the band trim. Row 0 is
        still finite, but comes from the other bands' extremes rather than from
        itself, and is neither level-invariant nor confined to the band. Keep
        ``fmin`` at or above one bin width for row 0 to mean anything -- at the
        defaults (22050 Hz, 2048) one bin is 10.8 Hz, so only a small ``n_fft``
        or a tiny ``fmin`` reaches this.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_rows = ctypes.c_int()
    out_cols = ctypes.c_int()
    with _out_float_array(lib) as (out, _out_length):
        rc = lib.sonare_spectral_contrast(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_int(hop_length, "hop_length"),
            _to_c_int(n_bands, "n_bands"),
            _to_c_float(fmin, "fmin"),
            _to_c_float(quantile, "quantile"),
            ctypes.byref(out),
            ctypes.byref(out_rows),
            ctypes.byref(out_cols),
        )
        _check(rc)
        rows = int(out_rows.value)
        cols = int(out_cols.value)
        return _from_c_float_array(out, rows * cols).reshape(rows, cols)


@_guard_buffer("samples")
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
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_int(hop_length, "hop_length"),
            _to_c_int(order, "order"),
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
    # A non-finite sample does not merely propagate here: it compares unequal to
    # itself, so the sign test fabricates crossings on both sides of it that are
    # absent from the same signal without the NaN.
    sample_buf = _validate_samples("zero_crossings", samples)
    c_array, length = _to_c_float_array(sample_buf)
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_zero_crossings(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_float(threshold, "threshold"),
            ctypes.c_int(1 if ref_magnitude else 0),
            ctypes.c_int(1 if pad else 0),
            ctypes.c_int(1 if zero_pos else 0),
            ctypes.byref(out),
            ctypes.byref(out_count),
        )
        _check(rc)
        return _from_c_int_array(out, out_count.value)


@_guard_buffer("samples")
def reassigned_spectrogram(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    ref_power: float = 1e-6,
    fill_nan: bool = False,
) -> ReassignedSpectrogramResult:
    """Return magnitude, reassigned times, and reassigned frequencies per STFT bin."""
    if ref_power < 0.0 or not np.isfinite(ref_power):
        raise SonareValueError("reassigned_spectrogram: ref_power must be finite and non-negative")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareReassignedSpectrogramResult()
    rc = lib.sonare_reassigned_spectrogram(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(ref_power, "ref_power"),
        ctypes.c_int(bool(fill_nan)),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        total = out.n_bins * out.n_frames
        return ReassignedSpectrogramResult(
            out.n_bins,
            out.n_frames,
            _float_array_result(out.magnitude, total),
            _float_array_result(out.times, total),
            _float_array_result(out.frequencies, total),
        )
    finally:
        lib.sonare_free_reassigned_spectrogram_result(ctypes.byref(out))
