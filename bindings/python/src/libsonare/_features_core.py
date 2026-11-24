"""Feature extraction wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np
import numpy.typing as npt

from ._errors import _entry, _refuse, _refuse_type
from ._features_transforms import _CQT_HOP_LENGTH_MAX
from ._ffi import (
    SonareChromaCensOptions,
    SonareChromaCqtOptions,
    SonareChromaResult,
    SonareIstftOptions,
    SonareMelResult,
    SonareMelSpectrogramOptions,
    SonareMfccOptions,
    SonareMfccResult,
    SonareReassignedSpectrogramResult,
    SonareSpectralBandwidthOptions,
    SonareStftOptions,
    SonareStftResult,
)
from ._field_spelling import surface_spelling
from ._narrowing import _optional_nonzero_domain_float
from ._runtime import (
    _C_INT_MAX,
    _call_float_array_transform,
    _check,
    _from_c_float_array,
    _from_c_float_matrix,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _out_int_array,
    _resolve_zero_default,
    _to_c_float,
    _to_c_float_array,
    _to_c_float_matrix,
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


@_entry("tone")
def tone(
    frequency: float = 440.0,
    sr: int = 22050,
    duration: float = 1.0,
    phi: float = 0.0,
    amplitude: float = 1.0,
) -> np.ndarray:
    """Generate a sine tone as a float32 array."""
    lib = _get_lib()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_tone(
                _to_c_float(frequency, "frequency"),
                _to_c_int(sr, "sr"),
                _to_c_float(duration, "duration"),
                _to_c_float(phi, "phi"),
                _to_c_float(amplitude, "amplitude"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("chirp")
def chirp(
    fmin: float = 440.0,
    fmax: float = 880.0,
    sr: int = 22050,
    duration: float = 1.0,
    linear: bool = False,
) -> np.ndarray:
    """Generate a linear or exponential chirp as a float32 array."""
    lib = _get_lib()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_chirp(
                _to_c_float(fmin, "fmin"),
                _to_c_float(fmax, "fmax"),
                _to_c_int(sr, "sr"),
                _to_c_float(duration, "duration"),
                1 if linear else 0,
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("clicks")
def clicks(
    times: Sequence[float] | list[float],
    sr: int = 22050,
    length: int | None = None,
    click_freq: float = 1000.0,
    click_duration: float = 0.1,
) -> np.ndarray:
    """Generate decaying sine clicks at times in seconds as a float32 array.

    ``length`` is the output length in samples; ``None`` ends after the last click.
    """
    lib = _get_lib()
    data, count = _to_c_float_array(times, arg_name="times")
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_clicks(
                data,
                _to_c_size_t(count, "times length"),
                _to_c_int(sr, "sr"),
                ctypes.c_size_t(
                    _narrow_int(_resolve_zero_default(length, "length"), "length", 0, _C_INT_MAX)
                ),
                _to_c_float(click_freq, "click_freq"),
                _to_c_float(click_duration, "click_duration"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("stft")
def stft(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    *,
    db: bool = False,
    phase: bool = False,
) -> StftResult:
    """Compute the short-time Fourier transform.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        db: Also compute the magnitude in dB (default False).
        phase: Also compute the phase in radians (default False).

    Returns:
        StftResult with magnitude and power spectrograms, each of shape
        ``(n_fft // 2 + 1, n_frames)``; ``db`` holds the magnitude in dB with
        the same shape when ``db`` is True and is ``None`` otherwise; phase
        holds the phase in radians with that shape when requested and is None
        otherwise.
    """
    if not isinstance(phase, bool):
        raise _refuse_type("phase must be a bool")
    lib = _get_lib()
    sample_buf = _validate_samples(samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareStftResult()
    options = SonareStftOptions._given(db=1 if db else 0, phase=1 if phase else 0)
    rc = lib.sonare_stft(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return StftResult(
            n_fft=out.n_fft,
            hop_length=out.hop_length,
            sample_rate=out.sample_rate,
            magnitude=_from_c_float_matrix(out.magnitude, out.n_bins, out.n_frames),
            power=_from_c_float_matrix(out.power, out.n_bins, out.n_frames),
            db=_from_c_float_matrix(out.db, out.n_bins, out.n_frames) if db else None,
            phase=(_from_c_float_matrix(out.phase, out.n_bins, out.n_frames) if phase else None),
        )
    finally:
        lib.sonare_free_stft_result(ctypes.byref(out))


@_entry("istft")
def istft(
    magnitude: npt.ArrayLike,
    phase: npt.ArrayLike,
    n_fft: int | None = None,
    hop_length: int | None = None,
    *,
    length_samples: int | None = None,
) -> np.ndarray:
    """Reconstruct centered audio from STFT magnitude and phase matrices."""
    magnitude_data, n_bins, n_frames = _to_c_float_matrix(
        magnitude, arg_name="magnitude", scan=False, allow_empty=True
    )
    phase_data, phase_bins, phase_frames = _to_c_float_matrix(
        phase, arg_name="phase", scan=False, allow_empty=True
    )
    if (phase_bins, phase_frames) != (n_bins, n_frames):
        raise _refuse("magnitude and phase must have the same shape")

    fft_size = 2 * (n_bins - 1) if n_fft is None else n_fft
    fft_arg = _to_c_int(fft_size, "n_fft")
    hop_size = max(1, int(fft_arg.value) // 4) if hop_length is None else hop_length
    hop_arg = _to_c_size_t(hop_size, "hop_length")

    options = None
    if length_samples is not None:
        length_arg = _to_c_size_t(length_samples, "length_samples")
        if length_arg.value == 0:
            raise _refuse("length_samples must not be 0; omit it for the natural length")
        options = SonareIstftOptions()
        options.length_samples = length_arg.value

    lib = _get_lib()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_istft(
                magnitude_data,
                phase_data,
                _to_c_int(n_bins, "magnitude row count"),
                _to_c_int(n_frames, "magnitude frame count"),
                fft_arg,
                hop_arg,
                None if options is None else ctypes.byref(options),
                ctypes.byref(out),
                ctypes.byref(out_length),
            ),
            flattened="options",
        )
        return _from_c_float_array(out, out_length.value)


# ============================================================================
# Features - Mel
# ============================================================================


@_entry("mel_spectrogram")
def mel_spectrogram(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    fmin_hz: float = 0.0,
    fmax_hz: float | None = None,
    htk: bool = False,
) -> MelSpectrogramResult:
    """Compute a Mel spectrogram.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).
        fmin_hz: Lower Mel band edge in Hz (default 0.0). Set together with
            ``fmax_hz`` to round-trip with ``mel_to_stft`` / ``mel_to_audio``.
        fmax_hz: Upper Mel band edge in Hz; None (default) is the Nyquist.
            Must be greater than 0 and no higher than Nyquist when given.
        htk: Use the HTK Mel formula instead of Slaney (default False).

    Returns:
        MelSpectrogramResult with power and dB spectrograms, each of shape
        ``(n_mels, n_frames)``.
    """
    lib = _get_lib()
    sample_buf = _validate_samples(samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareMelResult()
    options = SonareMelSpectrogramOptions._given(
        fmin_hz=_to_c_float(fmin_hz, "fmin_hz"),
        fmax_hz=_optional_nonzero_domain_float(fmax_hz, "fmax_hz"),
        htk=1 if htk else 0,
    )
    rc = lib.sonare_mel_spectrogram(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return MelSpectrogramResult(
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            power=_from_c_float_matrix(out.power, out.n_mels, out.n_frames),
            db=_from_c_float_matrix(out.db, out.n_mels, out.n_frames),
            n_fft=out.n_fft,
            fmin_hz=float(out.fmin_hz),
            fmax_hz=float(out.fmax_hz),
            htk=bool(out.htk),
        )
    finally:
        lib.sonare_free_mel_result(ctypes.byref(out))


@_entry("mfcc")
def mfcc(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 20,
    fmin: float = 0.0,
    fmax: float | None = None,
    htk: bool = False,
    lifter: float = 0.0,
) -> MfccResult:
    """Compute Mel-frequency cepstral coefficients.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        n_mels: Number of Mel bands (default 128).
        n_mfcc: Number of MFCC coefficients (default 20).
        fmin: Minimum Mel frequency in Hz (default 0.0 = librosa default).
        fmax: Maximum Mel frequency in Hz (default None = sr / 2); values above Nyquist are
            refused; an explicit 0 is not absence.
        htk: Use the HTK Mel formula instead of Slaney (default False).
        lifter: Cepstral liftering coefficient (default 0.0 = no liftering).

    Returns:
        MfccResult whose coefficients have shape ``(n_mfcc, n_frames)``.
    """
    lib = _get_lib()
    sample_buf = _validate_samples(samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareMfccResult()
    options = SonareMfccOptions._given(
        fmin_hz=_to_c_float(fmin, "fmin"),
        fmax_hz=_optional_nonzero_domain_float(fmax, "fmax"),
        htk=1 if htk else 0,
        lifter=_to_c_float(lifter, "lifter"),
    )
    rc = lib.sonare_mfcc(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
        _to_c_int(n_mfcc, "n_mfcc"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        return MfccResult(
            coefficients=_from_c_float_matrix(out.coefficients, out.n_mfcc, out.n_frames),
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            n_fft=out.n_fft,
            n_mels=out.n_mels,
            fmin_hz=float(out.fmin_hz),
            fmax_hz=float(out.fmax_hz),
            htk=bool(out.htk),
            lifter=float(out.lifter),
        )
    finally:
        lib.sonare_free_mfcc_result(ctypes.byref(out))


@_entry("mel_delta")
def mel_delta(features: npt.ArrayLike, width: int = 9) -> np.ndarray:
    """Compute first-order regression deltas of a feature matrix.

    ``features`` is a 2-D array of shape ``(n_features, n_frames)``; the result is
    a float32 array of the same shape.
    """
    lib = _get_lib()
    c_array, n_features, n_frames = _to_c_float_matrix(features, arg_name="features")
    out = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_mel_delta(
        c_array,
        _to_c_int(n_features, "features row count"),
        _to_c_int(n_frames, "features length"),
        _to_c_int(width, "width"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _from_c_float_matrix(out, n_features, n_frames)
    finally:
        if out:
            lib.sonare_free_floats(out)


# ============================================================================
# Features - Chroma
# ============================================================================


@_entry("chroma")
def chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> ChromaResult:
    """Compute STFT chroma features (librosa.feature.chroma_stft).

    The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
    librosa.feature.chroma_stft -- which estimates tuning from the signal when
    none is given -- this does NOT auto-estimate and takes no tuning argument.
    A tuning offset in cents is applied through :func:`analyze` (and to chords
    through :func:`detect_chords`); both also measure it themselves with
    ``tuning_cents="auto"``.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        ChromaResult with chroma features of shape ``(n_chroma, n_frames)`` and
        the mean energy of each class.
    """
    lib = _get_lib()
    sample_buf = _validate_samples(samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareChromaResult()
    rc = lib.sonare_chroma(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return ChromaResult(
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            features=_from_c_float_matrix(out.features, out.n_chroma, out.n_frames),
            mean_energy=_from_c_float_array(out.mean_energy, out.n_chroma),
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
    options_cls: type[SonareChromaCensOptions] | type[SonareChromaCqtOptions] | None = None,
    wrapper_name: str,
) -> ChromaResult:
    # ``fn_name`` is the C symbol; ``wrapper_name`` is the public Python name the
    # caller used, so the preflight blames the entry point rather than the ABI.
    lib = _get_lib()
    sample_buf = _validate_samples(samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonareChromaResult()
    args: list[object] = [
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, surface_spelling(wrapper_name, "sample_rate")),
        _narrow_int(hop_length, "hop_length", 1, _CQT_HOP_LENGTH_MAX),
        _to_c_int(n_chroma, "n_chroma"),
    ]
    if options_cls is not None:
        options = options_cls._given(
            bins_per_octave=None
            if bins_per_octave is None
            else _to_c_int(bins_per_octave, "bins_per_octave")
        )
        args.append(ctypes.byref(options))
    args.append(ctypes.byref(out))
    rc = getattr(lib, fn_name)(*args)
    _check(rc, flattened="options")
    try:
        return ChromaResult(
            sample_rate=out.sample_rate,
            hop_length=out.hop_length,
            features=_from_c_float_matrix(out.features, out.n_chroma, out.n_frames),
            mean_energy=_from_c_float_array(out.mean_energy, out.n_chroma),
        )
    finally:
        lib.sonare_free_chroma_result(ctypes.byref(out))


@_entry("chroma_cens")
def chroma_cens(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult:
    """Compute CENS chroma features.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12).
        bins_per_octave: Constant-Q bins per octave (default 36). Must be a
            positive multiple of ``n_chroma``: each pitch class takes the mean of
            a whole number of CQT bins, and a resolution that does not divide by
            them has no fold, so 18 is refused against the default 12 classes.

    Returns:
        ChromaResult with chroma features of shape ``(n_chroma, n_frames)`` and
        the mean energy of each class.
    """
    return _chroma_variant(
        "sonare_chroma_cens",
        samples,
        sr,
        hop_length,
        n_chroma,
        bins_per_octave,
        options_cls=SonareChromaCensOptions,
        wrapper_name="chroma_cens",
    )


@_entry("chroma_cqt")
def chroma_cqt(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult:
    """Compute a constant-Q chromagram (librosa.feature.chroma_cqt).

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12).
        bins_per_octave: Constant-Q bins per octave (default 36). Must be a
            positive multiple of ``n_chroma``: each pitch class takes the mean of
            a whole number of CQT bins, and a resolution that does not divide by
            them has no fold, so 18 is refused against the default 12 classes.

    Returns:
        ChromaResult with chroma features of shape ``(n_chroma, n_frames)`` and
        the mean energy of each class.
    """
    return _chroma_variant(
        "sonare_chroma_cqt",
        samples,
        sr,
        hop_length,
        n_chroma,
        bins_per_octave,
        options_cls=SonareChromaCqtOptions,
        wrapper_name="chroma_cqt",
    )


@_entry("bass_chroma")
def bass_chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    hop_length: int = 512,
    n_chroma: int = 12,
) -> ChromaResult:
    """Compute bass-focused chroma features.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        hop_length: Hop length in samples (default 512).
        n_chroma: Number of pitch classes (default 12). The constant-Q resolution
            is the library's own here rather than an argument, and every pitch
            class takes the mean of a whole number of its bins, so a class count
            that does not divide 36 -- 5 among them -- is refused.

    Returns:
        ChromaResult with chroma features of shape ``(n_chroma, n_frames)`` and
        the mean energy of each class.
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


