from __future__ import annotations

import builtins
from enum import IntEnum
from typing import Literal, TypedDict

import numpy as np
from numpy.typing import NDArray

MasteringProcessorKind = Literal["realtime", "offline", "pair"]
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
MasteringProcessorCategory = Literal[
    "dynamics",
    "effects",
    "eq",
    "final",
    "maximizer",
    "multiband",
    "other",
    "reference",
    "repair",
    "saturation",
    "spectral",
    "stereo",
]

class CapabilitiesAbi(TypedDict):
    project: int
    engine: int

class CapabilitiesFeatures(TypedDict):
    mastering: bool
    mixing: bool
    # Key stays camelCase: capabilities() returns the C ABI JSON verbatim.
    mixingAssistant: bool
    fx: bool
    ffmpeg: bool
    # Key stays camelCase: capabilities() returns the C ABI JSON verbatim.
    instrumentParamAutomation: bool

class CapabilitiesDecode(TypedDict):
    builtin: list[str]
    ffmpeg: list[str]

class Capabilities(TypedDict):
    version: str
    abi: CapabilitiesAbi
    platform: str
    features: CapabilitiesFeatures
    decode: CapabilitiesDecode
    simd: str
    hardwareConcurrency: int

class CapabilityCatalogPresets(TypedDict):
    mastering: list[str]
    synth: list[str]
    mixingScene: list[str]
    voiceChanger: list[str]

class MasteringInsertParamInfo(TypedDict):
    name: str
    id: int
    rtSafe: bool
    type: Literal["boolean", "number"]
    min: float | None
    max: float | None
    default: float | bool | None
    unit: str | None

class MasteringProcessorCatalogEntry(TypedDict):
    id: str
    kind: MasteringProcessorKind
    realtimeInsertable: bool
    stereoOnly: bool
    latencySamples: int
    tailSamples: int
    realtimeCost: Literal["low", "moderate", "high"] | None
    channelPolicy: MasteringChannelPolicy
    category: MasteringProcessorCategory
    params: list[MasteringInsertParamInfo]

class CapabilityCatalog(TypedDict):
    version: str
    abi: CapabilitiesAbi
    processors: list[MasteringProcessorCatalogEntry]
    presets: CapabilityCatalogPresets

class PitchClass(IntEnum):
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

class Mode(IntEnum):
    MAJOR = 0
    MINOR = 1
    DORIAN = 2
    PHRYGIAN = 3
    LYDIAN = 4
    MIXOLYDIAN = 5
    LOCRIAN = 6

class KeyProfile(IntEnum):
    KRUMHANSL_SCHMUCKLER = 0
    TEMPERLEY = 1
    SHAATH = 2
    FARALDO_EDMT = 3
    FARALDO_EDMA = 4
    FARALDO_EDMM = 5
    BELLMAN_BUDGE = 6

class AutomationCurve(IntEnum):
    LINEAR = 0
    EXPONENTIAL = 1
    HOLD = 2
    S_CURVE = 3

class PanLaw(IntEnum):
    """Mono centre-gain law; stereo Balance keeps centre unity and tapers only the far channel."""

    CONST_3DB = 0
    CONST_4_5DB = 1
    CONST_6DB = 2
    LINEAR_0DB = 3

class ChannelLayout(IntEnum):
    MONO = 0
    STEREO = 1
    FIVE_POINT_ONE = 2
    SEVEN_POINT_ONE = 3

class MeterTap(IntEnum):
    PRE_FADER = 0
    POST_FADER = 1

class SendTiming(IntEnum):
    PRE_FADER = 0
    POST_FADER = 1

class SectionType(IntEnum):
    """``PRE_CHORUS`` is never produced by the analyzer; every other value is
    reachable. ``UNKNOWN`` marks a segment the analyzer did not identify and
    carries ``confidence`` 0."""

    INTRO = 0
    VERSE = 1
    PRE_CHORUS = 2
    CHORUS = 3
    BRIDGE = 4
    INSTRUMENTAL = 5
    OUTRO = 6
    UNKNOWN = 7

class EngineTelemetryType(IntEnum):
    PROCESS_BLOCK = 0
    ERROR = 1

class EngineTelemetryError(IntEnum):
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
    INSERT_AUTOMATION_OVERFLOW = 16
    MIDI_CLOCK_OVERFLOW = 17
    METRONOME_OVERFLOW = 18
    # Ordinal 19 is reserved by the WASM worklet protocol.
    MAX_CHANNELS_EXCEEDED = 20

class Key:
    root: PitchClass
    mode: Mode
    confidence: float
    def __init__(self, root: PitchClass, mode: Mode, confidence: float) -> None: ...
    @property
    def name(self) -> str: ...
    @property
    def short_name(self) -> str: ...
    @property
    def shortName(self) -> str: ...

class KeyCandidate:
    key: Key
    correlation: float
    def __init__(self, key: Key, correlation: float) -> None: ...

class TimeSignature:
    numerator: int
    denominator: int
    confidence: float
    def __init__(self, numerator: int, denominator: int, confidence: float) -> None: ...

class Beat:
    """Beat event. ``strength`` is a single raw frame of the onset envelope
    sampled at the beat's own frame — not normalized, scaled by the material,
    and sensitive to beat-position jitter, so it is not a salience comparable
    across beats. Score accents with
    ``AnalysisResult.beat_observations.onset_strength``, the windowed value the
    library's own downbeat pass uses."""

    time: float
    strength: float | None
    def __init__(self, time: float, strength: float | None = None) -> None: ...

