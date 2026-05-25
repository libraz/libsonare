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
