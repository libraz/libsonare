"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum


class PitchClass(IntEnum):
    """Musical pitch class (chromatic scale)."""

    C = 0
    CS = 1
    D = 2
    DS = 3
    E = 4
    F = 5
    FS = 6
    G = 7
    GS = 8
    A = 9
    AS = 10
    B = 11

    def __str__(self) -> str:
        _names = {
            0: "C",
            1: "C#",
            2: "D",
            3: "D#",
            4: "E",
            5: "F",
            6: "F#",
            7: "G",
            8: "G#",
            9: "A",
            10: "A#",
            11: "B",
        }
        return _names[self.value]


class Mode(IntEnum):
    """Musical mode."""

    MAJOR = 0
    MINOR = 1
    DORIAN = 2
    PHRYGIAN = 3
    LYDIAN = 4
    MIXOLYDIAN = 5
    LOCRIAN = 6

    def __str__(self) -> str:
        names = {
            Mode.MAJOR: "major",
            Mode.MINOR: "minor",
            Mode.DORIAN: "dorian",
            Mode.PHRYGIAN: "phrygian",
            Mode.LYDIAN: "lydian",
            Mode.MIXOLYDIAN: "mixolydian",
            Mode.LOCRIAN: "locrian",
        }
        return names[self]


class AutomationCurve(IntEnum):
    """Interpolation curve for scheduled mixer automation events."""

    LINEAR = 0
    EXPONENTIAL = 1

    def __str__(self) -> str:
        return "exponential" if self is AutomationCurve.EXPONENTIAL else "linear"


class PanLaw(IntEnum):
    """Pan law applied to a mixer strip's pan position."""

    CONST_3DB = 0
    CONST_4_5DB = 1
    CONST_6DB = 2
    LINEAR_0DB = 3


class MeterTap(IntEnum):
    """Tap point at which a strip meter snapshot is read."""

    PRE_FADER = 0
    POST_FADER = 1


class EngineTelemetryType(IntEnum):
    """Realtime engine telemetry record type."""

    PROCESS_BLOCK = 0
    ERROR = 1


class EngineTelemetryError(IntEnum):
    """Recoverable realtime engine error codes."""

    NONE = 0
    COMMAND_QUEUE_OVERFLOW = 1
    PENDING_COMMAND_OVERFLOW = 2
    BOUNDARY_OVERFLOW = 3
    TELEMETRY_OVERFLOW = 4
    CAPTURE_OVERFLOW = 5
    MAX_BLOCK_EXCEEDED = 6
    UNKNOWN_TARGET = 7
    NON_REALTIME_SAFE_PARAMETER = 8
    NOT_PREPARED = 9


class AutomationPointCurve(IntEnum):
    """Breakpoint curve type used by engine automation lanes."""

    HOLD = 0
    LINEAR = 1
    EXPONENTIAL = 2
    S_CURVE = 3


class KeyProfile(IntEnum):
    """Key-profile family used by profile-correlation key detection."""

    KRUMHANSL_SCHMUCKLER = 0
    TEMPERLEY = 1
    SHAATH = 2
    FARALDO_EDMT = 3
    FARALDO_EDMA = 4
    FARALDO_EDMM = 5
    BELLMAN_BUDGE = 6


@dataclass(frozen=True, slots=True)
class Key:
    """Detected musical key."""

    root: PitchClass
    mode: Mode
    confidence: float

    @property
    def name(self) -> str:
        return f"{self.root} {self.mode}"

    @property
    def short_name(self) -> str:
        if self.mode == Mode.MAJOR:
            return f"{self.root}"
        if self.mode == Mode.MINOR:
            return f"{self.root}m"
        return f"{self.root} {self.mode}"

    @property
    def shortName(self) -> str:  # noqa: N802
        return self.short_name

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True, slots=True)
class KeyCandidate:
    """Key candidate with raw profile correlation."""

    key: Key
    correlation: float


@dataclass(frozen=True, slots=True)
class TimeSignature:
    """Detected time signature."""

    numerator: int
    denominator: int
    confidence: float

    def __str__(self) -> str:
        return f"{self.numerator}/{self.denominator}"


@dataclass(frozen=True, slots=True)
class Beat:
    """Beat event."""

    time: float
    strength: float | None = None


@dataclass(frozen=True, slots=True)
class AnalysisResult:
    """Full audio analysis result."""

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]

    @property
    def bpmConfidence(self) -> float:  # noqa: N802
        return self.bpm_confidence

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def beatTimes(self) -> list[float]:  # noqa: N802
        return self.beat_times

    @property
    def beats(self) -> list[Beat]:
        return [Beat(time=t) for t in self.beat_times]