class BpmHypothesis:
    value: float
    confidence: float
    relation: Literal["primary", "half", "double", "other"]
    def __init__(
        self,
        value: float,
        confidence: float,
        relation: Literal["primary", "half", "double", "other"],
    ) -> None: ...

class AnalysisDynamics:
    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_db: float,
        is_compressed: bool,
    ) -> None: ...
    @property
    def dynamicRangeDb(self) -> float: ...
    @property
    def peakDb(self) -> float: ...
    @property
    def rmsDb(self) -> float: ...
    @property
    def crestFactor(self) -> float: ...
    @property
    def loudnessRangeDb(self) -> float: ...
    @property
    def isCompressed(self) -> bool: ...

class AnalysisTimbre:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    def __init__(
        self,
        brightness: float,
        warmth: float,
        density: float,
        roughness: float,
        complexity: float,
    ) -> None: ...

class AnalysisRhythm:
    time_signature: TimeSignature
    syncopation: float
    groove_type: str
    pattern_regularity: float
    tempo_stability: float
    def __init__(
        self,
        time_signature: TimeSignature,
        syncopation: float,
        groove_type: str,
        pattern_regularity: float,
        tempo_stability: float,
    ) -> None: ...
    @property
    def timeSignature(self) -> TimeSignature: ...
    @property
    def grooveType(self) -> str: ...
    @property
    def patternRegularity(self) -> float: ...
    @property
    def tempoStability(self) -> float: ...

class AnalysisMelody:
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    pitches: list[MelodyPoint]
    def __init__(
        self,
        pitch_range_octaves: float,
        pitch_stability: float,
        mean_frequency: float,
        vibrato_rate: float,
        pitches: list[MelodyPoint],
    ) -> None: ...
    @property
    def pitchRangeOctaves(self) -> float: ...
    @property
    def pitchStability(self) -> float: ...
    @property
    def meanFrequency(self) -> float: ...
    @property
    def vibratoRate(self) -> float: ...

class AnalysisBeatObservations:
    """Beat-level evidence the downbeat and meter pass scores, one value per
    beat and parallel to ``AnalysisResult.beat_times``. An empty stream means
    the analysis could not produce it, not that every beat scored zero:
    ``low_frequency_energy`` is empty without audio, and ``chord_change`` is
    empty until chords are analyzed. ``onset_strength`` is the windowed
    aggregate around each beat, distinct from ``AnalysisResult.beat_strengths``
    (a single raw unwindowed envelope frame)."""

    onset_strength: list[float]
    low_frequency_energy: list[float]
    chord_change: list[float]
    def __init__(
        self,
        onset_strength: list[float] = ...,
        low_frequency_energy: list[float] = ...,
        chord_change: list[float] = ...,
    ) -> None: ...
    @property
    def onsetStrength(self) -> list[float]: ...
    @property
    def lowFrequencyEnergy(self) -> list[float]: ...
    @property
    def chordChange(self) -> list[float]: ...

class AnalysisResult:
    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]
    beat_strengths: list[float]
    downbeat_indices: list[int]
    downbeat_phase: int
    bpm_candidates: list[BpmHypothesis]
    time_signature_candidates: list[TimeSignature]
    chords: list[Chord]
    sections: list[Section]
    timbre: AnalysisTimbre | None
    dynamics: AnalysisDynamics | None
    rhythm: AnalysisRhythm | None
    melody: AnalysisMelody | None
    beat_observations: AnalysisBeatObservations | None
    beat_local_bpm: list[float]
    form: str
    def __init__(
        self,
        bpm: float,
        bpm_confidence: float,
        key: Key,
        time_signature: TimeSignature,
        beat_times: list[float],
        beat_strengths: list[float] = ...,
        downbeat_indices: list[int] = ...,
        downbeat_phase: int = 0,
        bpm_candidates: list[BpmHypothesis] = ...,
        time_signature_candidates: list[TimeSignature] = ...,
        chords: list[Chord] = ...,
        sections: list[Section] = ...,
        timbre: AnalysisTimbre | None = None,
        dynamics: AnalysisDynamics | None = None,
        rhythm: AnalysisRhythm | None = None,
        melody: AnalysisMelody | None = None,
        beat_observations: AnalysisBeatObservations | None = None,
        beat_local_bpm: list[float] = ...,
        form: str = "",
    ) -> None: ...
    @property
    def bpmConfidence(self) -> float: ...
    @property
    def timeSignature(self) -> TimeSignature: ...
    @property
    def bpmCandidates(self) -> list[BpmHypothesis]: ...
    @property
    def timeSignatureCandidates(self) -> list[TimeSignature]: ...
    @property
    def beatTimes(self) -> list[float]: ...
    @property
    def beatStrengths(self) -> list[float]: ...
    @property
    def downbeatIndices(self) -> list[int]: ...
    @property
    def downbeatPhase(self) -> int: ...
    @property
    def beatObservations(self) -> AnalysisBeatObservations | None: ...
    @property
    def beatLocalBpm(self) -> list[float]: ...
    @property
    def beats(self) -> list[Beat]: ...

