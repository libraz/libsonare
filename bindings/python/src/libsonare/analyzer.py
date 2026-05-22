"""High-level analysis functions wrapping the libsonare C quick API."""

from __future__ import annotations

import contextlib
import ctypes
from collections.abc import Sequence

from ._ffi import (
    SONARE_OK,
    SonareAnalysisResult,
    SonareBpmAnalysisResult,
    SonareChordAnalysisResult,
    SonareChromaResult,
    SonareDynamicsResult,
    SonareHpssResult,
    SonareKey,
    SonareMasteringChainResult,
    SonareMasteringChainStereoResult,
    SonareMasteringConfig,
    SonareMasteringParam,
    SonareMasteringResult,
    SonareMasteringStereoResult,
    SonareMelResult,
    SonareMfccResult,
    SonarePitchResult,
    SonareRhythmResult,
    SonareStftResult,
    SonareTimbreResult,
    load_library,
)
from .types import (
    AnalysisResult,
    BpmAnalysisResult,
    BpmCandidate,
    Chord,
    ChordAnalysisResult,
    ChromaResult,
    DynamicsResult,
    HpssResult,
    Key,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringResult,
    MasteringStereoResult,
    MelSpectrogramResult,
    MfccResult,
    Mode,
    PitchClass,
    PitchResult,
    RhythmResult,
    StftResult,
    TimbreResult,
    TimeSignature,
)

_lib: ctypes.CDLL | None = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure.

    When the C layer recorded a detailed thread-local message
    (``sonare_last_error_message``), it is preferred over the generic
    ``sonare_error_message(rc)`` fallback so users see the underlying cause.
    """
    if rc != SONARE_OK:
        lib = _get_lib()
        detail = lib.sonare_last_error_message()
        detail_str = detail.decode("utf-8") if detail else ""
        if detail_str:
            raise RuntimeError(detail_str)
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


def _to_c_float_array(
    samples: Sequence[float] | list[float],
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array."""
    length = len(samples)
    c_array = (ctypes.c_float * length)(*samples)
    return c_array, length


def _to_c_int_array(values: Sequence[int] | list[int]) -> tuple[ctypes.Array[ctypes.c_int], int]:
    length = len(values)
    c_array = (ctypes.c_int * length)(*values)
    return c_array, length


def _float_array_result(out: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    return [float(out[i]) for i in range(count)]


def _int_array_result(out: ctypes.POINTER(ctypes.c_int), count: int) -> list[int]:
    return [int(out[i]) for i in range(count)]


def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    One-shot wrapper for raw sample input. When you load from a file or call
    multiple analyses on the same signal, prefer :class:`libsonare.Audio` and
    :meth:`Audio.detect_bpm` to avoid re-copying samples across the FFI.

    Args:
        samples: Mono audio samples (1D, nominally ``[-1.0, 1.0]``). Accepts
            ``list[float]``, ``tuple[float, ...]``, ``array.array``, or a numpy
            1D array of dtype ``float32``.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected BPM as a ``float``. For confidence, use :func:`analyze`.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out_bpm)
    )
    _check(rc)
    return float(out_bpm.value)