@dataclass(frozen=True, slots=True)
class BpmCandidate:
    """BPM candidate with confidence."""

    bpm: float
    confidence: float


@dataclass(frozen=True, slots=True)
class BpmAnalysisResult:
    """Detailed BPM analysis result."""

    bpm: float
    confidence: float
    candidates: list[BpmCandidate]
    autocorrelation: list[float]
    tempogram: list[float]


@dataclass(frozen=True, slots=True)
class AcousticResult:
    """Room acoustic parameters from an impulse response."""

    rt60: float
    edt: float
    c50: float
    c80: float
    d50: float
    rt60_bands: list[float]
    edt_bands: list[float]
    c50_bands: list[float]
    c80_bands: list[float]
    confidence: float
    is_blind: bool

    @property
    def rt60Bands(self) -> list[float]:  # noqa: N802
        return self.rt60_bands

    @property
    def edtBands(self) -> list[float]:  # noqa: N802
        return self.edt_bands

    @property
    def c50Bands(self) -> list[float]:  # noqa: N802
        return self.c50_bands

    @property
    def c80Bands(self) -> list[float]:  # noqa: N802
        return self.c80_bands

    @property
    def isBlind(self) -> bool:  # noqa: N802
        return self.is_blind


@dataclass(frozen=True, slots=True)
class LufsResult:
    """ITU-R BS.1770 / EBU R128 loudness metrics (offline meter)."""

    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    loudness_range: float

    @property
    def integratedLufs(self) -> float:  # noqa: N802
        return self.integrated_lufs

    @property
    def momentaryLufs(self) -> float:  # noqa: N802
        return self.momentary_lufs

    @property
    def shortTermLufs(self) -> float:  # noqa: N802
        return self.short_term_lufs

    @property
    def loudnessRange(self) -> float:  # noqa: N802
        return self.loudness_range


@dataclass(frozen=True, slots=True)
class EqSpectrumSnapshot:
    """Realtime equalizer spectrum snapshot."""

    pre_left: list[float]
    pre_right: list[float]
    post_left: list[float]
    post_right: list[float]
    band_gain_db: list[float]
    profile_db: list[float]
    last_auto_gain_db: float
    seq: int

    @property
    def preLeft(self) -> list[float]:  # noqa: N802
        return self.pre_left

    @property
    def preRight(self) -> list[float]:  # noqa: N802
        return self.pre_right

    @property
    def postLeft(self) -> list[float]:  # noqa: N802
        return self.post_left

    @property
    def postRight(self) -> list[float]:  # noqa: N802
        return self.post_right

    @property
    def bandGainDb(self) -> list[float]:  # noqa: N802
        return self.band_gain_db

    @property
    def profileDb(self) -> list[float]:  # noqa: N802
        return self.profile_db

    @property
    def lastAutoGainDb(self) -> float:  # noqa: N802
        return self.last_auto_gain_db


@dataclass(frozen=True, slots=True)
class RhythmResult:
    """Rhythm analysis primitives."""

    bpm: float
    time_signature: TimeSignature
    groove_type: str
    syncopation: float
    pattern_regularity: float
    tempo_stability: float
    beat_intervals: list[float]

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def grooveType(self) -> str:  # noqa: N802
        return self.groove_type

    @property
    def patternRegularity(self) -> float:  # noqa: N802
        return self.pattern_regularity

    @property
    def tempoStability(self) -> float:  # noqa: N802
        return self.tempo_stability

    @property
    def beatIntervals(self) -> list[float]:  # noqa: N802
        return self.beat_intervals


@dataclass(frozen=True, slots=True)
class DynamicsResult:
    """Dynamics and loudness analysis primitives."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool
    loudness_times: list[float]
    loudness_rms_db: list[float]

    @property
    def dynamicRangeDb(self) -> float:  # noqa: N802
        return self.dynamic_range_db

    @property
    def peakDb(self) -> float:  # noqa: N802
        return self.peak_db

    @property
    def rmsDb(self) -> float:  # noqa: N802
        return self.rms_db

    @property
    def crestFactor(self) -> float:  # noqa: N802
        return self.crest_factor

    @property
    def loudnessRangeDb(self) -> float:  # noqa: N802
        return self.loudness_range_db

    @property
    def isCompressed(self) -> bool:  # noqa: N802
        return self.is_compressed

    @property
    def loudnessTimes(self) -> list[float]:  # noqa: N802
        return self.loudness_times

    @property
    def loudnessRmsDb(self) -> list[float]:  # noqa: N802
        return self.loudness_rms_db


@dataclass(frozen=True, slots=True)
class TimbreResult:
    """Timbre and spectral-shape analysis primitives."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    spectral_rolloff: list[float]

    @property
    def spectralCentroid(self) -> list[float]:  # noqa: N802
        return self.spectral_centroid

    @property
    def spectralFlatness(self) -> list[float]:  # noqa: N802
        return self.spectral_flatness

    @property
    def spectralRolloff(self) -> list[float]:  # noqa: N802
        return self.spectral_rolloff