class MeterEstimate:
    """Meter estimated over a caller-supplied beat series. ``candidate_scores``
    is parallel to the numerators that were *requested*, in request order,
    while ``candidates`` is ordered by descending support — the two do not
    index alike. A score is standardized and signed: zero is the level a
    numerator reaches on beats carrying no meter, so only the ordering and the
    gaps between entries carry meaning, and a score grows with the square root
    of how many beats were scored, so scores from spans of different lengths
    are not comparable without normalizing for length. ``grouping`` is how the
    bar divides, in beats per accent group — ``[3, 2, 2]`` for a 7/8 notated
    3+2+2 — and always sums to the reported numerator; a single entry means no
    internal division was resolved. ``searched`` is False when the series was
    too short to score any candidate, in which case every other field is the
    fixed fallback rather than a measurement."""

    time_signature: TimeSignature
    downbeat_phase: int
    searched: bool
    grouping: list[int]
    candidate_scores: list[float]
    candidates: list[TimeSignature]
    def __init__(
        self,
        time_signature: TimeSignature,
        downbeat_phase: int,
        searched: bool,
        grouping: list[int] = ...,
        candidate_scores: list[float] = ...,
        candidates: list[TimeSignature] = ...,
    ) -> None: ...
    @property
    def timeSignature(self) -> TimeSignature: ...
    @property
    def downbeatPhase(self) -> int: ...
    @property
    def candidateScores(self) -> list[float]: ...

class BpmCandidate:
    bpm: float
    confidence: float
    def __init__(self, bpm: float, confidence: float) -> None: ...

class BpmAnalysisResult:
    bpm: float
    confidence: float
    candidates: list[BpmCandidate]
    autocorrelation: list[float]
    tempogram: list[float]
    def __init__(
        self,
        bpm: float,
        confidence: float,
        candidates: list[BpmCandidate],
        autocorrelation: list[float],
        tempogram: list[float],
    ) -> None: ...

class AcousticResult:
    """Room acoustic parameters from a blind recording or a measured impulse
    response (``is_blind`` distinguishes the two). Only ``rt60`` (and
    ``rt60_bands``) is estimated in both modes.

    ``c50``/``c80``/``d50`` and ``edt`` require a known direct-sound arrival
    time, which only a measured impulse response provides, so they are NaN when
    ``is_blind`` is true -- ``edt`` measures the 0 to -10 dB decay and the blind
    estimator only fits the late decay ``rt60`` comes from. ``c50_bands`` and
    ``c80_bands`` are then empty lists (not computed), while ``edt_bands`` stays
    a full-length list of NaNs so it can be indexed by the same band index as
    ``rt60_bands``."""

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
    def __init__(
        self,
        rt60: float,
        edt: float,
        c50: float,
        c80: float,
        d50: float,
        rt60_bands: list[float],
        edt_bands: list[float],
        c50_bands: list[float],
        c80_bands: list[float],
        confidence: float,
        is_blind: bool,
    ) -> None: ...
    @property
    def rt60Bands(self) -> list[float]: ...
    @property
    def edtBands(self) -> list[float]: ...
    @property
    def c50Bands(self) -> list[float]: ...
    @property
    def c80Bands(self) -> list[float]: ...
    @property
    def isBlind(self) -> bool: ...

class RirResult:
    rir: list[float]
    sample_rate: int
    has_error: bool
    error_message: str
    warning_message: str
    def __init__(
        self,
        rir: list[float],
        sample_rate: int,
        has_error: bool,
        error_message: str = "",
        warning_message: str = "",
    ) -> None: ...
    @property
    def sampleRate(self) -> int: ...
    @property
    def hasError(self) -> bool: ...
    @property
    def warningMessage(self) -> str: ...

class RoomEstimate:
    volume: float
    length: float
    width: float
    height: float
    drr_db: float
    confidence: float
    absorption_bands: list[float]
    rt60_bands: list[float]
    def __init__(
        self,
        volume: float,
        length: float,
        width: float,
        height: float,
        drr_db: float,
        confidence: float,
        absorption_bands: list[float],
        rt60_bands: list[float],
    ) -> None: ...
    @property
    def drrDb(self) -> float: ...
    @property
    def absorptionBands(self) -> list[float]: ...
    @property
    def rt60Bands(self) -> list[float]: ...

class LufsResult:
    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    loudness_range: float
    def __init__(
        self,
        integrated_lufs: float,
        momentary_lufs: float,
        short_term_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        loudness_range: float,
    ) -> None: ...
    @property
    def integratedLufs(self) -> float: ...
    @property
    def momentaryLufs(self) -> float: ...
    @property
    def shortTermLufs(self) -> float: ...
    @property
    def maxMomentaryLufs(self) -> float: ...
    @property
    def maxShortTermLufs(self) -> float: ...
    @property
    def loudnessRange(self) -> float: ...

class EqSpectrumSnapshot:
    """Realtime equalizer snapshot.

    ``pre_left``/``pre_right`` and ``post_left``/``post_right`` are the pre- and
    post-EQ waveform streams (uniformly decimated time-domain samples), so they
    are a scope feed rather than a spectral estimate. ``profile_db`` is the
    frequency-domain view: the post-EQ signal is Hann-windowed, transformed and
    its bin powers summed into 16 geometrically spaced bands covering 20 Hz to
    20 kHz, in amplitude decibels relative to full scale, rising immediately and
    falling smoothly.
    """

    pre_left: list[float]
    pre_right: list[float]
    post_left: list[float]
    post_right: list[float]
    band_gain_db: list[float]
    profile_db: list[float]
    last_auto_gain_db: float
    seq: int
    def __init__(
        self,
        pre_left: list[float],
        pre_right: list[float],
        post_left: list[float],
        post_right: list[float],
        band_gain_db: list[float],
        profile_db: list[float],
        last_auto_gain_db: float,
        seq: int,
    ) -> None: ...
    @property
    def preLeft(self) -> list[float]: ...
    @property
    def preRight(self) -> list[float]: ...
    @property
    def postLeft(self) -> list[float]: ...
    @property
    def postRight(self) -> list[float]: ...
    @property
    def bandGainDb(self) -> list[float]: ...
    @property
    def profileDb(self) -> list[float]: ...
    @property
    def lastAutoGainDb(self) -> float: ...