@_entry("spectral_centroid")
@_guard_buffer("samples")
def spectral_centroid(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Compute the spectral centroid per frame.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        A float32 array of spectral centroid values, one per frame.
    """
    return _call_float_array_transform(
        "sonare_spectral_centroid",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
    )


@_entry("spectral_bandwidth")
@_guard_buffer("samples")
def spectral_bandwidth(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    p: float = 2.0,
) -> np.ndarray:
    """Compute the spectral bandwidth per frame.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        p: Positive Minkowski exponent (default 2.0).

    Returns:
        A float32 array of spectral bandwidth values, one per frame.
    """
    options = SonareSpectralBandwidthOptions._given(p=_to_c_float(p, "p"))
    return _call_float_array_transform(
        "sonare_spectral_bandwidth",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        ctypes.byref(options),
        flattened="options",
    )


@_entry("spectral_flux")
@_guard_buffer("samples")
def spectral_flux(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    lag: int = 1,
) -> np.ndarray:
    """Compute the unsigned L1 spectral-flux envelope as a float32 array."""
    return _call_float_array_transform(
        "sonare_spectral_flux",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(lag, "lag"),
    )


@_entry("onset_backtrack")
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


@_entry("spectral_rolloff")
@_guard_buffer("samples")
def spectral_rolloff(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    roll_percent: float = 0.85,
) -> np.ndarray:
    """Compute the spectral rolloff per frame.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        roll_percent: Rolloff percentage (default 0.85).

    Returns:
        A float32 array of spectral rolloff values, one per frame.
    """
    return _call_float_array_transform(
        "sonare_spectral_rolloff",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_float(roll_percent, "roll_percent"),
    )


@_entry("spectral_flatness")
@_guard_buffer("samples")
def spectral_flatness(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Compute the spectral flatness per frame.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        A float32 array of spectral flatness values, one per frame.
    """
    return _call_float_array_transform(
        "sonare_spectral_flatness",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
    )


@_entry("zero_crossing_rate")
@_guard_buffer("samples")
def zero_crossing_rate(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Compute the zero-crossing rate per frame.

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        A float32 array of zero-crossing rate values, one per frame.
    """
    return _call_float_array_transform(
        "sonare_zero_crossing_rate",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_size_t(frame_length, "frame_length"),
        _to_c_size_t(hop_length, "hop_length"),
    )


@_entry("rms_energy")
@_guard_buffer("samples")
def rms_energy(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Compute the RMS energy per frame.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).

    Returns:
        A float32 array of RMS energy values, one per frame.
    """
    return _call_float_array_transform(
        "sonare_rms_energy",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_size_t(frame_length, "frame_length"),
        _to_c_size_t(hop_length, "hop_length"),
    )


# ============================================================================
# Features - Spectral contrast / poly / zero-crossings
# ============================================================================


@_entry("spectral_contrast")
@_guard_buffer("samples")
def spectral_contrast(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_bands: int = 6,
    fmin: float = 200.0,
    quantile: float = 0.02,
) -> np.ndarray:
    """Compute spectral contrast (librosa.feature.spectral_contrast).

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
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
        (``sr / n_fft``) leaves it empty after the band trim. Row 0 is
        still finite, but comes from the other bands' extremes rather than from
        itself, and is neither level-invariant nor confined to the band. Keep
        ``fmin`` at or above one bin width for row 0 to mean anything -- at the
        defaults (22050 Hz, 2048) one bin is 10.8 Hz, so only a small ``n_fft``
        or a tiny ``fmin`` reaches this.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_n_subbands = ctypes.c_int()
    out_n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_spectral_contrast(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sr, "sr"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_int(n_bands, "n_bands"),
            _to_c_float(fmin, "fmin"),
            _to_c_float(quantile, "quantile"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(out_n_subbands),
            ctypes.byref(out_n_frames),
        )
        _check(rc)
        rows = int(out_n_subbands.value)
        cols = int(out_n_frames.value)
        return _from_c_float_array(out, int(out_length.value)).reshape(rows, cols)


@_entry("poly_features")
@_guard_buffer("samples")
def poly_features(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    order: int = 1,
) -> np.ndarray:
    """Fit polynomial coefficients per frame (librosa.feature.poly_features).

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        order: Polynomial order (default 1).

    Returns:
        A float32 array of shape ``(order + 1, n_frames)`` (matches the
        bare-ndarray convention of ``stft`` / ``mel_spectrogram`` / ``nn_filter``).
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_n_coefficients = ctypes.c_int()
    out_n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_poly_features(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sr, "sr"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_int(order, "order"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(out_n_coefficients),
            ctypes.byref(out_n_frames),
        )
        _check(rc)
        rows = int(out_n_coefficients.value)
        cols = int(out_n_frames.value)
        return _from_c_float_array(out, int(out_length.value)).reshape(rows, cols)


@_entry("zero_crossings")
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


@_entry("reassigned_spectrogram")
@_guard_buffer("samples")
def reassigned_spectrogram(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    ref_power: float = 1e-6,
    fill_nan: bool = False,
) -> ReassignedSpectrogramResult:
    """Return magnitude, reassigned times and reassigned frequencies per STFT bin.

    Each field is a float32 array of shape ``(n_bins, n_frames)``.
    """
    if ref_power < 0.0 or not np.isfinite(ref_power):
        raise _refuse("ref_power must be finite and at least 0")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareReassignedSpectrogramResult()
    rc = lib.sonare_reassigned_spectrogram(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_float(ref_power, "ref_power"),
        ctypes.c_int(bool(fill_nan)),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return ReassignedSpectrogramResult(
            magnitude=_from_c_float_matrix(out.magnitude, out.n_bins, out.n_frames),
            times=_from_c_float_matrix(out.times, out.n_bins, out.n_frames),
            frequencies=_from_c_float_matrix(out.frequencies, out.n_bins, out.n_frames),
        )
    finally:
        lib.sonare_free_reassigned_spectrogram_result(ctypes.byref(out))