def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> Key:
    """Detect the musical key of audio samples.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        :class:`libsonare.Key` with ``root`` (:class:`PitchClass`), ``mode``
        (:class:`Mode`), and ``confidence`` (``float`` in ``[0, 1]``).

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_key = SonareKey()
    rc = lib.sonare_detect_key(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out_key)
    )
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
    try:
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]
    finally:
        if out_times and out_count.value > 0:
            lib.sonare_free_floats(out_times)


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
    try:
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]
    finally:
        if out_times and out_count.value > 0:
            lib.sonare_free_floats(out_times)


def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        :class:`libsonare.AnalysisResult` with ``bpm`` (``float``),
        ``bpm_confidence`` (``float`` in ``[0, 1]``), ``key``
        (:class:`Key`), ``time_signature`` (:class:`TimeSignature`),
        and ``beat_times`` (``list[float]`` in seconds).

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAnalysisResult()
    rc = lib.sonare_analyze(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out)
    )
    _check(rc)
    try:
        beat_times = [float(out.beat_times[i]) for i in range(out.beat_count)]

        return AnalysisResult(
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
    finally:
        lib.sonare_free_result(ctypes.byref(out))


def analyze_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    bpm_min: float = 30.0,
    bpm_max: float = 300.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
    max_candidates: int = 5,
) -> BpmAnalysisResult:
    """Analyze BPM with confidence, candidates, autocorrelation, and tempogram."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareBpmAnalysisResult()
    rc = lib.sonare_analyze_bpm(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(bpm_min),
        ctypes.c_float(bpm_max),
        ctypes.c_float(start_bpm),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(max_candidates),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return BpmAnalysisResult(
            bpm=float(out.bpm),
            confidence=float(out.confidence),
            candidates=[
                BpmCandidate(
                    bpm=float(out.candidates[i].bpm),
                    confidence=float(out.candidates[i].confidence),
                )
                for i in range(out.candidate_count)
            ],
            autocorrelation=[
                float(out.autocorrelation[i]) for i in range(out.autocorrelation_count)
            ],
            tempogram=[float(out.tempogram[i]) for i in range(out.tempogram_count)],
        )
    finally:
        lib.sonare_free_bpm_analysis_result(ctypes.byref(out))


def analyze_rhythm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    bpm_min: float = 60.0,
    bpm_max: float = 200.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> RhythmResult:
    """Analyze rhythm primitives without generating a summary report."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareRhythmResult()
    rc = lib.sonare_analyze_rhythm(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(bpm_min),
        ctypes.c_float(bpm_max),
        ctypes.c_float(start_bpm),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
    )
    _check(rc)
    groove_names = {0: "straight", 1: "shuffle", 2: "swing"}
    try:
        return RhythmResult(
            bpm=float(out.bpm),
            time_signature=TimeSignature(
                numerator=int(out.time_signature.numerator),
                denominator=int(out.time_signature.denominator),
                confidence=float(out.time_signature.confidence),
            ),
            groove_type=groove_names.get(int(out.groove_type), "straight"),
            syncopation=float(out.syncopation),
            pattern_regularity=float(out.pattern_regularity),
            tempo_stability=float(out.tempo_stability),
            beat_intervals=[float(out.beat_intervals[i]) for i in range(out.beat_interval_count)],
        )
    finally:
        lib.sonare_free_rhythm_result(ctypes.byref(out))


def analyze_dynamics(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    window_sec: float = 0.4,
    hop_length: int = 512,
    compression_threshold: float = 6.0,
) -> DynamicsResult:
    """Analyze dynamics and loudness primitives."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareDynamicsResult()
    rc = lib.sonare_analyze_dynamics(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(window_sec),
        ctypes.c_int(hop_length),
        ctypes.c_float(compression_threshold),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return DynamicsResult(
            dynamic_range_db=float(out.dynamic_range_db),
            peak_db=float(out.peak_db),
            rms_db=float(out.rms_db),
            crest_factor=float(out.crest_factor),
            loudness_range_db=float(out.loudness_range_db),
            is_compressed=bool(out.is_compressed),
            loudness_times=[float(out.loudness_times[i]) for i in range(out.loudness_count)],
            loudness_rms_db=[float(out.loudness_rms_db[i]) for i in range(out.loudness_count)],
        )
    finally:
        lib.sonare_free_dynamics_result(ctypes.byref(out))


def analyze_timbre(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 13,
    window_sec: float = 0.5,
) -> TimbreResult:
    """Analyze timbre and spectral-shape primitives."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareTimbreResult()
    rc = lib.sonare_analyze_timbre(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
        ctypes.c_int(n_mfcc),
        ctypes.c_float(window_sec),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return TimbreResult(
            brightness=float(out.brightness),
            warmth=float(out.warmth),
            density=float(out.density),
            roughness=float(out.roughness),
            complexity=float(out.complexity),
            spectral_centroid=[
                float(out.spectral_centroid[i]) for i in range(out.spectral_centroid_count)
            ],
            spectral_flatness=[
                float(out.spectral_flatness[i]) for i in range(out.spectral_flatness_count)
            ],
            spectral_rolloff=[
                float(out.spectral_rolloff[i]) for i in range(out.spectral_rolloff_count)
            ],
        )
    finally:
        lib.sonare_free_timbre_result(ctypes.byref(out))


def detect_chords(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    min_duration: float = 0.3,
    smoothing_window: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = False,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
) -> ChordAnalysisResult:
    """Detect chord segments without generating a summary report."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareChordAnalysisResult()
    rc = lib.sonare_detect_chords(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(min_duration),
        ctypes.c_float(smoothing_window),
        ctypes.c_float(threshold),
        ctypes.c_int(1 if use_triads_only else 0),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(1 if use_beat_sync else 0),
        ctypes.byref(out),
    )
    _check(rc)
    quality_names = {
        0: "major",
        1: "minor",
        2: "diminished",
        3: "augmented",
        4: "dominant7",
        5: "major7",
        6: "minor7",
        7: "sus2",
        8: "sus4",
        9: "unknown",
    }
    try:
        return ChordAnalysisResult(
            chords=[
                Chord(
                    root=PitchClass(out.chords[i].root),
                    quality=quality_names.get(int(out.chords[i].quality), "unknown"),
                    start=float(out.chords[i].start),
                    end=float(out.chords[i].end),
                    confidence=float(out.chords[i].confidence),
                )
                for i in range(out.chord_count)
            ]
        )
    finally:
        lib.sonare_free_chord_analysis_result(ctypes.byref(out))


def version() -> str:
    """Return the libsonare version string."""
    lib = _get_lib()
    v = lib.sonare_version()
    return v.decode("utf-8") if v else ""


def has_ffmpeg_support() -> bool:
    """Return whether the loaded libsonare was compiled with FFmpeg support.

    When ``True``, :meth:`Audio.from_file` / :meth:`Audio.from_memory` can
    decode M4A, AAC, FLAC, OGG, Opus and any other container/codec supported
    by the linked FFmpeg. When ``False``, only WAV and MP3 are supported
    and unsupported formats raise an actionable :class:`RuntimeError`.
    """
    lib = _get_lib()
    return bool(lib.sonare_has_ffmpeg_support())


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
    rc = lib.sonare_hpss(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(kernel_harmonic),
        ctypes.c_int(kernel_percussive),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=[float(out.harmonic[i]) for i in range(n)],
            percussive=[float(out.percussive[i]) for i in range(n)],
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


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
    rc = lib.sonare_harmonic(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
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
    rc = lib.sonare_percussive(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
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
    rc = lib.sonare_time_stretch(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
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
    rc = lib.sonare_pitch_shift(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(semitones),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
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
    rc = lib.sonare_normalize(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(target_db),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def mastering(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    true_peak_oversample: int = 4,
) -> MasteringResult:
    """Apply mastering loudness normalization with a true-peak ceiling."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_process"):
        raise RuntimeError("libsonare was built without mastering support")

    c_array, length = _to_c_float_array(samples)
    config = SonareMasteringConfig(
        target_lufs=target_lufs,
        ceiling_db=ceiling_db,
        true_peak_oversample=true_peak_oversample,
    )
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_process(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        processed = [float(out.samples[i]) for i in range(out.length)]
        return MasteringResult(
            samples=processed,
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def _mastering_params(params: dict[str, float | int | bool] | None):
    items = list((params or {}).items())
    array_type = SonareMasteringParam * len(items)
    key_buffers = [str(key).encode("utf-8") for key, _ in items]
    array = array_type(
        *[
            SonareMasteringParam(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )
    return array, len(items)


def mastering_processor_names() -> list[str]:
    """Return supported mastering processor names shared by CLI/Node/WASM/Python."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_processor_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_pair_processor_names() -> list[str]:
    """Return supported two-input mastering processor names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_pair_processor_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_pair_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_pair_analysis_names() -> list[str]:
    """Return supported two-input mastering analysis names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_pair_analysis_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_pair_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_stereo_analysis_names() -> list[str]:
    """Return supported stereo mastering analysis names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_stereo_analysis_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_stereo_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_process(
    processor_name: str,
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringResult:
    """Apply a named mastering processor using the shared cross-language API."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_processor"):
        raise RuntimeError("libsonare was built without mastering support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_apply_processor(
        processor_name.encode("utf-8"),
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def mastering_process_stereo(
    processor_name: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringStereoResult:
    """Apply a named stereo mastering processor using the shared cross-language API."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_processor_stereo"):
        raise RuntimeError("libsonare was built without mastering support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringStereoResult()
    rc = lib.sonare_mastering_apply_processor_stereo(
        processor_name.encode("utf-8"),
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_stereo_result(ctypes.byref(out))


def _flatten_chain_config(
    config: dict | None,
    prefix: str = "",
) -> dict[str, float]:
    """Flatten a nested chain config dict using dot-notation keys.

    Accepts both nested (``{"dynamics": {"compressor": {"thresholdDb": -24}}}``)
    and flat (``{"dynamics.compressor.thresholdDb": -24}``) representations.
    Booleans are coerced to 0.0/1.0; other values are coerced via ``float``.
    """
    flat: dict[str, float] = {}
    if not config:
        return flat
    for key, value in config.items():
        full_key = f"{prefix}{key}" if not prefix else f"{prefix}.{key}"
        if isinstance(value, dict):
            flat.update(_flatten_chain_config(value, full_key))
        elif isinstance(value, bool):
            flat[full_key] = 1.0 if value else 0.0
        else:
            flat[full_key] = float(value)
    return flat


def _chain_params(config: dict | None):
    flat = _flatten_chain_config(config)
    items = list(flat.items())
    array_type = SonareMasteringParam * len(items)
    key_buffers = [str(key).encode("utf-8") for key, _ in items]
    array = array_type(
        *[
            SonareMasteringParam(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )
    return array, len(items)


def _extract_stages(stages_ptr, count: int) -> list[str]:
    if not stages_ptr or count == 0:
        return []
    result: list[str] = []
    for i in range(count):
        raw = stages_ptr[i]
        result.append(raw.decode("utf-8") if raw else "")
    return result


def mastering_chain(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict | None = None,
) -> MasteringChainResult:
    """Apply a configurable mastering chain to mono audio.

    The chain composes (in fixed order) repair, EQ, dynamics, saturation,
    spectral, maximizer, and loudness stages. Each stage is enabled either
    by passing ``"<stage>.enabled": True`` or by setting any field under
    ``"<stage>.*"``. Unknown keys raise ``RuntimeError``.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        config: Either a flat dict using dot-notation keys (e.g.
            ``{"dynamics.compressor.thresholdDb": -24}``) or a nested dict
            (``{"dynamics": {"compressor": {"thresholdDb": -24}}}``). The
            two forms can be freely mixed.

    Returns:
        :class:`MasteringChainResult` with processed samples, LUFS info,
        and the ordered list of stages that ran.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_chain"):
        raise RuntimeError("libsonare was built without mastering chain support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _chain_params(config)
    out = SonareMasteringChainResult()
    rc = lib.sonare_mastering_chain(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringChainResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_result(ctypes.byref(out))


def mastering_chain_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict | None = None,
) -> MasteringChainStereoResult:
    """Apply a configurable mastering chain to stereo audio.

    See :func:`mastering_chain` for ``config`` semantics. The stereo path
    also recognises ``stereo.imager.*`` and ``stereo.monoMaker.*`` stages.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_chain_stereo"):
        raise RuntimeError("libsonare was built without mastering chain support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _chain_params(config)
    out = SonareMasteringChainStereoResult()
    rc = lib.sonare_mastering_chain_stereo(
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringChainStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_stereo_result(ctypes.byref(out))


def mastering_preset_names() -> list[str]:
    """Return built-in mastering preset identifiers (e.g. ``"pop"``, ``"aiMusic"``)."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_preset_names"):
        raise RuntimeError("libsonare was built without mastering preset support")
    raw = lib.sonare_mastering_preset_names()
    return raw.decode("utf-8").splitlines() if raw else []


def master_audio(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    preset: str = "pop",
    overrides: dict[str, float | int | bool] | None = None,
) -> MasteringChainResult:
    """Apply a named mastering preset chain to mono audio.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        preset: Preset identifier from :func:`mastering_preset_names`.
        overrides: Optional flat / nested overrides applied on top of the preset
            defaults. Keys use the same dot-notation as :func:`mastering_chain`.

    Returns:
        :class:`MasteringChainResult` with processed samples, LUFS info, and the
        ordered list of stages that ran.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_master_audio"):
        raise RuntimeError("libsonare was built without mastering preset support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _chain_params(overrides)
    out = SonareMasteringChainResult()
    rc = lib.sonare_master_audio(
        preset.encode("utf-8"),
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringChainResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_result(ctypes.byref(out))


def master_audio_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    preset: str = "pop",
    overrides: dict[str, float | int | bool] | None = None,
) -> MasteringChainStereoResult:
    """Apply a named mastering preset chain to stereo audio.

    See :func:`master_audio` for the ``preset`` and ``overrides`` semantics.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_master_audio_stereo"):
        raise RuntimeError("libsonare was built without mastering preset support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _chain_params(overrides)
    out = SonareMasteringChainStereoResult()
    rc = lib.sonare_master_audio_stereo(
        preset.encode("utf-8"),
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringChainStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_stereo_result(ctypes.byref(out))


class StreamingMasteringChain:
    """Block-by-block streaming variant of :func:`mastering_chain`.

    Maintains processor state across :meth:`process_mono`/:meth:`process_stereo`
    calls. Only ProcessorBase-backed stages (eq.tilt, dynamics.compressor,
    saturation.tape, saturation.exciter, spectral.airBand, stereo.imager,
    stereo.monoMaker, maximizer.truePeakLimiter) are supported. Configurations
    that enable ``repair.denoise`` or ``loudness`` raise :class:`RuntimeError`.

    Example::

        chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
        chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
        out = chain.process_mono([0.1] * 512)
        chain.reset()

    Can also be used as a context manager to ensure the underlying handle is
    released::

        with StreamingMasteringChain({"eq.tilt.tiltDb": 1.0}) as chain:
            chain.prepare(44100, 512, 1)
            ...
    """

    def __init__(self, config: dict | None = None) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_streaming_mastering_chain_create"):
            raise RuntimeError("libsonare was built without streaming mastering chain support")
        param_array, param_count = _chain_params(config)
        handle = lib.sonare_streaming_mastering_chain_create(
            param_array, ctypes.c_size_t(param_count)
        )
        if not handle:
            detail = ""
            if hasattr(lib, "sonare_last_error_message"):
                raw = lib.sonare_last_error_message()
                if raw:
                    detail = raw.decode("utf-8", errors="replace")
            message = "failed to create StreamingMasteringChain"
            if detail:
                message = f"{message}: {detail}"
            raise RuntimeError(message)
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self._prepared_channels = 0

    def prepare(self, sample_rate: int, max_block_size: int, num_channels: int) -> None:
        """Initialize processors for the given sample rate and block layout.

        Args:
            sample_rate: Sample rate in Hz.
            max_block_size: Maximum block size in samples per
                :meth:`process_mono` / :meth:`process_stereo` call.
            num_channels: 1 (mono) or 2 (stereo). Stereo-only stages
                (imager, monoMaker) are skipped when ``num_channels`` is 1.
        """
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_prepare(
            self._handle,
            ctypes.c_int(int(sample_rate)),
            ctypes.c_int(int(max_block_size)),
            ctypes.c_int(int(num_channels)),
        )
        _check(rc)
        self._prepared_channels = int(num_channels)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning the processed samples (length unchanged)."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        rc = self._lib.sonare_streaming_mastering_chain_process_mono(
            self._handle, c_array, ctypes.c_size_t(length)
        )
        _check(rc)
        return [float(c_array[i]) for i in range(length)]

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        rc = self._lib.sonare_streaming_mastering_chain_process_stereo(
            self._handle, left_array, right_array, ctypes.c_size_t(left_length)
        )
        _check(rc)
        return (
            [float(left_array[i]) for i in range(left_length)],
            [float(right_array[i]) for i in range(right_length)],
        )

    def reset(self) -> None:
        """Reset all processor state without rebuilding."""
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_reset(self._handle)
        _check(rc)

    @property
    def latency_samples(self) -> int:
        """Total reported latency in samples across all active processors."""
        if self._handle is None or not self._handle:
            return 0
        return int(self._lib.sonare_streaming_mastering_chain_latency_samples(self._handle))

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_streaming_mastering_chain_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingMasteringChain:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        # Defensive: __del__ must not raise
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        if self._handle is None or not self._handle:
            raise RuntimeError("StreamingMasteringChain is closed")


def mastering_pair_process(
    processor_name: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringResult:
    """Apply a named two-input mastering processor."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_pair_processor"):
        raise RuntimeError("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    if source_length != reference_length:
        raise ValueError("source and reference lengths must match")
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_apply_pair_processor(
        processor_name.encode("utf-8"),
        source_array,
        reference_array,
        ctypes.c_size_t(source_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def mastering_pair_analyze(
    analysis_name: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Run a named two-input mastering analysis and return shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_pair"):
        raise RuntimeError("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    if source_length != reference_length:
        raise ValueError("source and reference lengths must match")
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_void_p()
    rc = lib.sonare_mastering_analyze_pair(
        analysis_name.encode("utf-8"),
        source_array,
        reference_array,
        ctypes.c_size_t(source_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def mastering_stereo_analyze(
    analysis_name: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Run a named stereo mastering analysis and return shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_stereo"):
        raise RuntimeError("libsonare was built without mastering support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_void_p()
    rc = lib.sonare_mastering_analyze_stereo(
        analysis_name.encode("utf-8"),
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


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
    rc = lib.sonare_trim(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(threshold_db),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
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
    return float(
        lib.sonare_frames_to_time(ctypes.c_int(frames), ctypes.c_int(sr), ctypes.c_int(hop_length))
    )


def time_to_frames(time: float, sr: int = 22050, hop_length: int = 512) -> int:
    """Convert time in seconds to frame count."""
    lib = _get_lib()
    return int(
        lib.sonare_time_to_frames(ctypes.c_float(time), ctypes.c_int(sr), ctypes.c_int(hop_length))
    )


def frames_to_samples(frames: int, hop_length: int = 512, n_fft: int = 0) -> int:
    """Convert frame count to sample index."""
    lib = _get_lib()
    return int(
        lib.sonare_frames_to_samples(
            ctypes.c_int(frames), ctypes.c_int(hop_length), ctypes.c_int(n_fft)
        )
    )


def samples_to_frames(samples: int, hop_length: int = 512, n_fft: int = 0) -> int:
    """Convert sample index to frame count."""
    lib = _get_lib()
    return int(
        lib.sonare_samples_to_frames(
            ctypes.c_int(samples), ctypes.c_int(hop_length), ctypes.c_int(n_fft)
        )
    )


def _call_float_transform(
    fn_name: str, values: Sequence[float] | list[float], *args: object
) -> list[float]:
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = getattr(lib, fn_name)(
        c_array,
        ctypes.c_size_t(length),
        *args,
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def power_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-10,
    top_db: float = 80.0,
) -> list[float]:
    """Convert power values to dB."""
    return _call_float_transform(
        "sonare_power_to_db",
        values,
        ctypes.c_float(ref),
        ctypes.c_float(amin),
        ctypes.c_float(top_db),
    )


def amplitude_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-5,
    top_db: float = 80.0,
) -> list[float]:
    """Convert amplitude values to dB."""
    return _call_float_transform(
        "sonare_amplitude_to_db",
        values,
        ctypes.c_float(ref),
        ctypes.c_float(amin),
        ctypes.c_float(top_db),
    )


def db_to_power(values: Sequence[float] | list[float], ref: float = 1.0) -> list[float]:
    """Convert dB values back to power."""
    return _call_float_transform("sonare_db_to_power", values, ctypes.c_float(ref))


def db_to_amplitude(values: Sequence[float] | list[float], ref: float = 1.0) -> list[float]:
    """Convert dB values back to amplitude."""
    return _call_float_transform("sonare_db_to_amplitude", values, ctypes.c_float(ref))


def preemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> list[float]:
    """Apply librosa-compatible pre-emphasis."""
    return _call_float_transform(
        "sonare_preemphasis",
        samples,
        ctypes.c_float(coef),
        ctypes.c_float(0.0 if zi is None else zi),
        ctypes.c_int(0 if zi is None else 1),
    )


def deemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> list[float]:
    """Apply inverse pre-emphasis."""
    return _call_float_transform(
        "sonare_deemphasis",
        samples,
        ctypes.c_float(coef),
        ctypes.c_float(0.0 if zi is None else zi),
        ctypes.c_int(0 if zi is None else 1),
    )


def trim_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> tuple[list[float], int, int]:
    """Trim leading/trailing silence and return (audio, start_sample, end_sample)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    start = ctypes.c_int()
    end = ctypes.c_int()
    rc = lib.sonare_trim_silence(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_float(top_db),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(start),
        ctypes.byref(end),
    )
    _check(rc)
    try:
        return (_float_array_result(out, out_length.value), int(start.value), int(end.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def split_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> list[tuple[int, int]]:
    """Return non-silent intervals as (start_sample, end_sample)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_int)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_split_silence(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_float(top_db),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        flat = _int_array_result(out, out_count.value)
        return [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_ints(out)


def frame_signal(
    samples: Sequence[float] | list[float],
    frame_length: int,
    hop_length: int,
) -> tuple[int, list[float]]:
    """Slice a signal into frames. Returns (n_frames, row-major frames)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_frame_signal(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def pad_center(
    values: Sequence[float] | list[float],
    size: int,
    pad_value: float = 0.0,
) -> list[float]:
    """Pad an array by centering it within target size."""
    return _call_float_transform(
        "sonare_pad_center", values, ctypes.c_size_t(size), ctypes.c_float(pad_value)
    )


def fix_length(
    values: Sequence[float] | list[float],
    size: int,
    pad_value: float = 0.0,
) -> list[float]:
    """Crop or pad an array to exact length."""
    return _call_float_transform(
        "sonare_fix_length", values, ctypes.c_size_t(size), ctypes.c_float(pad_value)
    )


def fix_frames(
    frames: Sequence[int] | list[int],
    x_min: int = 0,
    x_max: int = -1,
    pad: bool = True,
) -> list[int]:
    """Adjust frame indices to fit within bounds."""
    lib = _get_lib()
    c_array, length = _to_c_int_array(frames)
    out = ctypes.POINTER(ctypes.c_int)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_fix_frames(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(x_min),
        ctypes.c_int(x_max),
        ctypes.c_int(1 if pad else 0),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _int_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_ints(out)


def peak_pick(
    values: Sequence[float] | list[float],
    pre_max: int,
    post_max: int,
    pre_avg: int,
    post_avg: int,
    delta: float,
    wait: int,
) -> list[int]:
    """Pick peaks using librosa.util.peak_pick-compatible parameters."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    out = ctypes.POINTER(ctypes.c_int)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_peak_pick(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(pre_max),
        ctypes.c_int(post_max),
        ctypes.c_int(pre_avg),
        ctypes.c_int(post_avg),
        ctypes.c_float(delta),
        ctypes.c_int(wait),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _int_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_ints(out)


def vector_normalize(
    values: Sequence[float] | list[float],
    norm_type: int = 0,
    threshold: float = 0.0,
) -> list[float]:
    """Normalize a vector. norm_type: 0=inf, 1=L1, 2=L2, 3=power."""
    return _call_float_transform(
        "sonare_vector_normalize",
        values,
        ctypes.c_int(norm_type),
        ctypes.c_float(threshold),
    )


def pcen(
    values: Sequence[float] | list[float],
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    hop_length: int = 512,
    time_constant: float = 0.4,
    gain: float = 0.98,
    bias: float = 2.0,
    power: float = 0.5,
    eps: float = 1e-6,
) -> list[float]:
    """Apply per-channel energy normalization to a row-major spectrogram."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    if length != n_bins * n_frames:
        raise ValueError("values length must equal n_bins * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_pcen(
        c_array,
        ctypes.c_int(n_bins),
        ctypes.c_int(n_frames),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(time_constant),
        ctypes.c_float(gain),
        ctypes.c_float(bias),
        ctypes.c_float(power),
        ctypes.c_float(eps),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def tonnetz(chromagram: Sequence[float] | list[float], n_chroma: int, n_frames: int) -> list[float]:
    """Compute Tonnetz from row-major chromagram data."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(chromagram)
    if length != n_chroma * n_frames:
        raise ValueError("chromagram length must equal n_chroma * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_tonnetz(
        c_array,
        ctypes.c_int(n_chroma),
        ctypes.c_int(n_frames),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def tempogram(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
) -> tuple[int, list[float]]:
    """Compute autocorrelation tempogram. Returns (n_frames, row-major matrix)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_tempogram(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_int(win_length),
        ctypes.c_int(1 if center else 0),
        ctypes.c_int(1 if norm else 0),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def plp(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    tempo_min: float = 30.0,
    tempo_max: float = 300.0,
    win_length: int = 384,
) -> list[float]:
    """Compute predominant local pulse from an onset envelope."""
    return _call_float_transform(
        "sonare_plp",
        onset_envelope,
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(tempo_min),
        ctypes.c_float(tempo_max),
        ctypes.c_int(win_length),
    )


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
    rc = lib.sonare_resample(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(src_sr),
        ctypes.c_int(target_sr),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result