class RhythmResult:
    bpm: float
    time_signature: TimeSignature
    groove_type: str
    syncopation: float
    pattern_regularity: float
    tempo_stability: float
    beat_intervals: list[float]
    def __init__(
        self,
        bpm: float,
        time_signature: TimeSignature,
        groove_type: str,
        syncopation: float,
        pattern_regularity: float,
        tempo_stability: float,
        beat_intervals: list[float],
    ) -> None: ...
    @property
    def timeSignature(self) -> TimeSignature: ...
    @property
    def grooveType(self) -> str: ...
    @property
    def patternRegularity(self) -> float: ...
    @property
    def tempoStability(self) -> float: ...
    @property
    def beatIntervals(self) -> list[float]: ...

class DynamicsResult:
    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool
    loudness_times: list[float]
    loudness_rms_db: list[float]
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_db: float,
        is_compressed: bool,
        loudness_times: list[float],
        loudness_rms_db: list[float],
    ) -> None: ...
    @property
    def dynamicRangeDb(self) -> float: ...
    @property
    def peakDb(self) -> float: ...
    @property
    def rmsDb(self) -> float: ...
    @property
    def crestFactor(self) -> float: ...
    @property
    def loudnessRangeDb(self) -> float: ...
    @property
    def isCompressed(self) -> bool: ...
    @property
    def loudnessTimes(self) -> list[float]: ...
    @property
    def loudnessRmsDb(self) -> list[float]: ...

class ClippingRegion:
    start_sample: int
    end_sample: int
    length: int
    peak: float
    def __init__(self, start_sample: int, end_sample: int, length: int, peak: float) -> None: ...

class ClippingReport:
    clipped_samples: int
    clipping_ratio: float
    max_clipped_peak: float
    regions: list[ClippingRegion]
    def __init__(
        self,
        clipped_samples: int,
        clipping_ratio: float,
        max_clipped_peak: float,
        regions: list[ClippingRegion],
    ) -> None: ...

class DynamicRangeReport:
    dynamic_range_db: float
    low_percentile_db: float
    high_percentile_db: float
    window_rms_db: list[float]
    def __init__(
        self,
        dynamic_range_db: float,
        low_percentile_db: float,
        high_percentile_db: float,
        window_rms_db: list[float],
    ) -> None: ...

class InverseResult:
    rows: int
    n_frames: int
    data: list[float]
    def __init__(self, rows: int, n_frames: int, data: list[float]) -> None: ...

class VectorscopeReport:
    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    def __init__(self, mid: NDArray[np.float32], side: NDArray[np.float32]) -> None: ...

class PhaseScopeReport:
    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    radius: NDArray[np.float32]
    angle_rad: NDArray[np.float32]
    correlation: float
    average_abs_angle_rad: float
    max_radius: float
    def __init__(
        self,
        mid: NDArray[np.float32],
        side: NDArray[np.float32],
        radius: NDArray[np.float32],
        angle_rad: NDArray[np.float32],
        correlation: float,
        average_abs_angle_rad: float,
        max_radius: float,
    ) -> None: ...

class SpectrumReport:
    frequencies: NDArray[np.float32]
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]
    db: NDArray[np.float32]
    n_fft: int
    sample_rate: int
    def __init__(
        self,
        frequencies: NDArray[np.float32],
        magnitude: NDArray[np.float32],
        power: NDArray[np.float32],
        db: NDArray[np.float32],
        n_fft: int,
        sample_rate: int,
    ) -> None: ...

class WaveformPeaksReport:
    min: NDArray[np.float32]
    max: NDArray[np.float32]
    channels: int
    bucket_count: int
    samples_per_bucket: int
    def __init__(
        self,
        min: NDArray[np.float32],
        max: NDArray[np.float32],
        channels: int,
        bucket_count: int,
        samples_per_bucket: int,
    ) -> None: ...

class TimbreFrame:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    def __init__(
        self,
        brightness: float,
        warmth: float,
        density: float,
        roughness: float,
        complexity: float,
    ) -> None: ...

class TimbreResult:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    spectral_rolloff: list[float]
    timbre_over_time: list[TimbreFrame]
    def __init__(
        self,
        brightness: float,
        warmth: float,
        density: float,
        roughness: float,
        complexity: float,
        spectral_centroid: list[float],
        spectral_flatness: list[float],
        spectral_rolloff: list[float],
        timbre_over_time: list[TimbreFrame],
    ) -> None: ...
    @property
    def spectralCentroid(self) -> list[float]: ...
    @property
    def spectralFlatness(self) -> list[float]: ...
    @property
    def spectralRolloff(self) -> list[float]: ...
    @property
    def timbreOverTime(self) -> list[TimbreFrame]: ...

class Chord:
    root: PitchClass
    quality: str
    start: float
    end: float
    confidence: float
    bass: PitchClass | None
    canonical_name: str
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start: float,
        end: float,
        confidence: float,
        bass: PitchClass | None = None,
        canonical_name: str = "",
    ) -> None: ...
    @property
    def duration(self) -> float: ...
    @property
    def root_name(self) -> str: ...
    @property
    def bass_name(self) -> str: ...
    @property
    def rootName(self) -> str: ...
    @property
    def bassName(self) -> str: ...
    @property
    def name(self) -> str: ...

class ChordAnalysisResult:
    chords: list[Chord]
    def __init__(self, chords: list[Chord]) -> None: ...

class StftResult:
    n_bins: int
    n_frames: int
    n_fft: int
    hop_length: int
    sample_rate: int
    magnitude: list[float]
    power: list[float]
    def __init__(
        self,
        n_bins: int,
        n_frames: int,
        n_fft: int,
        hop_length: int,
        sample_rate: int,
        magnitude: list[float],
        power: list[float],
    ) -> None: ...

