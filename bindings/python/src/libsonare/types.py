"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass
from enum import IntEnum
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray


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
    HOLD = 2
    S_CURVE = 3

    def __str__(self) -> str:
        names = {
            AutomationCurve.LINEAR: "linear",
            AutomationCurve.EXPONENTIAL: "exponential",
            AutomationCurve.HOLD: "hold",
            AutomationCurve.S_CURVE: "s-curve",
        }
        return names[self]


class PanLaw(IntEnum):
    """Pan law applied to a mixer strip's pan position."""

    CONST_3DB = 0
    CONST_4_5DB = 1
    CONST_6DB = 2
    LINEAR_0DB = 3


class ChannelLayout(IntEnum):
    """Speaker bed layout for a bus or source (mirrors SonareChannelLayout).

    Plane order is WAVE_FORMAT_EXTENSIBLE: ``FIVE_POINT_ONE`` = L R C LFE Ls Rs,
    ``SEVEN_POINT_ONE`` = L R C LFE Lss Rss Ls Rs.
    """

    MONO = 0
    STEREO = 1
    FIVE_POINT_ONE = 2
    SEVEN_POINT_ONE = 3


class MeterTap(IntEnum):
    """Tap point at which a strip meter snapshot is read."""

    PRE_FADER = 0
    POST_FADER = 1


class SendTiming(IntEnum):
    """Pre/post-fader timing of a mixer strip send."""

    PRE_FADER = 0
    POST_FADER = 1


class SectionType(IntEnum):
    """Song-structure section type (mirrors sonare::SectionType ordinals)."""

    INTRO = 0
    VERSE = 1
    PRE_CHORUS = 2
    CHORUS = 3
    BRIDGE = 4
    INSTRUMENTAL = 5
    OUTRO = 6
    UNKNOWN = 7


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
    NON_QUEUEABLE_COMMAND = 10
    AUTOMATION_BIND_TARGET_OVERFLOW = 11
    STALE_AUTOMATION_LANES = 12
    SMOOTHED_PARAMETER_CAPACITY = 13
    COMMAND_BACKLOG_DEFERRED = 14
    CLIP_PAGE_UNDERRUN = 15


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
class AnalysisDynamics:
    """Dynamics summary embedded in :class:`AnalysisResult`."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool

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


@dataclass(frozen=True, slots=True)
class AnalysisTimbre:
    """Timbre summary embedded in :class:`AnalysisResult`."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float


@dataclass(frozen=True, slots=True)
class AnalysisRhythm:
    """Rhythm summary embedded in :class:`AnalysisResult`."""

    time_signature: TimeSignature
    syncopation: float
    groove_type: str
    pattern_regularity: float
    tempo_stability: float

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


@dataclass(frozen=True, slots=True)
class AnalysisMelody:
    """Melody summary embedded in :class:`AnalysisResult`."""

    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    pitches: list[MelodyPoint]

    @property
    def pitchRangeOctaves(self) -> float:  # noqa: N802
        return self.pitch_range_octaves

    @property
    def pitchStability(self) -> float:  # noqa: N802
        return self.pitch_stability

    @property
    def meanFrequency(self) -> float:  # noqa: N802
        return self.mean_frequency

    @property
    def vibratoRate(self) -> float:  # noqa: N802
        return self.vibrato_rate


@dataclass(frozen=True, slots=True)
class AnalysisResult:
    """Full audio analysis result."""

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]
    # Extended fields (populated when sonare_analyze_json is available).
    beat_strengths: list[float] = dataclasses.field(default_factory=list)
    chords: list[Chord] = dataclasses.field(default_factory=list)
    sections: list[Section] = dataclasses.field(default_factory=list)
    timbre: AnalysisTimbre | None = None
    dynamics: AnalysisDynamics | None = None
    rhythm: AnalysisRhythm | None = None
    melody: AnalysisMelody | None = None
    form: str = ""

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
    def beatStrengths(self) -> list[float]:  # noqa: N802
        return self.beat_strengths

    @property
    def beats(self) -> list[Beat]:
        if self.beat_strengths:
            return [
                Beat(time=t, strength=s)
                for t, s in zip(self.beat_times, self.beat_strengths, strict=False)
            ]
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


@dataclass(frozen=True, slots=True)
class RirResult:
    """Room impulse response synthesized from shoebox geometry."""

    rir: list[float]
    sample_rate: int
    has_error: bool

    @property
    def sampleRate(self) -> int:  # noqa: N802
        return self.sample_rate

    @property
    def hasError(self) -> bool:  # noqa: N802
        return self.has_error