@dataclass(frozen=True, slots=True)
class Chord:
    """Detected chord with timing and confidence."""

    root: PitchClass
    quality: str
    start: float
    end: float
    confidence: float
    bass: PitchClass | None = None

    @property
    def duration(self) -> float:
        return self.end - self.start

    @property
    def name(self) -> str:
        suffixes = {
            "major": "maj",
            "minor": "m",
            "diminished": "dim",
            "augmented": "aug",
            "dominant7": "7",
            "major7": "maj7",
            "minor7": "m7",
            "sus2": "sus2",
            "sus4": "sus4",
            "unknown": "",
            "add9": "add9",
            "minorAdd9": "madd9",
            "dim7": "dim7",
            "halfDim7": "m7b5",
            "major9": "maj9",
            "dominant9": "9",
            "sus2Add4": "sus2add4",
        }
        bass = self.root if self.bass is None else self.bass
        slash = "" if bass == self.root else f"/{bass}"
        return f"{self.root}{suffixes.get(self.quality, '')}{slash}"


@dataclass(frozen=True, slots=True)
class ChordAnalysisResult:
    """Chord detection primitives."""

    chords: list[Chord]


@dataclass(frozen=True, slots=True)
class StftResult:
    """Short-time Fourier transform result."""

    n_bins: int
    n_frames: int
    n_fft: int
    hop_length: int
    sample_rate: int
    magnitude: list[float]
    power: list[float]


@dataclass(frozen=True, slots=True)
class MelSpectrogramResult:
    """Mel spectrogram result."""

    n_mels: int
    n_frames: int
    sample_rate: int
    hop_length: int
    power: list[float]
    db: list[float]


@dataclass(frozen=True, slots=True)
class MfccResult:
    """MFCC (Mel-frequency cepstral coefficients) result."""

    n_mfcc: int
    n_frames: int
    coefficients: list[float]


@dataclass(frozen=True, slots=True)
class ChromaResult:
    """Chroma feature result."""

    n_chroma: int
    n_frames: int
    sample_rate: int
    hop_length: int
    features: list[float]
    mean_energy: list[float]


@dataclass(frozen=True, slots=True)
class PitchResult:
    """Pitch detection result."""

    n_frames: int
    f0: list[float]
    voiced_prob: list[float]
    voiced_flag: list[bool]
    median_f0: float
    mean_f0: float


@dataclass(frozen=True, slots=True)
class HpssResult:
    """Harmonic-percussive source separation result."""

    harmonic: list[float]
    percussive: list[float]
    length: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class MasteringResult:
    """Mastering loudness/true-peak processing result."""

    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int = 0


@dataclass(frozen=True, slots=True)
class MasteringStereoResult:
    """Stereo mastering processing result."""

    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int = 0


@dataclass(frozen=True, slots=True)
class MasteringChainResult:
    """Result of running a configurable mastering chain on mono audio."""

    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]


@dataclass(frozen=True, slots=True)
class MasteringChainStereoResult:
    """Result of running a configurable mastering chain on stereo audio."""

    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]


@dataclass(frozen=True, slots=True)
class MixMeterSnapshot:
    """Realtime mixer meter snapshot for one strip."""

    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    correlation: float
    mono_compat_width: float
    mono_compat_peak: float
    mono_compat_side_rms: float
    likely_mono_compatible: bool
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
    seq: int


@dataclass(frozen=True, slots=True)
class GoniometerPoint:
    """A single left/right sample pair for goniometer (vectorscope) display."""

    left: float
    right: float


@dataclass(frozen=True, slots=True)
class MixResult:
    """Result of rendering a small stereo mixer scene."""

    left: list[float]
    right: list[float]
    sample_rate: int
    meters: list[MixMeterSnapshot]