class MelSpectrogramResult:
    n_mels: int
    n_frames: int
    sample_rate: int
    hop_length: int
    power: list[float]
    db: list[float]
    def __init__(
        self,
        n_mels: int,
        n_frames: int,
        sample_rate: int,
        hop_length: int,
        power: list[float],
        db: list[float],
    ) -> None: ...

class MfccResult:
    n_mfcc: int
    n_frames: int
    coefficients: list[float]
    def __init__(self, n_mfcc: int, n_frames: int, coefficients: list[float]) -> None: ...

class ChromaResult:
    n_chroma: int
    n_frames: int
    sample_rate: int
    hop_length: int
    features: list[float]
    mean_energy: list[float]
    def __init__(
        self,
        n_chroma: int,
        n_frames: int,
        sample_rate: int,
        hop_length: int,
        features: list[float],
        mean_energy: list[float],
    ) -> None: ...

class PitchResult:
    n_frames: int
    f0: list[float]
    voiced_prob: list[float]
    voiced_flag: list[bool]
    median_f0: float
    mean_f0: float
    def __init__(
        self,
        n_frames: int,
        f0: list[float],
        voiced_prob: list[float],
        voiced_flag: list[bool],
        median_f0: float,
        mean_f0: float,
    ) -> None: ...

class PiptrackResult:
    n_bins: int
    n_frames: int
    pitches: list[float]
    magnitudes: list[float]
    def __init__(
        self, n_bins: int, n_frames: int, pitches: list[float], magnitudes: list[float]
    ) -> None: ...

class ReassignedSpectrogramResult:
    n_bins: int
    n_frames: int
    magnitude: list[float]
    times: list[float]
    frequencies: list[float]
    def __init__(
        self,
        n_bins: int,
        n_frames: int,
        magnitude: list[float],
        times: list[float],
        frequencies: list[float],
    ) -> None: ...

class SegmentMatrix:
    rows: int
    cols: int
    values: list[float]
    def __init__(self, rows: int, cols: int, values: list[float]) -> None: ...

class NoteSegment:
    frame_start: int
    frame_end: int
    start_seconds: float
    end_seconds: float
    median_cents: float
    def __init__(
        self,
        frame_start: int,
        frame_end: int,
        start_seconds: float,
        end_seconds: float,
        median_cents: float,
    ) -> None: ...

class HpssResult:
    harmonic: list[float]
    percussive: list[float]
    length: int
    sample_rate: int
    def __init__(
        self, harmonic: list[float], percussive: list[float], length: int, sample_rate: int
    ) -> None: ...

class MasteringResult:
    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
    ) -> None: ...

class MasteringStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
    ) -> None: ...

class StageGainReduction:
    stage: str
    gain_reduction_db: float
    def __init__(self, stage: str, gain_reduction_db: float) -> None: ...

class MasteringLoudnessSummary:
    integrated_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    true_peak_dbtp: float
    loudness_range: float
    def __init__(
        self,
        integrated_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        true_peak_dbtp: float,
        loudness_range: float,
    ) -> None: ...

class MasteringReport:
    before: MasteringLoudnessSummary
    after: MasteringLoudnessSummary
    applied_gain_db: float
    max_gain_reduction_db: float
    loudness_target_limited: bool
    band_energy_delta_db: list[float]
    def __init__(
        self,
        before: MasteringLoudnessSummary,
        after: MasteringLoudnessSummary,
        applied_gain_db: float,
        max_gain_reduction_db: float,
        loudness_target_limited: bool,
        band_energy_delta_db: list[float] = ...,
    ) -> None: ...

class MasteringChainResult:
    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_lra: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_lra: float = ...,
        loudness_target_limited: bool = ...,
        stage_gain_reductions: list[StageGainReduction] = ...,
        report: MasteringReport | None = ...,
    ) -> None: ...

class MasteringChainStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_lra: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_lra: float = ...,
        loudness_target_limited: bool = ...,
        stage_gain_reductions: list[StageGainReduction] = ...,
        report: MasteringReport | None = ...,
    ) -> None: ...

class MixMeterSnapshot:
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
    channel_count: int
    peak_db: tuple[float, ...]
    rms_db: tuple[float, ...]
    true_peak_db: tuple[float, ...]
    def __init__(
        self,
        peak_db_l: float,
        peak_db_r: float,
        rms_db_l: float,
        rms_db_r: float,
        correlation: float,
        mono_compat_width: float,
        mono_compat_peak: float,
        mono_compat_side_rms: float,
        likely_mono_compatible: bool,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        true_peak_db_l: float,
        true_peak_db_r: float,
        max_true_peak_db: float,
        seq: int,
        channel_count: int = ...,
        peak_db: tuple[float, ...] = ...,
        rms_db: tuple[float, ...] = ...,
        true_peak_db: tuple[float, ...] = ...,
    ) -> None: ...

class GoniometerPoint:
    left: float
    right: float
    def __init__(self, left: float, right: float) -> None: ...

class MixResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    meters: list[MixMeterSnapshot]
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        meters: list[MixMeterSnapshot],
    ) -> None: ...

class EngineTelemetry:
    type: EngineTelemetryType
    error: EngineTelemetryError
    render_frame: int
    timeline_sample: int
    audible_timeline_sample: int
    graph_latency_samples_q8: int
    value: int
    def __init__(
        self,
        type: EngineTelemetryType,
        error: EngineTelemetryError,
        render_frame: int,
        timeline_sample: int,
        audible_timeline_sample: int,
        graph_latency_samples_q8: int,
        value: int,
    ) -> None: ...
    @property
    def renderFrame(self) -> int: ...
    @property
    def timelineSample(self) -> int: ...
    @property
    def audibleTimelineSample(self) -> int: ...
    @property
    def graphLatencySamplesQ8(self) -> int: ...