@dataclass(frozen=True, slots=True)
class RoomEstimate:
    """Blind equivalent-room estimate (volume/dimensions/absorption/DRR)."""

    volume: float
    length: float
    width: float
    height: float
    drr_db: float
    confidence: float
    absorption_bands: list[float]
    rt60_bands: list[float]

    @property
    def drrDb(self) -> float:  # noqa: N802
        return self.drr_db

    @property
    def absorptionBands(self) -> list[float]:  # noqa: N802
        return self.absorption_bands

    @property
    def rt60Bands(self) -> list[float]:  # noqa: N802
        return self.rt60_bands

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
class ClippingRegion:
    """One contiguous run of clipped samples reported by detect_clipping."""

    start_sample: int
    end_sample: int
    length: int
    peak: float


@dataclass(frozen=True, slots=True)
class ClippingReport:
    """Aggregated clipping detection result (mirrors SonareClippingResult)."""

    clipped_samples: int
    clipping_ratio: float
    max_clipped_peak: float
    regions: list[ClippingRegion]


@dataclass(frozen=True, slots=True)
class DynamicRangeReport:
    """Sliding-window dynamic range report (mirrors SonareDynamicRangeResult)."""

    dynamic_range_db: float
    low_percentile_db: float
    high_percentile_db: float
    window_rms_db: list[float]


@dataclass(frozen=True, slots=True)
class VectorscopeReport:
    """Mid/side vectorscope point series for a (left, right) stereo pair."""

    mid: NDArray[np.float32]
    side: NDArray[np.float32]


@dataclass(frozen=True, slots=True)
class PhaseScopeReport:
    """Phase-scope (Lissajous) point series plus summary stats."""

    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    radius: NDArray[np.float32]
    angle_rad: NDArray[np.float32]
    correlation: float
    average_abs_angle_rad: float
    max_radius: float


@dataclass(frozen=True, slots=True)
class SpectrumReport:
    """Single-frame magnitude / power / dB spectrum (mirrors SonareSpectrumResult)."""

    frequencies: NDArray[np.float32]
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]
    db: NDArray[np.float32]
    n_fft: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class WaveformPeaksReport:
    """Per-channel min/max waveform buckets. Arrays are channel-major."""

    min: NDArray[np.float32]
    max: NDArray[np.float32]
    channels: int
    bucket_count: int
    samples_per_bucket: int


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
class TimbreFrame:
    """Timbre metrics for one analysis window in TimbreResult.timbre_over_time."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float


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
    timbre_over_time: list[TimbreFrame]

    @property
    def spectralCentroid(self) -> list[float]:  # noqa: N802
        return self.spectral_centroid

    @property
    def spectralFlatness(self) -> list[float]:  # noqa: N802
        return self.spectral_flatness

    @property
    def spectralRolloff(self) -> list[float]:  # noqa: N802
        return self.spectral_rolloff

    @property
    def timbreOverTime(self) -> list[TimbreFrame]:  # noqa: N802
        return self.timbre_over_time


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
    default_curve: AutomationCurve


@dataclass(frozen=True, slots=True)
class AutomationPoint:
    """PPQ automation breakpoint."""

    ppq: float
    value: float
    curve_to_next: AutomationCurve = AutomationCurve.LINEAR


class MarkerKind(IntEnum):
    """Timeline marker kind. Mirrors SonareMarkerKind in the C ABI and the
    other bindings' marker-kind enums; the values are part of the ABI."""

    MARKER = 0
    TEXT = 1
    LYRIC = 2
    CUE_POINT = 3
    KEY_SIGNATURE = 4


@dataclass(frozen=True, slots=True)
class EngineMarker:
    """Timeline marker used by the realtime engine transport.

    ``kind`` is a :class:`MarkerKind` ordinal; ``key_fifths`` (-7..7, sharps
    positive) and ``key_minor`` apply only to the key-signature kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: int = 0
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class ProjectMarker:
    """Timeline marker stored on a headless :class:`~libsonare._project.Project`.

    Same shape as :class:`EngineMarker`: ``kind`` is a :class:`MarkerKind`
    ordinal and ``key_fifths`` / ``key_minor`` apply only to the key-signature
    kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: int = 0
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class EngineMetronomeConfig:
    """Realtime engine metronome click configuration."""

    enabled: bool = False
    beat_gain: float = 0.35
    accent_gain: float = 0.7
    click_samples: int = 96
    # Click duration in seconds; used when click_samples is 0 to derive the click
    # length from the prepared sample rate. 0.0 selects the engine default (2 ms).
    click_seconds: float = 0.0