@dataclass(frozen=True, slots=True)
class EngineTelemetry:
    """Realtime engine telemetry event."""

    type: EngineTelemetryType
    error: EngineTelemetryError
    render_frame: int
    timeline_sample: int
    audible_timeline_sample: int
    graph_latency_samples_q8: int
    value: int

    @property
    def renderFrame(self) -> int:  # noqa: N802
        return self.render_frame

    @property
    def timelineSample(self) -> int:  # noqa: N802
        return self.timeline_sample

    @property
    def audibleTimelineSample(self) -> int:  # noqa: N802
        return self.audible_timeline_sample

    @property
    def graphLatencySamplesQ8(self) -> int:  # noqa: N802
        return self.graph_latency_samples_q8


@dataclass(frozen=True, slots=True)
class ParameterInfo:
    """DAW parameter metadata for automation/introspection UIs."""

    id: int
    name: str
    unit: str
    min_value: float
    max_value: float
    default_value: float
    rt_safe: bool
    default_curve: AutomationPointCurve


@dataclass(frozen=True, slots=True)
class AutomationPoint:
    """PPQ automation breakpoint."""

    ppq: float
    value: float
    curve_to_next: AutomationPointCurve = AutomationPointCurve.LINEAR


@dataclass(frozen=True, slots=True)
class EngineMarker:
    """Timeline marker used by the realtime engine transport."""

    id: int
    ppq: float
    name: str = ""


@dataclass(frozen=True, slots=True)
class EngineMetronomeConfig:
    """Realtime engine metronome click configuration."""

    enabled: bool = False
    beat_gain: float = 0.35
    accent_gain: float = 0.7
    click_samples: int = 96


@dataclass(frozen=True, slots=True)
class EngineClip:
    """Owned audio clip schedule for realtime engine playback."""

    id: int
    channels: list[list[float]]
    start_ppq: float
    length_samples: int | None = None
    clip_offset_samples: int = 0
    loop: bool = False
    gain: float = 1.0
    fade_in_samples: int = 0
    fade_out_samples: int = 0


@dataclass(frozen=True, slots=True)
class EngineCaptureStatus:
    """Capture progress for the realtime engine recording sink."""

    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool


@dataclass(frozen=True, slots=True)
class EngineBounceOptions:
    """Offline export options for the realtime engine."""

    total_frames: int
    block_size: int = 128
    num_channels: int = 2
    target_sample_rate: int = 48000
    source_sample_rate: int = 48000
    normalize_lufs: bool = False
    target_lufs: float = -14.0
    dither: int = 0
    dither_bits: int = 16
    dither_seed: int = 0


@dataclass(frozen=True, slots=True)
class EngineBounceResult:
    """Interleaved offline export result from the realtime engine."""

    interleaved: list[float]
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float


@dataclass(frozen=True, slots=True)
class EngineFreezeOptions:
    """Offline freeze options for replacing current engine output with a clip."""

    total_frames: int
    block_size: int = 128
    num_channels: int = 2
    clip_id: int = 1
    start_ppq: float = 0.0
    gain: float = 1.0


@dataclass(frozen=True, slots=True)
class EngineFreezeResult:
    """Result of freezing current engine output into a scheduled clip."""

    clip_id: int
    frames: int
    num_channels: int


class EngineGraphNodeType(IntEnum):
    """Builtin processor node type for realtime engine graphs."""

    PASS_THROUGH = 0
    GAIN = 1


class EngineGraphMix(IntEnum):
    """Connection mix mode for realtime engine graphs."""

    REPLACE = 0
    ADD = 1


@dataclass(frozen=True, slots=True)
class EngineGraphNode:
    """Prepared realtime engine graph node."""

    id: str
    type: EngineGraphNodeType = EngineGraphNodeType.PASS_THROUGH
    gain_db: float = 0.0
    num_ports: int = 0


@dataclass(frozen=True, slots=True)
class EngineGraphConnection:
    """Prepared realtime engine graph connection."""

    source_node: str
    source_port: int
    dest_node: str
    dest_port: int
    mix: EngineGraphMix = EngineGraphMix.ADD


@dataclass(frozen=True, slots=True)
class EngineGraphParameterBinding:
    """Map an engine automation parameter id to a graph node processor."""

    param_id: int
    node_id: str


@dataclass(frozen=True, slots=True)
class EngineGraphSpec:
    """Prepared realtime engine graph specification."""

    nodes: list[EngineGraphNode]
    connections: list[EngineGraphConnection]
    input_node: str
    output_node: str
    num_channels: int = 2
    parameter_bindings: list[EngineGraphParameterBinding] | None = None