class EngineTrackMonitorMode(IntEnum):
    OFF = 0
    PFL = 1
    AFL = 2

class ParameterInfo:
    id: int
    name: str
    unit: str
    min_value: float
    max_value: float
    default_value: float
    rt_safe: bool
    default_curve: AutomationCurve
    def __init__(
        self,
        id: int,
        name: str,
        unit: str,
        min_value: float,
        max_value: float,
        default_value: float,
        rt_safe: bool,
        default_curve: AutomationCurve,
    ) -> None: ...

class AutomationPoint:
    ppq: float
    value: float
    curve_to_next: AutomationCurve
    def __init__(
        self,
        ppq: float,
        value: float,
        curve_to_next: AutomationCurve = AutomationCurve.LINEAR,
    ) -> None: ...

class MarkerKind(IntEnum):
    MARKER = 0
    TEXT = 1
    LYRIC = 2
    CUE_POINT = 3
    KEY_SIGNATURE = 4

class EngineMarker:
    id: int
    ppq: float
    name: str
    kind: int
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: int = 0,
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectMarker:
    id: int
    ppq: float
    name: str
    kind: int
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: int = 0,
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectTrack:
    id: int
    kind: int
    midi_destination_id: int
    gain: float
    pan: float
    mute: bool
    solo: bool
    name: str
    def __init__(
        self,
        id: int,
        kind: int,
        midi_destination_id: int,
        gain: float,
        pan: float,
        mute: bool,
        solo: bool,
        name: str,
    ) -> None: ...

class ProjectClip:
    id: int
    track_id: int
    source_id: int
    source_kind: int
    start_ppq: float
    length_ppq: float
    source_offset_ppq: float
    gain: float
    loop_mode: int
    loop_length_ppq: float
    def __init__(
        self,
        id: int,
        track_id: int,
        source_id: int,
        source_kind: int,
        start_ppq: float,
        length_ppq: float,
        source_offset_ppq: float,
        gain: float,
        loop_mode: int,
        loop_length_ppq: float,
    ) -> None: ...

class ProjectSource:
    id: int
    kind: int
    channel_count: int
    storage_handle_id: int
    sample_rate_hint: float
    name_or_uri: str
    content_hash: str
    external_stem_role: str
    def __init__(
        self,
        id: int,
        kind: int,
        channel_count: int,
        storage_handle_id: int,
        sample_rate_hint: float,
        name_or_uri: str,
        content_hash: str = "",
        external_stem_role: str = "",
    ) -> None: ...

class EngineMetronomeConfig:
    enabled: bool
    beat_gain: float
    accent_gain: float
    click_samples: int
    click_seconds: float
    def __init__(
        self,
        enabled: bool = False,
        beat_gain: float = 0.35,
        accent_gain: float = 0.7,
        click_samples: int = 0,
        click_seconds: float = 0.0,
    ) -> None: ...

class EngineClip:
    id: int
    channels: list[list[float]] | None
    start_ppq: float
    track_id: int
    length_samples: int | None
    clip_offset_samples: int
    loop: bool
    gain: float
    fade_in_samples: int
    fade_out_samples: int
    warp_mode: str | int
    warp_anchors: list[tuple[float, float]] | None
    page_provider: object | None
    def __init__(
        self,
        id: int,
        channels: list[list[float]] | None,
        start_ppq: float,
        track_id: int = 0,
        length_samples: int | None = None,
        clip_offset_samples: int = 0,
        loop: bool = False,
        gain: float = 1.0,
        fade_in_samples: int = 0,
        fade_out_samples: int = 0,
        warp_mode: str | int = "off",
        warp_anchors: list[tuple[float, float]] | None = None,
        page_provider: object | None = None,
    ) -> None: ...

class EngineMidiEvent:
    render_frame: int
    word0: int
    word1: int
    word2: int
    word3: int
    word_count: int
    group: int
    sysex_handle: int
    def __init__(
        self,
        render_frame: int,
        word0: int = 0,
        word1: int = 0,
        word2: int = 0,
        word3: int = 0,
        word_count: int = 0,
        group: int = 0,
        sysex_handle: int = 0,
    ) -> None: ...

class EngineMidiClipSchedule:
    events: list[EngineMidiEvent]
    id: int
    track_id: int
    destination_id: int | None
    start_sample: int
    start_ppq: float
    length_samples: int
    loop: bool
    loop_length_samples: int
    def __init__(
        self,
        events: list[EngineMidiEvent],
        id: int = 0,
        track_id: int = 0,
        destination_id: int | None = None,
        start_sample: int = 0,
        start_ppq: float = 0.0,
        length_samples: int = 0,
        loop: bool = False,
        loop_length_samples: int = 0,
    ) -> None: ...

class ClipPageRequest:
    clip_id: int
    channel: int
    sample: int
    def __init__(self, clip_id: int, channel: int, sample: int) -> None: ...

class EngineCaptureStatus:
    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool
    source: str
    record_offset_samples: int
    def __init__(
        self,
        captured_frames: int,
        overflow_count: int,
        armed: bool,
        punch_enabled: bool,
        source: str,
        record_offset_samples: int,
    ) -> None: ...

