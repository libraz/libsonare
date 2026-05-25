from __future__ import annotations

from enum import IntEnum

class PitchClass(IntEnum):
    C: PitchClass
    CS: PitchClass
    D: PitchClass
    DS: PitchClass
    E: PitchClass
    F: PitchClass
    FS: PitchClass
    G: PitchClass
    GS: PitchClass
    A: PitchClass
    AS: PitchClass
    B: PitchClass

class Mode(IntEnum):
    MAJOR: Mode
    MINOR: Mode
    DORIAN: Mode
    PHRYGIAN: Mode
    LYDIAN: Mode
    MIXOLYDIAN: Mode
    LOCRIAN: Mode

class KeyProfile(IntEnum):
    KRUMHANSL_SCHMUCKLER: KeyProfile
    TEMPERLEY: KeyProfile
    SHAATH: KeyProfile
    FARALDO_EDMT: KeyProfile
    FARALDO_EDMA: KeyProfile
    FARALDO_EDMM: KeyProfile
    BELLMAN_BUDGE: KeyProfile

class AutomationCurve(IntEnum):
    LINEAR: AutomationCurve
    EXPONENTIAL: AutomationCurve

class PanLaw(IntEnum):
    CONST_3DB: PanLaw
    CONST_4_5DB: PanLaw
    CONST_6DB: PanLaw
    LINEAR_0DB: PanLaw

class MeterTap(IntEnum):
    PRE_FADER: MeterTap
    POST_FADER: MeterTap

class EngineTelemetryType(IntEnum):
    PROCESS_BLOCK: EngineTelemetryType
    ERROR: EngineTelemetryType

class EngineTelemetryError(IntEnum):
    NONE: EngineTelemetryError
    COMMAND_QUEUE_OVERFLOW: EngineTelemetryError
    PENDING_COMMAND_OVERFLOW: EngineTelemetryError
    BOUNDARY_OVERFLOW: EngineTelemetryError
    TELEMETRY_OVERFLOW: EngineTelemetryError
    CAPTURE_OVERFLOW: EngineTelemetryError
    MAX_BLOCK_EXCEEDED: EngineTelemetryError
    UNKNOWN_TARGET: EngineTelemetryError
    NON_REALTIME_SAFE_PARAMETER: EngineTelemetryError
    NOT_PREPARED: EngineTelemetryError

class AutomationPointCurve(IntEnum):
    HOLD: AutomationPointCurve
    LINEAR: AutomationPointCurve
    EXPONENTIAL: AutomationPointCurve
    S_CURVE: AutomationPointCurve

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
    time: float
    strength: float | None
    def __init__(self, time: float, strength: float | None = None) -> None: ...

class AnalysisResult:
    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]
    def __init__(
        self,
        bpm: float,
        bpm_confidence: float,
        key: Key,
        time_signature: TimeSignature,
        beat_times: list[float],
    ) -> None: ...
    @property
    def bpmConfidence(self) -> float: ...
    @property
    def timeSignature(self) -> TimeSignature: ...
    @property
    def beatTimes(self) -> list[float]: ...
    @property
    def beats(self) -> list[Beat]: ...

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

class LufsResult:
    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    loudness_range: float
    def __init__(
        self,
        integrated_lufs: float,
        momentary_lufs: float,
        short_term_lufs: float,
        loudness_range: float,
    ) -> None: ...
    @property
    def integratedLufs(self) -> float: ...
    @property
    def momentaryLufs(self) -> float: ...
    @property
    def shortTermLufs(self) -> float: ...
    @property
    def loudnessRange(self) -> float: ...

class EqSpectrumSnapshot:
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

class TimbreResult:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    spectral_rolloff: list[float]
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
    ) -> None: ...
    @property
    def spectralCentroid(self) -> list[float]: ...
    @property
    def spectralFlatness(self) -> list[float]: ...
    @property
    def spectralRolloff(self) -> list[float]: ...

class Chord:
    root: PitchClass
    quality: str
    start: float
    end: float
    confidence: float
    bass: PitchClass | None
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start: float,
        end: float,
        confidence: float,
        bass: PitchClass | None = None,
    ) -> None: ...
    @property
    def duration(self) -> float: ...
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
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
    ) -> None: ...

class MasteringStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
    ) -> None: ...

class MasteringChainResult:
    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
    ) -> None: ...

class MasteringChainStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
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

class GoniometerPoint:
    left: float
    right: float
    def __init__(self, left: float, right: float) -> None: ...

class MixResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    meters: list[MixMeterSnapshot]

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

class ParameterInfo:
    id: int
    name: str
    unit: str
    min_value: float
    max_value: float
    default_value: float
    rt_safe: bool
    default_curve: AutomationPointCurve
    def __init__(
        self,
        id: int,
        name: str,
        unit: str,
        min_value: float,
        max_value: float,
        default_value: float,
        rt_safe: bool,
        default_curve: AutomationPointCurve,
    ) -> None: ...

class AutomationPoint:
    ppq: float
    value: float
    curve_to_next: AutomationPointCurve
    def __init__(
        self,
        ppq: float,
        value: float,
        curve_to_next: AutomationPointCurve = AutomationPointCurve.LINEAR,
    ) -> None: ...

class EngineMarker:
    id: int
    ppq: float
    name: str
    def __init__(self, id: int, ppq: float, name: str = "") -> None: ...

class EngineMetronomeConfig:
    enabled: bool
    beat_gain: float
    accent_gain: float
    click_samples: int
    def __init__(
        self,
        enabled: bool = False,
        beat_gain: float = 0.35,
        accent_gain: float = 0.7,
        click_samples: int = 96,
    ) -> None: ...

class EngineClip:
    id: int
    channels: list[list[float]]
    start_ppq: float
    length_samples: int | None
    clip_offset_samples: int
    loop: bool
    gain: float
    fade_in_samples: int
    fade_out_samples: int
    def __init__(
        self,
        id: int,
        channels: list[list[float]],
        start_ppq: float,
        length_samples: int | None = None,
        clip_offset_samples: int = 0,
        loop: bool = False,
        gain: float = 1.0,
        fade_in_samples: int = 0,
        fade_out_samples: int = 0,
    ) -> None: ...

class EngineCaptureStatus:
    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool
    def __init__(
        self,
        captured_frames: int,
        overflow_count: int,
        armed: bool,
        punch_enabled: bool,
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
    PASS_THROUGH: EngineGraphNodeType
    GAIN: EngineGraphNodeType

class EngineGraphMix(IntEnum):
    REPLACE: EngineGraphMix
    ADD: EngineGraphMix

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
