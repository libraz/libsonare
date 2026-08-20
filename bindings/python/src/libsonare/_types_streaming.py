"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class CqtResult:
    """Constant-Q / Variable-Q transform magnitude result."""

    n_bins: int
    n_frames: int
    hop_length: int
    sample_rate: int
    magnitude: list[float]
    frequencies: list[float]


@dataclass(frozen=True, slots=True)
class InverseResult:
    """Inverse spectrogram reconstruction result.

    ``data`` is a row-major ``[rows x n_frames]`` matrix (``rows`` are the
    reconstructed frequency/Mel bins).
    """

    rows: int
    n_frames: int
    data: list[float]


@dataclass(frozen=True, slots=True)
class QuantizeConfig:
    """Quantization ranges for the u8/i16 bandwidth-reduction read paths.

    Defaults mirror ``sonare_stream_quantize_config_default``. Widen any range
    whose source values exceed the defaults: the quantizers clamp normalized
    values to ``[0, 1]``, so a stream louder or quieter than these ranges
    otherwise saturates silently to the endpoints.
    """

    mel_db_min: float = -80.0
    mel_db_max: float = 0.0
    onset_max: float = 50.0
    rms_max: float = 1.0
    centroid_max: float = 11025.0


@dataclass(frozen=True, slots=True)
class StreamConfig:
    """Construction config for :class:`StreamAnalyzer`.

    Defaults mirror the C ``sonare_stream_analyzer_config_default`` values
    (real-time 44100 Hz / n_fft 2048).
    """

    sample_rate: int = 44100
    n_fft: int = 2048
    hop_length: int = 512
    n_mels: int = 128
    fmin: float = 0.0
    fmax: float = 0.0
    # A4 tuning reference in Hz. Must be within 220..880, the same range
    # StreamAnalyzer.set_tuning_ref_hz accepts live.
    tuning_ref_hz: float = 440.0
    # The streaming C ABI has no magnitude read path, so magnitude is off by
    # default and an explicit True is rejected by the native layer.
    compute_magnitude: bool = False
    compute_mel: bool = True
    compute_chroma: bool = True
    compute_onset: bool = True
    compute_spectral: bool = True
    emit_every_n_frames: int = 1
    magnitude_downsample: int = 1
    max_pending_frames: int = 4096  # Overflow drops the newly produced frame.
    max_progression_entries: int = 4096
    key_update_interval_sec: float = 5.0
    bpm_update_interval_sec: float = 10.0
    window: int = 0
    output_format: int = 0  # Deprecated: must be 0; choose an explicit read method.


@dataclass(frozen=True, slots=True)
class StreamFrames:
    """Structure-of-arrays batch of analyzed frames from :class:`StreamAnalyzer`.

    Matrix fields are flattened row-major ``[n_frames x stride]`` lists
    (``mel`` stride is ``n_mels``, ``chroma`` stride is ``n_chroma``). A
    disabled feature has a zero stride or empty scalar array; ``feature_flags``
    uses MEL=1, CHROMA=2, ONSET=4, SPECTRAL=8.

    ``mel`` here is LINEAR mel power (not dB) — the raw per-frame mel energies.
    The quantized read paths (:class:`StreamFramesU8` / :class:`StreamFramesI16`)
    convert to dB before packing, so their ``mel`` is dB-scaled; this float
    buffer is not.
    """

    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps: list[float]
    mel: list[float]
    chroma: list[float]
    onset_strength: list[float]
    rms_energy: list[float]
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    chord_root: list[int]
    chord_quality: list[int]
    chord_confidence: list[float]


@dataclass(frozen=True, slots=True)
class StreamFramesU8:
    """Quantized (uint8) frame batch. ``mel`` is in dB, quantized over
    ``[mel_db_min, mel_db_max]`` (unlike :class:`StreamFrames`, whose float
    ``mel`` is linear power)."""

    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps: list[float]
    mel: list[int]
    chroma: list[int]
    onset_strength: list[int]
    rms_energy: list[int]
    spectral_centroid: list[int]
    spectral_flatness: list[int]


@dataclass(frozen=True, slots=True)
class StreamFramesI16:
    """Quantized (int16) frame batch. ``mel`` is in dB, quantized over
    ``[mel_db_min, mel_db_max]`` (unlike :class:`StreamFrames`, whose float
    ``mel`` is linear power)."""

    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps: list[float]
    mel: list[int]
    chroma: list[int]
    onset_strength: list[int]
    rms_energy: list[int]
    spectral_centroid: list[int]
    spectral_flatness: list[int]


@dataclass(frozen=True, slots=True)
class StreamChordChange:
    root: int
    quality: int
    start_time: float
    confidence: float


@dataclass(frozen=True, slots=True)
class StreamBarChord:
    bar_index: int
    root: int
    quality: int
    start_time: float
    confidence: float


@dataclass(frozen=True, slots=True)
class StreamPatternScore:
    name: str
    score: float


@dataclass(frozen=True, slots=True)
class StreamStats:
    """Progressive estimate and counters snapshot from :class:`StreamAnalyzer`."""

    total_frames: int
    total_samples: int
    duration_seconds: float
    pending_frames: int
    dropped_output_frames: int
    dropped_chord_progression_entries: int
    dropped_bar_progression_entries: int
    bpm: float
    bpm_confidence: float
    bpm_candidate_count: int
    key: int
    key_minor: bool
    key_confidence: float
    chord_root: int
    chord_quality: int
    chord_confidence: float
    chord_start_time: float
    current_bar: int
    bar_duration: float
    chord_progression: list[StreamChordChange]
    bar_chord_progression: list[StreamBarChord]
    voted_pattern: list[StreamBarChord]
    pattern_length: int
    detected_pattern_name: str
    detected_pattern_score: float
    all_pattern_scores: list[StreamPatternScore]
    accumulated_seconds: float
    used_frames: int
    updated: bool