class EngineBounceOptions:
    total_frames: int
    block_size: int
    num_channels: int
    target_sample_rate: int
    source_sample_rate: int
    normalize_lufs: bool
    target_lufs: float
    dither: int
    dither_bits: int
    dither_seed: int
    def __init__(
        self,
        total_frames: int,
        block_size: int = 128,
        num_channels: int = 2,
        target_sample_rate: int = 48000,
        source_sample_rate: int = 48000,
        normalize_lufs: bool = False,
        target_lufs: float = -14.0,
        dither: int = 0,
        dither_bits: int = 16,
        dither_seed: int = 0,
    ) -> None: ...

class EngineBounceResult:
    interleaved: list[float]
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float
    def __init__(
        self,
        interleaved: list[float],
        frames: int,
        num_channels: int,
        sample_rate: int,
        integrated_lufs: float,
    ) -> None: ...

class EngineFreezeOptions:
    total_frames: int
    block_size: int
    num_channels: int
    clip_id: int
    start_ppq: float
    gain: float
    def __init__(
        self,
        total_frames: int,
        block_size: int = 128,
        num_channels: int = 2,
        clip_id: int = 1,
        start_ppq: float = 0.0,
        gain: float = 1.0,
    ) -> None: ...

class EngineFreezeResult:
    clip_id: int
    frames: int
    num_channels: int
    def __init__(self, clip_id: int, frames: int, num_channels: int) -> None: ...

class EngineGraphNodeType(IntEnum):
    PASS_THROUGH = 0
    GAIN = 1

class EngineGraphMix(IntEnum):
    """Mixing intent for a graph edge.

    NOTE: not currently honored -- the compiled graph always sums edges into a
    shared destination port in an order-independent way (the first edge into a
    port overwrites, every later edge adds), regardless of this value. Retained
    for API compatibility and to express intent; multiple edges into one port
    are always summed.
    """

    REPLACE = 0
    ADD = 1

class EngineGraphNode:
    id: str
    type: EngineGraphNodeType
    gain_db: float
    num_ports: int
    def __init__(
        self,
        id: str,
        type: EngineGraphNodeType = EngineGraphNodeType.PASS_THROUGH,
        gain_db: float = 0.0,
        num_ports: int = 0,
    ) -> None: ...

class EngineGraphConnection:
    source_node: str
    source_port: int
    dest_node: str
    dest_port: int
    mix: EngineGraphMix
    def __init__(
        self,
        source_node: str,
        source_port: int,
        dest_node: str,
        dest_port: int,
        mix: EngineGraphMix = EngineGraphMix.ADD,
    ) -> None: ...

class EngineGraphParameterBinding:
    param_id: int
    node_id: str
    def __init__(self, param_id: int, node_id: str) -> None: ...

class EngineGraphSpec:
    nodes: list[EngineGraphNode]
    connections: list[EngineGraphConnection]
    input_node: str
    output_node: str
    num_channels: int
    parameter_bindings: list[EngineGraphParameterBinding] | None
    def __init__(
        self,
        nodes: list[EngineGraphNode],
        connections: list[EngineGraphConnection],
        input_node: str,
        output_node: str,
        num_channels: int = 2,
        parameter_bindings: list[EngineGraphParameterBinding] | None = None,
    ) -> None: ...

class MeterTelemetryRecord:
    """Stereo meter snapshot drained from the engine meter tap.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

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
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        peak_db_l: float,
        peak_db_r: float,
        rms_db_l: float,
        rms_db_r: float,
        true_peak_db_l: float,
        true_peak_db_r: float,
        max_true_peak_db: float,
        correlation: float,
        mono_compat_width: float,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        dropped_records: int,
    ) -> None: ...

class MeterTelemetryRecordWide:
    """Per-plane meter snapshot for a surround mix target.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

    target_id: int
    render_frame: int
    seq: int
    channel_count: int
    peak_db: list[float]
    rms_db: list[float]
    true_peak_db: list[float]
    max_true_peak_db: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        channel_count: int,
        peak_db: list[float],
        rms_db: list[float],
        true_peak_db: list[float],
        max_true_peak_db: float,
        correlation: float,
        mono_compat_width: float,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        dropped_records: int,
    ) -> None: ...