@dataclass(frozen=True, slots=True)
class EngineClip:
    """Owned audio clip schedule for realtime engine playback."""

    id: int
    channels: list[list[float]] | None
    start_ppq: float
    track_id: int = 0
    length_samples: int | None = None
    clip_offset_samples: int = 0
    loop: bool = False
    gain: float = 1.0
    fade_in_samples: int = 0
    fade_out_samples: int = 0
    warp_mode: str | int = "off"
    warp_anchors: list[tuple[float, float]] | None = None
    page_provider: object | None = None


@dataclass(frozen=True, slots=True)
class EngineMidiEvent:
    """Absolute render-frame MIDI event for realtime engine MIDI clips."""

    render_frame: int
    word0: int = 0
    word1: int = 0
    word2: int = 0
    word3: int = 0
    word_count: int = 0
    group: int = 0
    sysex_handle: int = 0


@dataclass(frozen=True, slots=True)
class EngineMidiClipSchedule:
    """Compiled realtime MIDI clip schedule."""

    events: list[EngineMidiEvent]
    id: int = 0
    track_id: int = 0
    destination_id: int = 0
    start_sample: int = 0
    start_ppq: float = 0.0
    length_samples: int = 0
    loop: bool = False
    loop_length_samples: int = 0


@dataclass(frozen=True, slots=True)
class ClipPageRequest:
    """Paged clip sample request drained from the realtime engine."""

    clip_id: int
    channel: int
    sample: int


@dataclass(frozen=True, slots=True)
class EngineCaptureStatus:
    """Capture progress for the realtime engine recording sink."""

    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool
    source: str
    record_offset_samples: int


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


@dataclass(frozen=True, slots=True)
class MeterTelemetryRecord:
    """A meter snapshot drained from the realtime engine's meter tap."""

    target_id: int
    render_frame: int
    seq: int
    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int


@dataclass(frozen=True, slots=True)
class ScopeTelemetryRecord:
    """A spectrum/vectorscope snapshot drained from the realtime engine."""

    target_id: int
    render_frame: int
    seq: int
    dropped_records: int
    bands: list[float]
    points: list[tuple[float, float]]


@dataclass(frozen=True, slots=True)
class TransportState:
    """Read-only snapshot of the realtime engine transport state."""

    playing: bool
    looping: bool
    render_frame: int
    sample_position: int
    ppq_position: float
    bpm: float
    loop_start_ppq: float
    loop_end_ppq: float
    sample_rate: float
    # Musical position derived from the tempo map (computed every block).
    bar_start_ppq: float
    bar_count: int
    time_signature: TimeSignature


@dataclass(frozen=True, slots=True)
class Section:
    """A detected song-structure section."""

    type: SectionType
    start: float
    end: float
    energy_level: float
    confidence: float

    @property
    def name(self) -> str:
        names = {
            SectionType.INTRO: "Intro",
            SectionType.VERSE: "Verse",
            SectionType.PRE_CHORUS: "PreChorus",
            SectionType.CHORUS: "Chorus",
            SectionType.BRIDGE: "Bridge",
            SectionType.INSTRUMENTAL: "Instrumental",
            SectionType.OUTRO: "Outro",
            SectionType.UNKNOWN: "Unknown",
        }
        return names[self.type]


@dataclass(frozen=True, slots=True)
class SectionResult:
    """Song-structure analysis result."""

    sections: list[Section]


@dataclass(frozen=True, slots=True)
class MelodyPoint:
    """A single point on a melody contour."""

    time: float
    frequency: float
    confidence: float


@dataclass(frozen=True, slots=True)
class MelodyResult:
    """Melody contour analysis result."""

    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float


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
    key_update_interval_sec: float = 5.0
    bpm_update_interval_sec: float = 10.0
    window: int = 0
    output_format: int = 0


@dataclass(frozen=True, slots=True)
class StreamFrames:
    """Structure-of-arrays batch of analyzed frames from :class:`StreamAnalyzer`.

    Matrix fields are flattened row-major ``[n_frames x stride]`` lists
    (``mel`` stride is ``n_mels``, ``chroma`` stride is 12).
    """

    n_frames: int
    n_mels: int
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
    n_frames: int
    n_mels: int
    timestamps: list[float]
    mel: list[int]
    chroma: list[int]
    onset_strength: list[int]
    rms_energy: list[int]
    spectral_centroid: list[int]
    spectral_flatness: list[int]


@dataclass(frozen=True, slots=True)
class StreamFramesI16:
    n_frames: int
    n_mels: int
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
