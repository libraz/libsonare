from __future__ import annotations

from enum import IntEnum

import numpy as np
from numpy.typing import NDArray

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

class RirResult:
    rir: list[float]
    sample_rate: int
    has_error: bool
    def __init__(self, rir: list[float], sample_rate: int, has_error: bool) -> None: ...
    @property
    def sampleRate(self) -> int: ...
    @property
    def hasError(self) -> bool: ...

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
        click_samples: int = 96,
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

class ScopeTelemetryRecord:
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
    ) -> None: ...

class Section:
    type: SectionType
    start: float
    end: float
    energy_level: float
    confidence: float
    def __init__(
        self,
        type: SectionType,
        start: float,
        end: float,
        energy_level: float,
        confidence: float,
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
    key_update_interval_sec: float
    bpm_update_interval_sec: float
    window: int
    output_format: int
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
        key_update_interval_sec: float = 5.0,
        bpm_update_interval_sec: float = 10.0,
        window: int = 0,
        output_format: int = 0,
    ) -> None: ...

class StreamFrames:
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
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
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
        timestamps: list[float],
        mel: list[int],
        chroma: list[int],
        onset_strength: list[int],
        rms_energy: list[int],
        spectral_centroid: list[int],
        spectral_flatness: list[int],
    ) -> None: ...

class StreamFramesI16(StreamFramesU8): ...

class StreamChordChange:
    root: int
    quality: int
    start_time: float
    confidence: float
    def __init__(self, root: int, quality: int, start_time: float, confidence: float) -> None: ...

class StreamBarChord:
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
    def __init__(
        self,
        total_frames: int,
        total_samples: int,
        duration_seconds: float,
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