class ScopeTelemetryRecord:
    """Spectrum + goniometer snapshot drained from the scope tap.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

    target_id: int
    render_frame: int
    seq: int
    dropped_records: int
    bands: list[float]
    points: list[tuple[float, float]]
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        dropped_records: int,
        bands: list[float],
        points: list[tuple[float, float]],
    ) -> None: ...

class ExternalMidiEvent:
    destination_id: int
    render_frame: int
    bytes: builtins.bytes
    def __init__(
        self,
        destination_id: int,
        render_frame: int,
        bytes: builtins.bytes,
    ) -> None: ...

class TransportState:
    playing: bool
    looping: bool
    render_frame: int
    sample_position: int
    ppq_position: float
    bpm: float
    loop_start_ppq: float
    loop_end_ppq: float
    sample_rate: float
    bar_start_ppq: float
    bar_count: int
    time_signature: TimeSignature
    beat: int
    beat_fraction: float
    def __init__(
        self,
        playing: bool,
        looping: bool,
        render_frame: int,
        sample_position: int,
        ppq_position: float,
        bpm: float,
        loop_start_ppq: float,
        loop_end_ppq: float,
        sample_rate: float,
        bar_start_ppq: float,
        bar_count: int,
        time_signature: TimeSignature,
        beat: int,
        beat_fraction: float,
    ) -> None: ...

class Section:
    type: SectionType
    start: float
    end: float
    energy_level: float
    confidence: float
    canonical_name: str
    def __init__(
        self,
        type: SectionType,
        start: float,
        end: float,
        energy_level: float,
        confidence: float,
        canonical_name: str = "",
    ) -> None: ...
    @property
    def name(self) -> str: ...

class SectionResult:
    sections: list[Section]
    def __init__(self, sections: list[Section]) -> None: ...

class MelodyPoint:
    time: float
    frequency: float
    confidence: float
    def __init__(self, time: float, frequency: float, confidence: float) -> None: ...

class MelodyResult:
    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    def __init__(
        self,
        points: list[MelodyPoint],
        pitch_range_octaves: float,
        pitch_stability: float,
        mean_frequency: float,
        vibrato_rate: float,
    ) -> None: ...

class CqtResult:
    n_bins: int
    n_frames: int
    hop_length: int
    sample_rate: int
    magnitude: list[float]
    frequencies: list[float]
    def __init__(
        self,
        n_bins: int,
        n_frames: int,
        hop_length: int,
        sample_rate: int,
        magnitude: list[float],
        frequencies: list[float],
    ) -> None: ...

class QuantizeConfig:
    mel_db_min: float
    mel_db_max: float
    onset_max: float
    rms_max: float
    centroid_max: float
    def __init__(
        self,
        mel_db_min: float = -80.0,
        mel_db_max: float = 0.0,
        onset_max: float = 50.0,
        rms_max: float = 1.0,
        centroid_max: float = 11025.0,
    ) -> None: ...

class StreamConfig:
    sample_rate: int
    n_fft: int
    hop_length: int
    n_mels: int
    fmin: float
    fmax: float
    tuning_ref_hz: float
    compute_magnitude: bool
    compute_mel: bool
    compute_chroma: bool
    compute_onset: bool
    compute_spectral: bool
    emit_every_n_frames: int
    magnitude_downsample: int
    max_pending_frames: int  # Overflow drops the newly produced frame.
    max_progression_entries: int
    key_update_interval_sec: float
    bpm_update_interval_sec: float
    window: int
    output_format: int  # Deprecated compatibility field; must be 0 (Float32).
    def __init__(
        self,
        sample_rate: int = 44100,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
        fmin: float = 0.0,
        fmax: float = 0.0,
        tuning_ref_hz: float = 440.0,
        compute_magnitude: bool = False,
        compute_mel: bool = True,
        compute_chroma: bool = True,
        compute_onset: bool = True,
        compute_spectral: bool = True,
        emit_every_n_frames: int = 1,
        magnitude_downsample: int = 1,
        max_pending_frames: int = 4096,
        max_progression_entries: int = 4096,
        key_update_interval_sec: float = 5.0,
        bpm_update_interval_sec: float = 10.0,
        window: int = 0,
        output_format: int = 0,
    ) -> None: ...

class StreamFrames:
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
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps: list[float],
        mel: list[float],
        chroma: list[float],
        onset_strength: list[float],
        rms_energy: list[float],
        spectral_centroid: list[float],
        spectral_flatness: list[float],
        chord_root: list[int],
        chord_quality: list[int],
        chord_confidence: list[float],
    ) -> None: ...

class StreamFramesU8:
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
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps: list[float],
        mel: list[int],
        chroma: list[int],
        onset_strength: list[int],
        rms_energy: list[int],
        spectral_centroid: list[int],
        spectral_flatness: list[int],
    ) -> None: ...

class StreamFramesI16:
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
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps: list[float],
        mel: list[int],
        chroma: list[int],
        onset_strength: list[int],
        rms_energy: list[int],
        spectral_centroid: list[int],
        spectral_flatness: list[int],
    ) -> None: ...

class StreamChordChange:
    root: int
    quality: int
    start_time: float
    confidence: float
    def __init__(self, root: int, quality: int, start_time: float, confidence: float) -> None: ...

class StreamBarChord:
    """``bar_index`` is the bar number, not this entry's position in the list:
    bars with no confident chord are not recorded and the oldest entries are
    dropped at the history cap, so group bars by pattern position with
    ``bar_index``. ``start_time`` is on the same timeline as
    ``StreamFrame.timestamp`` and consecutive bars are ``bar_duration`` apart.
    In ``voted_pattern``, ``bar_index`` is the pattern position and
    ``start_time`` is unused."""

    bar_index: int
    root: int
    quality: int
    start_time: float
    confidence: float
    def __init__(
        self, bar_index: int, root: int, quality: int, start_time: float, confidence: float
    ) -> None: ...

class StreamPatternScore:
    name: str
    score: float
    def __init__(self, name: str, score: float) -> None: ...

class StreamStats:
    """``bpm_candidate_count`` is the number of tempo candidates the most recent
    BPM estimate chose from, 0 until one has run — the same quantity as
    ``AnalysisResult``'s field of the same name. ``updated`` is true when the
    key or BPM was re-estimated since the previous snapshot: one change sets it
    on exactly one snapshot however the caller chunks its input."""

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
    def __init__(
        self,
        total_frames: int,
        total_samples: int,
        duration_seconds: float,
        pending_frames: int,
        dropped_output_frames: int,
        dropped_chord_progression_entries: int,
        dropped_bar_progression_entries: int,
        bpm: float,
        bpm_confidence: float,
        bpm_candidate_count: int,
        key: int,
        key_minor: bool,
        key_confidence: float,
        chord_root: int,
        chord_quality: int,
        chord_confidence: float,
        chord_start_time: float,
        current_bar: int,
        bar_duration: float,
        chord_progression: list[StreamChordChange],
        bar_chord_progression: list[StreamBarChord],
        voted_pattern: list[StreamBarChord],
        pattern_length: int,
        detected_pattern_name: str,
        detected_pattern_score: float,
        all_pattern_scores: list[StreamPatternScore],
        accumulated_seconds: float,
        used_frames: int,
        updated: bool,
    ) -> None: ...
