from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping, Sequence
from typing import Any, Literal, TypeAlias, overload

import numpy as np
import numpy.typing as npt

from ._conversions import SilenceCommonReport as SilenceCommonReport
from ._conversions import SplitSilenceCommonWithReportResult as SplitSilenceCommonWithReportResult
from ._conversions import TrimResult as TrimResult
from ._effects_note_model import NoteEdit as NoteEdit
from ._effects_note_model import NoteEditInput as NoteEditInput
from ._effects_note_model import NoteObject as NoteObject
from ._effects_note_model import NoteObjectInput as NoteObjectInput
from ._effects_note_model import NoteSetEntry as NoteSetEntry
from ._effects_note_model import PitchDecompositionResult as PitchDecompositionResult
from ._effects_note_model import TranscribeResult as TranscribeResult
from ._effects_note_ops import AssignNoteTargetsResult as AssignNoteTargetsResult
from ._effects_note_ops import NoteTarget as NoteTarget
from ._effects_percussive import PercussiveEvent as PercussiveEvent
from ._effects_percussive import PercussiveEventEdit as PercussiveEventEdit
from ._effects_percussive import PercussiveEventEditInput as PercussiveEventEditInput
from ._effects_percussive import PercussiveEventInput as PercussiveEventInput
from ._effects_separation import DecomposeResult as DecomposeResult
from ._effects_separation import DecomposeStemsLinkedResult as DecomposeStemsLinkedResult
from ._effects_separation import DecomposeStemsResult as DecomposeStemsResult
from ._effects_spectral import SpectralRegionOp as SpectralRegionOp
from ._effects_voice import CharacterType as CharacterType
from ._effects_voice import FormantMode as FormantMode
from ._effects_voice import MidiRelease as MidiRelease
from ._effects_voice import NoteDivision as NoteDivision
from ._effects_voice import PitchCorrectionMode as PitchCorrectionMode
from ._effects_voice import RealtimeVoiceChanger as RealtimeVoiceChanger
from ._effects_voice import RealtimeVoiceChangerConfig as RealtimeVoiceChangerConfig
from ._effects_voice import ReverbVariation as ReverbVariation
from ._effects_voice import ScaleKind as ScaleKind
from ._effects_voice import StreamingRetune as StreamingRetune
from ._effects_voice import VocoderCarrierSource as VocoderCarrierSource
from ._effects_voice import VocoderCarrierWave as VocoderCarrierWave
from ._effects_voice import VoiceEngine as VoiceEngine
from ._mastering_streaming import StreamingEqualizer as StreamingEqualizer
from ._mastering_streaming import StreamingMasteringChain as StreamingMasteringChain
from ._mixing import Mixer as Mixer
from ._mixing import MixerStereoResult as MixerStereoResult
from ._runtime import IntSamples
from ._types_engine import PlanarChannels
from ._types_mastering import (
    MasteringAssistantParams,
    MasteringParams,
    MasteringPreset,
    SoloProcessor,
)
from .types import (
    AcousticModeName,
    AcousticResult,
    AnalysisResult,
    AutoTuneResult,
    BeatObservations,
    BoundaryResult,
    BpmAnalysisResult,
    Capabilities,
    CapabilityCatalog,
    Chord,
    ChordAnalysisResult,
    ChordChromaMethod,
    ChromaResult,
    ClickDetection,
    ClipDetection,
    ClippingReport,
    CompressorDetector,
    CqtResult,
    CrackleDetection,
    DeclickResult,
    DeclickStereoResult,
    DeclipResult,
    DeclipStereoResult,
    DecrackleMode,
    DecrackleResult,
    DecrackleStereoResult,
    DehumMode,
    DehumResult,
    DehumStereoResult,
    DenoiseClassicalMode,
    DenoiseClassicalNoiseEstimator,
    DenoiseLinkedResult,
    DenoiseResult,
    DenoiseStereoResult,
    DereverbClassicalConfig,
    DereverbLinkedResult,
    DereverbResult,
    DereverbStereoResult,
    DynamicRangeReport,
    DynamicsProcessorResult,
    DynamicsResult,
    FunctionalChord,
    GenreHint,
    HpssResult,
    HumDetection,
    Key,
    KeyCandidate,
    KeyDetection,
    KeyMode,
    KeyProfile,
    LoudnessMatchResult,
    LoudnessMatchStereoResult,
    LufsResult,
    LufsSeriesResult,
    MasteringAmpPresetCatalogEntry,
    MasteringAssistantResult,
    MasteringAudioProfile,
    MasteringChainConfig,
    MasteringChainConfigInput,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringInsertParamInfo,
    MasteringInsertTiming,
    MasteringProcessorCatalogEntry,
    MasteringRepairAnalysis,
    MasteringResult,
    MasteringStereoResult,
    MasteringStreamingPreviewResult,
    MatchEqCurveResult,
    MatchEstimateReferenceDelaySamplesResult,
    MatchReferenceLoudnessResult,
    MatchTonalBalanceResult,
    MaterialPresetName,
    MelNorm,
    MelodyResult,
    MelSpectrogramResult,
    MeterEstimate,
    MfccResult,
    MixResult,
    MixSceneDocument,
    NmfInit,
    NnFilterAggregate,
    NoiseDetection,
    NormalizeStereoResult,
    NormType,
    NoteSegment,
    PanMode,
    PhaseScopeReport,
    PiptrackResult,
    PitchClass,
    PitchResult,
    PitchTargetMode,
    ReassignedSpectrogramResult,
    RepairApplyResult,
    RepairStage,
    ReverbDetection,
    RhythmResult,
    RirResult,
    RoomEstimate,
    RoomGeometry,
    RoomMorphResult,
    RoomMorphStereoResult,
    SectionAnalysis,
    SpectrumReport,
    StereoMonoCompatCheckLogBandsResult,
    StereoMonoCompatCheckResult,
    StftResult,
    StreamingLoudnessGain,
    StreamingPlatform,
    TempogramMode,
    TimbreResult,
    TrimRange,
    TrimSilenceMode,
    TrimSilenceResult,
    TrimSilenceStereoResult,
    VectorscopeReport,
    VoicePresetId,
    WaveformPeaksReport,
    WindowType,
)

_FloatSamples: TypeAlias = Sequence[float] | list[float] | np.ndarray[Any, Any]

def abi_version() -> int: ...
def engine_abi_version() -> int: ...
def voice_changer_abi_version() -> int | None: ...

_ProgressCallback: TypeAlias = Callable[[float, str], object]
_CancelCallback: TypeAlias = Callable[[], bool]

_PairProcessor: TypeAlias = Literal[
    "match.applyMatchEq",
    "match.alignReferenceToSource",
    "match.abSwitch",
    "match.abCrossfade",
]
_StereoPairProcessor: TypeAlias = Literal["match.abCrossfade"]
_PairAnalysis: TypeAlias = Literal[
    "match.referenceLoudness",
    "match.tonalBalance",
    "match.tonalBalanceLogBands",
    "match.matchEqCurve",
    "match.estimateReferenceDelaySamples",
]
_StereoAnalysis: TypeAlias = Literal["stereo.monoCompatCheck", "stereo.monoCompatCheckLogBands"]

def detect_bpm(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
) -> float | None: ...
def detect_key(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    chroma_high_pass_hz: float = 80.0,
    modes: Sequence[KeyMode] | str | None = None,
    profile: KeyProfile | None = None,
    genre_hint: GenreHint | None = None,
    tuning_cents: float | str = "auto",
) -> KeyDetection:
    """``n_fft`` is in samples at 22050 Hz, rescaled to the analysis rate; ``hop_length`` is the
    hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is
    above 22050 Hz, the input itself otherwise."""

def detect_key_candidates(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    chroma_high_pass_hz: float = 80.0,
    modes: Sequence[KeyMode] | str | None = None,
    profile: KeyProfile | None = None,
    genre_hint: GenreHint | None = None,
    tuning_cents: float | str = "auto",
) -> list[KeyCandidate]: ...
def detect_beats(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
) -> np.ndarray[Any, Any]: ...
def detect_downbeats(samples: _FloatSamples, sample_rate: int) -> np.ndarray[Any, Any]: ...
def detect_onsets(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    threshold: float | None = None,
    pre_max: int = 1,
    post_max: int = 1,
    pre_avg: int = 3,
    post_avg: int = 4,
    delta: float = 0.06,
    wait: int = 1,
    backtrack: bool = False,
    backtrack_range: int = 10,
) -> np.ndarray[Any, Any]: ...
def analyze(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    start_bpm: float = 120.0,
    use_triads_only: bool = True,
    use_hpss: bool = False,
    chroma_high_pass_hz: float = 80.0,
    use_bass_weighted: bool = True,
    chroma_hop_multiplier: int = 4,
    use_chord_hmm: bool = True,
    use_chord_key_context: bool = False,
    chord_hmm_beam_width: int = 24,
    detect_chord_inversions: bool = False,
    adaptive_tempo: bool = False,
    tempo_update_interval_beats: int = 8,
    compute_tempo_curve: bool = False,
    meter_candidate_numerators: Sequence[int] | None = None,
    meter_denominator: int = 4,
    tuning_cents: float | str = "auto",
    on_progress: _ProgressCallback | None = None,
    cancel: _CancelCallback | None = None,
) -> AnalysisResult:
    """``n_fft`` is in samples at 22050 Hz, rescaled to the analysis rate; ``hop_length`` is the
    hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is
    above 22050 Hz, the input itself otherwise. ``start_bpm`` is a finite, positive BPM prior
    that seeds beat tracking if the tempo search finds none. The result's ``bpm`` is ``None``
    when no tempo is measured."""

def estimate_meter(
    observations: BeatObservations,
    *,
    candidate_numerators: Sequence[int] | None = None,
    denominator: int = 4,
    downbeat_weight: float = 1.0,
    measure_weight: float = 0.5,
    subdivision_weight: float = 0.15,
    compound_subdivision_threshold: float = 0.5,
) -> MeterEstimate: ...
def analyze_bpm(
    samples: _FloatSamples,
    sample_rate: int,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    n_fft: int = 2048,
    hop_length: int = 512,
    max_candidates: int = 5,
) -> BpmAnalysisResult:
    """``n_fft`` is the analysis window length in samples at 22050 Hz, rescaled to the input
    sample rate; ``hop_length`` is in samples of the input buffer."""

def analyze_impulse_response(
    samples: _FloatSamples,
    sample_rate: int,
    n_octave_bands: int = 6,
    min_decay_db: float = 30.0,
) -> AcousticResult: ...
def detect_acoustic(
    samples: _FloatSamples,
    sample_rate: int,
    n_octave_bands: int = 6,
    n_third_octave_subbands: int = 24,
    min_decay_db: float = 30.0,
    noise_floor_margin_db: float = 10.0,
) -> AcousticResult: ...
def synthesize_rir(
    length_m: float = 7.0,
    width_m: float = 5.0,
    height_m: float = 3.0,
    *,
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    sample_rate: int,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,  # None = 1; 0 is a seed
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,  # None = 5 ms
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,  # None = 20 degC
    air_humidity_percent: float | None = None,  # None = 50 %
) -> RirResult: ...
def estimate_room(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    aspect_hint_lw: float = 1.0,
    aspect_hint_lh: float = 1.0,
    reference_absorption: float | None = None,
    prefer_eyring: bool = True,
    n_octave_bands: int | None = None,
    mode: AcousticModeName = "auto",
    min_decay_db: float | None = None,
    noise_floor_margin_db: float | None = None,
) -> RoomEstimate: ...
def room_geometry_from_estimate(
    estimate: RoomEstimate,
    *,
    source: Sequence[float] | None = None,
    listener: Sequence[float] | None = None,
) -> RoomGeometry: ...
def room_morph(
    samples: _FloatSamples,
    sample_rate: int,
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,  # None = 1; 0 is a seed
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,  # None = 5 ms
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,  # None = 20 degC
    air_humidity_percent: float | None = None,  # None = 50 %
) -> RoomMorphResult: ...
def room_morph_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    receiver_spacing_m: float | None = None,  # None = 0.5 m
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,  # None = 1; 0 is a seed
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,  # None = 5 ms
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,  # None = 20 degC
    air_humidity_percent: float | None = None,  # None = 50 %
) -> RoomMorphStereoResult: ...
def analyze_rhythm(
    samples: _FloatSamples,
    sample_rate: int,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> RhythmResult:
    """``n_fft`` is the analysis window length in samples at 22050 Hz, rescaled to the input
    sample rate; ``hop_length`` is in samples of the input buffer. ``start_bpm`` is a finite,
    positive BPM prior that seeds beat tracking if the tempo search finds none. The result's
    ``bpm`` is ``None`` when no tempo is measured."""

def analyze_dynamics(
    samples: _FloatSamples,
    sample_rate: int,
    window_sec: float = 0.4,
    hop_length: int = 512,
    compression_threshold_db: float = 6.0,
) -> DynamicsResult:
    """``hop_length`` is the hop in samples of the caller's buffer, at the sample rate."""

def analyze_timbre(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 13,
    window_sec: float = 0.5,
) -> TimbreResult:
    """``n_fft`` is the analysis window length in samples at 22050 Hz, rescaled to the input
    sample rate; ``hop_length`` is in samples of the input buffer."""

def detect_chords(
    samples: _FloatSamples,
    sample_rate: int,
    min_duration_sec: float = 0.3,
    smoothing_window_sec: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = True,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = True,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    key_root: PitchClass = PitchClass.C,
    key_mode: KeyMode = "major",
    detect_inversions: bool = False,
    chroma_method: ChordChromaMethod = "cqt",
    tuning_cents: float | str = "auto",
) -> ChordAnalysisResult:
    """``hop_length`` is the hop in samples of the caller's buffer, at the sample rate."""

def chord_functional_analysis(
    samples: _FloatSamples,
    sample_rate: int,
    key_root: PitchClass,
    key_mode: KeyMode = "major",
    min_duration_sec: float = 0.3,
    smoothing_window_sec: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = True,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = True,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    detect_inversions: bool = False,
    chroma_method: ChordChromaMethod = "cqt",
    tuning_cents: float | str = "auto",
) -> list[FunctionalChord]: ...
def chord_functions(
    chords: ChordAnalysisResult | Sequence[Chord],
    key: Key | Mapping[str, object] | tuple[PitchClass, KeyMode],
) -> list[FunctionalChord]: ...
def analyze_sections(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    min_section_sec: float = 4.0,
    threshold: float | None = None,
) -> SectionAnalysis:
    """``hop_length`` is the hop in samples of the analysed signal: the input resampled to
    22050 Hz when its rate is above 22050 Hz, the input itself otherwise."""

def detect_boundaries(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    kernel_size: int = 64,
    threshold: float = 0.3,
    absolute_threshold: float = 0.005,
    n_mfcc: int = 13,
    n_chroma: int = 12,
    peak_distance_sec: float = 2.0,
    reference_window_sec: float = 60.0,
    use_mfcc: bool = True,
    use_chroma: bool = True,
) -> BoundaryResult:
    """Detect structural boundaries and return the novelty curve behind them.

    ``reference_window_sec`` is a one-sided span in seconds (default 60) for the local
    reference the relative ``threshold`` is measured against, a lone dominant event
    set aside; 0 disables that threshold. Boundaries with ``strength`` below ``threshold``
    can still be
    returned, because the gate is local while ``strength`` ranks within the track.
    ``hop_length`` is the hop in samples of the analysed signal: the input resampled to
    22050 Hz when its rate is above 22050 Hz, the input itself otherwise.
    """
    ...

def analyze_melody(
    samples: _FloatSamples,
    sample_rate: int,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    frame_length: int = 2048,
    hop_length: int = 256,
    threshold: float = 0.1,
    use_pyin: bool = False,
    center: bool = True,
) -> MelodyResult: ...
def cqt(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    filter_scale: float = 1.0,
) -> CqtResult: ...
def pseudo_cqt(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    filter_scale: float = 1.0,
) -> CqtResult: ...
def hybrid_cqt(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    filter_scale: float = 1.0,
) -> CqtResult: ...
def vqt(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    gamma: float | None = None,
    filter_scale: float = 1.0,
) -> CqtResult: ...
def version() -> str: ...
def capabilities() -> Capabilities: ...
def system_info() -> dict[str, Any]: ...
def hpss(
    samples: _FloatSamples,
    sample_rate: int,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = 2048,
    hop_length: int | None = None,
    hard_mask: bool = False,
    with_residual: bool = False,
) -> HpssResult: ...
def harmonic(samples: _FloatSamples, sample_rate: int) -> np.ndarray[Any, Any]: ...
def percussive(samples: _FloatSamples, sample_rate: int) -> np.ndarray[Any, Any]: ...
def time_stretch(
    samples: _FloatSamples,
    sample_rate: int,
    speed_ratio: float,
    n_fft: int = 2048,
    hop_length: int | None = None,
) -> np.ndarray[Any, Any]: ...
def pitch_shift(
    samples: _FloatSamples,
    sample_rate: int,
    pitch_semitones: float,
    n_fft: int = 2048,
    hop_length: int | None = None,
) -> np.ndarray[Any, Any]: ...
def pitch_correct_to_midi(
    samples: _FloatSamples,
    sample_rate: int,
    current_midi: float,
    target_midi: float,
) -> np.ndarray[Any, Any]: ...
def pitch_correct_timevarying(
    samples: _FloatSamples,
    sample_rate: int,
    f0_hz: _FloatSamples,
    hop_length: int = 512,
    *,
    target_mode: PitchTargetMode = "midi",
    target_midi: float = 69.0,
    scale_root: int = 0,
    scale_mode_mask: int | KeyMode | None = None,
    scale_reference_midi: float | None = None,
    retune_amount: float | None = None,
    max_correction_semitones: float | None = None,
    retune_speed_ms: float | None = None,
    vibrato_threshold_cents: float | None = None,
    voiced: IntSamples | None = None,
    voiced_prob: _FloatSamples | None = None,
) -> np.ndarray[Any, Any]: ...
def auto_tune(
    samples: _FloatSamples,
    sample_rate: int,
    key: str | Key | Mapping[str, object] | tuple[PitchClass, KeyMode] = "detect",
    *,
    retune_amount: float | None = None,
    retune_speed_ms: float | None = None,
    vibrato_threshold_cents: float | None = None,
    max_correction_semitones: float | None = None,
    scale_reference_midi: float | None = None,
) -> AutoTuneResult: ...
def note_stretch(
    samples: _FloatSamples,
    sample_rate: int,
    onset_sample: int | None = None,
    offset_sample: int | None = None,
    stretch_ratio: float = 1.0,
    *,
    onset_sec: float | None = None,
    offset_sec: float | None = None,
) -> np.ndarray[Any, Any]: ...
def note_move(
    samples: _FloatSamples,
    sample_rate: int,
    onset_sample: int | None = None,
    offset_sample: int | None = None,
    target_onset_sample: int | None = None,
    *,
    onset_sec: float | None = None,
    offset_sec: float | None = None,
    target_onset_sec: float | None = None,
) -> np.ndarray[Any, Any]: ...
def voice_change(
    samples: _FloatSamples,
    sample_rate: int,
    pitch_semitones: float = 0.0,
    formant_factor: float = 1.0,
    formant_mode: FormantMode = "relative",
) -> np.ndarray[Any, Any]: ...
def extract_notes(
    samples: _FloatSamples,
    sample_rate: int,
    f0_hz: _FloatSamples,
    frame_rate: float,
    *,
    voiced: IntSamples | None = None,
    voiced_prob: _FloatSamples | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]: ...
def render_notes(
    samples: _FloatSamples,
    sample_rate: int,
    notes: Sequence[NoteObject | NoteObjectInput],
    *,
    f0_hz: _FloatSamples | None = None,
    voiced: IntSamples | None = None,
    frame_rate: float | None = None,
    fade_ms: float | None = None,
    vibrato_cutoff_hz: float | None = None,
) -> np.ndarray[Any, Any]: ...
def transcribe(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    tempo_bpm: float | None = None,
    polyphonic: bool = False,
    reference_hz: float | str | None = None,
    f0_min_hz: float | None = None,
    f0_max_hz: float | None = None,
    min_note_ms: float | None = None,
    segmentation_threshold_cents: float | None = None,
    velocity_floor_db: float | None = None,
    fixed_velocity: int | None = None,
    group: int = 0,
    channel: int = 0,
    max_polyphony: int | None = None,
    min_frame_peak_ratio: float | None = None,
    min_ridge_peak_ratio: float | None = None,
    reattack_ratio: float | None = None,
    min_note_division: int | None = None,
    monophonic_model: Literal["segments", "singing"] | None = None,
) -> TranscribeResult: ...
def extract_percussive_events(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    onset_wait: int | None = None,
    onset_delta: float | None = None,
    max_event_ms: float | None = None,
    min_percussive_ratio: float | None = None,
) -> list[PercussiveEvent]: ...
def render_percussive_events(
    samples: _FloatSamples,
    sample_rate: int,
    events: Sequence[PercussiveEvent | PercussiveEventInput],
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    fade_ms: float | None = None,
) -> np.ndarray[Any, Any]: ...
def decompose_note_pitch(
    f0_hz: _FloatSamples,
    frame_rate: float,
    median_hz: float | None,
    *,
    voiced: IntSamples | None = None,
    vibrato_cutoff_hz: float | None = None,
) -> PitchDecompositionResult: ...
def split_note(
    samples: _FloatSamples,
    sample_rate: int,
    f0_hz: _FloatSamples,
    frame_rate: float,
    notes: Sequence[NoteObject | NoteSetEntry],
    index: int,
    frame: int,
    *,
    voiced: IntSamples | None = None,
    voiced_prob: _FloatSamples | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]: ...
def merge_notes(
    samples: _FloatSamples,
    sample_rate: int,
    f0_hz: _FloatSamples,
    frame_rate: float,
    notes: Sequence[NoteObject | NoteSetEntry],
    first: int,
    last: int,
    *,
    voiced: IntSamples | None = None,
    voiced_prob: _FloatSamples | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]: ...
def note_targets_from_smf(
    data: bytes | bytearray,
    *,
    track_index: int = 0,
) -> list[NoteTarget]: ...
def assign_note_targets(
    notes: Sequence[NoteObject],
    sample_rate: int,
    targets: Sequence[NoteTarget],
    *,
    unmatched_policy: str = "leave",
    min_overlap_ratio: float | None = None,
    max_correction_semitones: float | None = None,
) -> AssignNoteTargetsResult: ...
def spectral_edit(
    samples: _FloatSamples,
    sample_rate: int,
    ops: Sequence[SpectralRegionOp],
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    window: WindowType = "hann",
    heal_radius_frames: int | None = None,
) -> npt.NDArray[np.float32]: ...

RealtimeVoiceChangerPreset = dict[str, Any]

def realtime_voice_changer_render(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    config: VoicePresetId
    | RealtimeVoiceChangerPreset
    | RealtimeVoiceChangerConfig
    | None = "neutralMonitor",
    num_channels: int = 1,
) -> np.ndarray:
    """Give exactly one of target_onset_sample or target_onset_sec."""
    ...
def realtime_voice_changer_preset_names() -> list[str]: ...
def realtime_voice_changer_preset_config(preset: VoicePresetId) -> RealtimeVoiceChangerConfig: ...
def voice_character_preset_id(ordinal: int) -> str | None: ...
def realtime_voice_changer_preset_json(name: str) -> str: ...
def validate_realtime_voice_changer_preset_json(document: str) -> dict[str, object]: ...

NormalizeMode: TypeAlias = Literal["peak", "rms"]

def normalize(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    mode: NormalizeMode = "peak",
    target_db: float | None = None,
) -> np.ndarray[Any, Any]: ...
def fade(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    fade_in_sec: float = 0.0,
    fade_out_sec: float = 0.0,
) -> np.ndarray[Any, Any]: ...

BiquadFilterType: TypeAlias = Literal["highPass", "lowPass", "bandPass", "notch"]

def biquad_filter(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    type: BiquadFilterType,  # noqa: A002
    order: int = 2,
    zero_phase: bool = False,
    cutoff_hz: float | None = None,
    center_hz: float | None = None,
    bandwidth_hz: float | None = None,
) -> np.ndarray[Any, Any]: ...
def normalize_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    mode: NormalizeMode = "peak",
    target_db: float | None = None,
) -> NormalizeStereoResult: ...
def mastering(
    samples: _FloatSamples,
    sample_rate: int,
    target_lufs: float = -14.0,
    ceiling_dbtp: float = -1.0,
    true_peak_oversample: int | None = None,
    release_ms: float | None = None,
    apply_gain_at_input_rate: bool = False,
) -> MasteringResult: ...
def mastering_assistant_suggest(
    samples: _FloatSamples,
    sample_rate: int,
    params: MasteringAssistantParams | None = None,
) -> MasteringAssistantResult: ...
def mastering_audio_profile(
    samples: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
) -> MasteringAudioProfile:
    """``nFft`` and ``hopLength`` in ``params`` are the window length and hop in samples at
    48000 Hz, rescaled to the input sample rate."""

def mastering_assistant_suggest_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    params: MasteringAssistantParams | None = None,
) -> MasteringAssistantResult: ...
def mastering_audio_profile_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
) -> MasteringAudioProfile:
    """``nFft`` and ``hopLength`` in ``params`` are the window length and hop in samples at
    48000 Hz, rescaled to the input sample rate."""

def mastering_processor_names() -> list[SoloProcessor]: ...
def mastering_pair_processor_names() -> list[_PairProcessor]: ...
def mastering_pair_analysis_names() -> list[_PairAnalysis]: ...
def mastering_stereo_analysis_names() -> list[_StereoAnalysis]: ...
def mastering_apply_processor(
    processor: SoloProcessor,
    samples: _FloatSamples,
    sample_rate: int,
    params: Mapping[str, float | int | bool | str] | None = None,
) -> MasteringResult: ...
def mastering_apply_processor_stereo(
    processor: SoloProcessor,
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    params: Mapping[str, float | int | bool | str] | None = None,
) -> MasteringStereoResult: ...
def mastering_preset_names() -> list[MasteringPreset]: ...
def mastering_preset_params(preset: MasteringPreset | str) -> MasteringChainConfig: ...
def mastering_chain_config_from_json(document: str) -> MasteringChainConfig: ...
def mastering_platform_names() -> list[str]: ...
def mastering_insert_names() -> list[str]: ...
def mastering_insert_param_names(insert: str) -> list[str]: ...
def mastering_insert_param_info(
    insert: str, sample_rate: int | None = None
) -> list[MasteringInsertParamInfo]: ...
@overload
def mastering_insert_timing(
    insert: str,
    params: Mapping[str, float | bool | str | Iterable[float]] | None,
    sample_rate: int,
) -> MasteringInsertTiming: ...
@overload
def mastering_insert_timing(insert: str, *, sample_rate: int) -> MasteringInsertTiming: ...
def mastering_processor_catalog() -> list[MasteringProcessorCatalogEntry]: ...
def mastering_amp_preset_catalog() -> list[MasteringAmpPresetCatalogEntry]: ...
def capability_catalog() -> CapabilityCatalog: ...
def mixing_scene_preset_names() -> list[str]: ...
def mixing_scene_preset(name: str) -> MixSceneDocument: ...
def mix_stereo(
    strips: Sequence[tuple[_FloatSamples, _FloatSamples]],
    sample_rate: int,
    fader_db: Sequence[float] | None = None,
    pan: Sequence[float] | None = None,
    pan_mode: Sequence[PanMode] | PanMode = "balance",
    width: Sequence[float] | None = None,
    muted: Sequence[bool] | None = None,
    input_trim_db: Sequence[float] | None = None,
) -> MixResult:
    """Render a small stereo mixer scene from per-strip left/right buffers.

    ``sample_rate`` is an integer in ``[8000, 384000]``. A per-strip option may be
    shorter than ``strips`` (the remaining strips keep their defaults); one longer
    than ``strips`` raises ``SonareValueError`` naming it.
    """

def master_audio(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    preset: MasteringPreset | None = None,
    overrides: MasteringChainConfigInput | None = None,
    on_progress: _ProgressCallback | None = None,
    cancel: _CancelCallback | None = None,
) -> MasteringChainResult:
    """Master with a preset, an ``overrides`` chain document's params, or both.

    A list value is the path ``<path>.<index>``, so
    ``{"dynamics": {"multibandComp": {"crossover": {"cutoffsHz": [200, 2000, 8000]},
    "bands": [{"ratio": 2.0}, ...]}}}`` carries an arbitrary-band multiband
    compressor: ``cutoffsHz`` replaces the cutoff list (band count is its length
    plus one) and a band field left out keeps the base or default value. Content
    refusals are SonareValueError naming ``overrides.<path>``.
    """

def master_audio_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    preset: MasteringPreset | None = None,
    overrides: MasteringChainConfigInput | None = None,
    on_progress: _ProgressCallback | None = None,
    cancel: _CancelCallback | None = None,
) -> MasteringChainStereoResult: ...
def streaming_loudness_gain(
    samples: _FloatSamples,
    sample_rate: int,
    config: MasteringChainConfigInput | None = None,
) -> StreamingLoudnessGain: ...
def streaming_loudness_gain_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    config: MasteringChainConfigInput | None = None,
) -> StreamingLoudnessGain: ...
def mastering_apply_pair_processor(
    processor: _PairProcessor,
    source: _FloatSamples,
    reference: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MasteringResult: ...
def mastering_apply_pair_processor_stereo(
    processor: _StereoPairProcessor,
    source_left: _FloatSamples,
    source_right: _FloatSamples,
    reference_left: _FloatSamples,
    reference_right: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MasteringStereoResult: ...
@overload
def mastering_analyze_pair(
    analysis: Literal["match.referenceLoudness"],
    source: _FloatSamples | PlanarChannels,
    reference: _FloatSamples | PlanarChannels,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MatchReferenceLoudnessResult: ...
@overload
def mastering_analyze_pair(
    analysis: Literal["match.tonalBalance", "match.tonalBalanceLogBands"],
    source: _FloatSamples | PlanarChannels,
    reference: _FloatSamples | PlanarChannels,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MatchTonalBalanceResult: ...
@overload
def mastering_analyze_pair(
    analysis: Literal["match.matchEqCurve"],
    source: _FloatSamples | PlanarChannels,
    reference: _FloatSamples | PlanarChannels,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MatchEqCurveResult: ...
@overload
def mastering_analyze_pair(
    analysis: Literal["match.estimateReferenceDelaySamples"],
    source: _FloatSamples | PlanarChannels,
    reference: _FloatSamples | PlanarChannels,
    sample_rate: int,
    params: MasteringParams | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MatchEstimateReferenceDelaySamplesResult: ...
def mastering_ab_match_loudness(
    source: _FloatSamples,
    reference: _FloatSamples,
    sample_rate: int,
    *,
    reference_sample_rate: int | None = None,
) -> LoudnessMatchResult: ...
def mastering_ab_match_loudness_stereo(
    source_left: _FloatSamples,
    source_right: _FloatSamples,
    reference_left: _FloatSamples,
    reference_right: _FloatSamples,
    sample_rate: int,
    *,
    reference_sample_rate: int | None = None,
) -> LoudnessMatchStereoResult: ...
@overload
def mastering_analyze_stereo(
    analysis: Literal["stereo.monoCompatCheck"],
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
) -> StereoMonoCompatCheckResult: ...
@overload
def mastering_analyze_stereo(
    analysis: Literal["stereo.monoCompatCheckLogBands"],
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    params: MasteringParams | None = None,
) -> StereoMonoCompatCheckLogBandsResult: ...
def mastering_streaming_preview(
    samples: _FloatSamples,
    sample_rate: int,
    platforms: Sequence[StreamingPlatform] | None = None,
) -> MasteringStreamingPreviewResult: ...
def mastering_streaming_preview_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    platforms: Sequence[StreamingPlatform] | None = None,
) -> MasteringStreamingPreviewResult: ...
def trim(
    samples: _FloatSamples,
    sample_rate: int,
    threshold_db: float = -60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray[Any, Any]: ...
def stft(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    *,
    db: bool = False,
    phase: bool = False,
) -> StftResult: ...
def mel_spectrogram(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    fmin_hz: float = 0.0,
    fmax_hz: float | None = None,
    htk: bool = False,
) -> MelSpectrogramResult: ...
def mel_delta(features: npt.ArrayLike, width: int = 9) -> np.ndarray[Any, Any]: ...
def mfcc(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 20,
    fmin: float = 0.0,
    fmax: float | None = None,
    htk: bool = False,
    lifter: float = 0.0,
) -> MfccResult: ...
def chroma(
    samples: _FloatSamples, sample_rate: int, n_fft: int = 2048, hop_length: int = 512
) -> ChromaResult: ...
def chroma_cens(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult: ...
def chroma_cqt(
    samples: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    n_chroma: int = 12,
    bins_per_octave: int = 36,
) -> ChromaResult: ...
def bass_chroma(
    samples: _FloatSamples, sample_rate: int, hop_length: int = 512, n_chroma: int = 12
) -> ChromaResult: ...
def spectral_centroid(
    samples: _FloatSamples, sr: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> np.ndarray[Any, Any]: ...
def tone(
    frequency: float = 440.0,
    sr: int = 22050,
    duration: float = 1.0,
    phi: float = 0.0,
    amplitude: float = 1.0,
) -> np.ndarray[Any, Any]: ...
def chirp(
    fmin: float = 440.0,
    fmax: float = 880.0,
    sr: int = 22050,
    duration: float = 1.0,
    linear: bool = False,
) -> np.ndarray[Any, Any]: ...
def clicks(
    times: _FloatSamples,
    sr: int = 22050,
    length: int | None = None,
    click_freq: float = 1000.0,
    click_duration: float = 0.1,
) -> np.ndarray[Any, Any]: ...
def spectral_bandwidth(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    p: float = 2.0,
) -> np.ndarray[Any, Any]: ...
def spectral_flux(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    lag: int = 1,
) -> np.ndarray[Any, Any]: ...
def onset_backtrack(events: IntSamples, energy: _FloatSamples) -> np.ndarray[Any, Any]: ...
def spectral_rolloff(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    roll_percent: float = 0.85,
) -> np.ndarray[Any, Any]: ...
def spectral_flatness(
    samples: _FloatSamples, sr: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> np.ndarray[Any, Any]: ...
def zero_crossing_rate(
    samples: _FloatSamples, sr: int = 22050, frame_length: int = 2048, hop_length: int = 512
) -> np.ndarray[Any, Any]: ...
def rms_energy(
    samples: _FloatSamples, sample_rate: int, frame_length: int = 2048, hop_length: int = 512
) -> np.ndarray[Any, Any]: ...
def pitch_yin(
    samples: _FloatSamples,
    sample_rate: int,
    frame_length: int = 2048,
    hop_length: int = 512,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    threshold: float = 0.1,
) -> PitchResult: ...
def pitch_pyin(
    samples: _FloatSamples,
    sample_rate: int,
    frame_length: int = 2048,
    hop_length: int = 512,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult: ...
def piptrack(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 150.0,
    fmax: float = 4000.0,
    threshold: float = 0.1,
) -> PiptrackResult: ...
def reassigned_spectrogram(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    ref_power: float = 1e-6,
    fill_nan: bool = False,
) -> ReassignedSpectrogramResult: ...
def segment_cross_similarity(
    x: npt.ArrayLike,
    y: npt.ArrayLike,
    k: int | None = None,
    metric: Literal["cosine", "euclidean"] = "cosine",
    mode: Literal["connectivity", "affinity"] = "connectivity",
) -> np.ndarray[Any, Any]: ...
def segment_recurrence_matrix(
    data: npt.ArrayLike,
    k: int | None = None,
    width: int = 1,
    sym: bool = False,
    metric: Literal["cosine", "euclidean"] = "euclidean",
    mode: Literal["connectivity", "affinity"] = "connectivity",
) -> np.ndarray[Any, Any]: ...
def segment_recurrence_to_lag(
    recurrence: npt.ArrayLike, pad: bool = True
) -> np.ndarray[Any, Any]: ...
def segment_lag_to_recurrence(lag: npt.ArrayLike) -> np.ndarray[Any, Any]: ...
def segment_subsegment(
    data: npt.ArrayLike,
    boundaries: IntSamples,
    n_segments: int = 4,
) -> np.ndarray[Any, Any]: ...
def segment_agglomerative(
    data: npt.ArrayLike,
    k: int,
    linkage: Literal["average", "single", "complete", "ward"] = "average",
) -> np.ndarray[Any, Any]: ...
def segment_path_enhance(
    recurrence: npt.ArrayLike,
    win: int,
    max_ratio: int = 2,
    min_ratio: int | None = None,
    n_filters: int = 7,
) -> np.ndarray[Any, Any]: ...
def note_segments(
    f0_hz: _FloatSamples,
    frame_rate: float,
    *,
    voiced: IntSamples | None = None,
    voiced_prob: _FloatSamples | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteSegment]: ...
def spectral_contrast(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_bands: int = 6,
    fmin: float = 200.0,
    quantile: float = 0.02,
) -> np.ndarray[Any, Any]: ...
def poly_features(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    order: int = 1,
) -> np.ndarray[Any, Any]: ...
def zero_crossings(
    samples: _FloatSamples,
    threshold: float = 1e-10,
    ref_magnitude: bool = False,
    pad: bool = True,
    zero_pos: bool = True,
) -> np.ndarray[Any, Any]: ...
def pitch_tuning(
    frequencies: _FloatSamples,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float: ...
def estimate_tuning(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float: ...
def tuning_to_reference_hz(tuning_cents: float, a4_hz: float = 440.0) -> float: ...
def reference_hz_to_tuning(reference_hz: float, a4_hz: float = 440.0) -> float: ...
def lufs_interleaved(
    samples: _FloatSamples,
    num_channels: int,
    sample_rate: int,
) -> LufsResult: ...
def lufs_series_interleaved(
    samples: _FloatSamples,
    num_channels: int,
    sample_rate: int,
) -> LufsSeriesResult: ...
def ebur128_loudness_range_lu(
    samples: _FloatSamples,
    sample_rate: int,
) -> float | None: ...
def decompose(
    s: _FloatSamples,
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int | None = None,
    beta: float = 2.0,
    init: NmfInit | None = None,
) -> DecomposeResult: ...
def decompose_stems(
    samples: _FloatSamples,
    sample_rate: int,
    n_components: int | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int | None = None,
    beta: float | None = None,
    init: NmfInit | None = None,
    mask_power: float | None = None,
) -> DecomposeStemsResult: ...
def decompose_stems_linked(
    channels: PlanarChannels,
    sample_rate: int,
    n_components: int | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int | None = None,
    beta: float | None = None,
    init: NmfInit | None = None,
    mask_power: float | None = None,
) -> DecomposeStemsLinkedResult: ...
def nn_filter(
    s: _FloatSamples,
    n_features: int,
    n_frames: int,
    aggregate: NnFilterAggregate | None = None,
    k: int | None = None,
    width: int | None = None,
) -> np.ndarray[Any, Any]: ...
def remix(
    samples: _FloatSamples,
    intervals: npt.ArrayLike,
    sr: int = 22050,
    align_zeros: bool = True,
) -> np.ndarray[Any, Any]: ...
def remix_aligned_intervals(
    samples: _FloatSamples,
    sample_rate: int,
    intervals: npt.ArrayLike,
    align_zeros: bool = True,
) -> np.ndarray[Any, Any]: ...
def phase_vocoder(
    samples: _FloatSamples,
    sample_rate: int,
    speed_ratio: float,
    n_fft: int = 2048,
    hop_length: int | None = None,
) -> np.ndarray[Any, Any]: ...
def hz_to_mel(hz: float) -> float: ...
def mel_to_hz(mel: float) -> float: ...
def mel_frequencies(
    n_mels: int = 128,
    fmin: float = 0.0,
    fmax: float = 11025.0,
    htk: bool = False,
) -> np.ndarray[Any, Any]: ...
def mel_filterbank(
    sample_rate: int,
    n_fft: int,
    *,
    n_mels: int = 128,
    fmin_hz: float = 0.0,
    fmax_hz: float | None = None,
    htk: bool = False,
    norm: MelNorm = "slaney",
) -> np.ndarray[Any, Any]: ...
def hz_to_midi(hz: float) -> float: ...
def midi_to_hz(midi: float) -> float: ...
def hz_to_note(hz: float) -> str:
    """Raises ``SonareValueError`` when ``hz`` is not finite and greater than 0."""

def note_to_hz(note: str) -> float:
    """Raises ``SonareValueError`` when ``note`` does not parse or its octave is out of range."""

def frames_to_time(frames: int, sr: int = 22050, hop_length: int = 512) -> float: ...
def time_to_frames(time_sec: float, sr: int = 22050, hop_length: int = 512) -> int: ...
def frames_to_samples(frames: int, hop_length: int = 512, n_fft: int | None = None) -> int: ...
def samples_to_frames(samples: int, hop_length: int = 512, n_fft: int | None = None) -> int: ...
def power_to_db(
    values: _FloatSamples, ref: float = 1.0, amin: float = 1e-10, top_db: float = 80.0
) -> np.ndarray[Any, Any]: ...
def amplitude_to_db(
    values: _FloatSamples, ref: float = 1.0, amin: float = 1e-5, top_db: float = 80.0
) -> np.ndarray[Any, Any]: ...
def db_to_power(values: _FloatSamples, ref: float = 1.0) -> np.ndarray[Any, Any]: ...
def db_to_amplitude(values: _FloatSamples, ref: float = 1.0) -> np.ndarray[Any, Any]: ...
def preemphasis(
    samples: _FloatSamples, coef: float = 0.97, zi: float | None = None
) -> np.ndarray[Any, Any]: ...
def deemphasis(
    samples: _FloatSamples, coef: float = 0.97, zi: float | None = None
) -> np.ndarray[Any, Any]: ...
def trim_silence(
    samples: _FloatSamples, top_db: float = 60.0, frame_length: int = 2048, hop_length: int = 512
) -> TrimResult: ...
def split_silence(
    samples: _FloatSamples, top_db: float = 60.0, frame_length: int = 2048, hop_length: int = 512
) -> np.ndarray[Any, Any]: ...
def split_silence_common(
    signals: Sequence[_FloatSamples] | np.ndarray,
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray[Any, Any]: ...
def split_silence_common_with_report(
    signals: Sequence[_FloatSamples] | np.ndarray,
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> SplitSilenceCommonWithReportResult: ...
def frame_signal(
    samples: _FloatSamples, frame_length: int, hop_length: int
) -> np.ndarray[Any, Any]: ...
def pad_center(
    values: _FloatSamples, target_size: int, pad_value: float = 0.0
) -> np.ndarray[Any, Any]: ...
def fix_length(
    values: _FloatSamples, target_size: int, pad_value: float = 0.0
) -> np.ndarray[Any, Any]: ...
def fix_frames(
    frames: IntSamples, x_min: int = 0, x_max: int | None = None, pad: bool = True
) -> np.ndarray[Any, Any]: ...
def peak_pick(
    values: _FloatSamples,
    pre_max: int,
    post_max: int,
    pre_avg: int,
    post_avg: int,
    delta: float,
    wait: int,
) -> np.ndarray[Any, Any]: ...
def vector_normalize(
    values: _FloatSamples, norm: NormType = "max", threshold: float = 0.0
) -> np.ndarray[Any, Any]: ...
def pcen(
    values: npt.ArrayLike,
    sr: int = 22050,
    hop_length: int = 512,
    time_constant: float = 0.4,
    gain: float = 0.98,
    bias: float = 2.0,
    power: float = 0.5,
    eps: float = 1e-6,
) -> np.ndarray[Any, Any]: ...
def tonnetz(chromagram: npt.ArrayLike) -> np.ndarray[Any, Any]: ...
def tempogram(
    onset_envelope: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
    mode: TempogramMode = "autocorrelation",
) -> np.ndarray[Any, Any]: ...
def cyclic_tempogram(
    onset_envelope: _FloatSamples,
    sample_rate: int,
    hop_length: int = 512,
    win_length: int = 384,
    bpm_min: float = 60.0,
    n_bins: int = 60,
) -> np.ndarray[Any, Any]: ...
def plp(
    onset_envelope: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    tempo_min: float | None = None,
    tempo_max: float | None = None,
    win_length: int = 384,
) -> np.ndarray[Any, Any]: ...
def onset_strength(
    samples: _FloatSamples,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
) -> np.ndarray[Any, Any]: ...
def fourier_tempogram(
    onset_envelope: _FloatSamples,
    sr: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
) -> np.ndarray[Any, Any]: ...
def tempogram_ratio(
    tempogram: npt.ArrayLike,
    sr: int = 22050,
    hop_length: int = 512,
    factors: _FloatSamples | None = None,
) -> np.ndarray[Any, Any]: ...
def nnls_chroma(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    enable_stft_blend: bool = True,
    stft_blend_weight: float = 0.55,
    stft_blend_n_fft: int = 4096,
    hop_length: int = 512,
) -> np.ndarray[Any, Any]: ...
def onset_strength_multi(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_bands: int = 3,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_declick(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> DeclickResult: ...
def mastering_repair_declick_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> DeclickStereoResult: ...
def mastering_repair_denoise_classical(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    mode: DenoiseClassicalMode = "logMmse",
    noise_estimator: DenoiseClassicalNoiseEstimator = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> DenoiseResult: ...
def mastering_repair_denoise_classical_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    mode: DenoiseClassicalMode = "logMmse",
    noise_estimator: DenoiseClassicalNoiseEstimator = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> DenoiseStereoResult: ...
def mastering_repair_denoise_classical_linked(
    channels: PlanarChannels,
    sample_rate: int,
    *,
    mode: DenoiseClassicalMode = "logMmse",
    noise_estimator: DenoiseClassicalNoiseEstimator = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> DenoiseLinkedResult: ...
def mastering_repair_analyze(
    channels: PlanarChannels,
    sample_rate: int,
    *,
    prefer_streaming_safe: bool = True,
) -> MasteringRepairAnalysis: ...
def mastering_repair_apply(
    channels: PlanarChannels,
    sample_rate: int,
    *,
    stages: Sequence[RepairStage],
    on_progress: Callable[[float, str], None] | None = None,
    cancel: Callable[[], bool] | None = None,
) -> RepairApplyResult: ...
def mastering_repair_declip(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    clip_threshold_db: float = ...,
    lpc_order: int = 36,
    iterations: int = 2,
) -> DeclipResult: ...
def mastering_repair_declip_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    clip_threshold_db: float = ...,
    lpc_order: int = 36,
    iterations: int = 2,
    link_channels: bool = False,
) -> DeclipStereoResult: ...
def mastering_repair_decrackle(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.4,
    mode: DecrackleMode = "median",
    levels: int = 4,
) -> DecrackleResult: ...
def mastering_repair_decrackle_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.4,
    mode: DecrackleMode = "median",
    levels: int = 4,
) -> DecrackleStereoResult: ...
def mastering_repair_dehum(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: DehumMode = "subtract",
) -> DehumResult: ...
def mastering_repair_dehum_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: DehumMode = "subtract",
) -> DehumStereoResult: ...
def mastering_repair_dereverb_classical(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbResult: ...
def mastering_repair_dereverb_classical_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbStereoResult: ...
def mastering_repair_dereverb_classical_linked(
    channels: PlanarChannels,
    sample_rate: int,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbLinkedResult: ...
def mastering_repair_dereverb_config_for_room(
    estimate: RoomEstimate,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbClassicalConfig: ...
def mastering_repair_detect_clicks(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> ClickDetection: ...
def mastering_repair_detect_noise_floor(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    mode: DenoiseClassicalMode = "logMmse",
    noise_estimator: DenoiseClassicalNoiseEstimator = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> NoiseDetection: ...
def mastering_repair_noise_band_bins(
    sample_rate: int,
    n_fft: int = 1024,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_detect_clipping(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    clip_threshold_db: float = ...,
    lpc_order: int = 36,
    iterations: int = 2,
) -> ClipDetection: ...
def mastering_repair_detect_crackle(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.4,
    mode: DecrackleMode = "median",
    levels: int = 4,
) -> CrackleDetection: ...
def mastering_repair_detect_hum(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: DehumMode = "subtract",
) -> HumDetection: ...
def mastering_repair_detect_reverb(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> ReverbDetection: ...
def mastering_repair_detect_trim_range(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -60.0,
    padding_samples: int | None = None,
    padding_sec: float | None = None,
    mode: TrimSilenceMode = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimRange: ...
def mastering_repair_detect_trim_range_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -60.0,
    padding_samples: int | None = None,
    padding_sec: float | None = None,
    mode: TrimSilenceMode = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimRange: ...
def mastering_repair_trim_silence(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -60.0,
    padding_samples: int | None = None,
    padding_sec: float | None = None,
    mode: TrimSilenceMode = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimSilenceResult: ...
def mastering_repair_trim_silence_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -60.0,
    padding_samples: int | None = None,
    padding_sec: float | None = None,
    mode: TrimSilenceMode = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimSilenceStereoResult: ...
def lufs(samples: _FloatSamples, sample_rate: int) -> LufsResult: ...
def momentary_lufs(samples: _FloatSamples, sample_rate: int) -> np.ndarray[Any, Any]: ...
def short_term_lufs(samples: _FloatSamples, sample_rate: int) -> np.ndarray[Any, Any]: ...
def metering_stereo_correlation(
    left: _FloatSamples, right: _FloatSamples, sample_rate: int
) -> float | None: ...
def metering_stereo_width(
    left: _FloatSamples, right: _FloatSamples, sample_rate: int
) -> float | None: ...
def metering_vectorscope(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    max_points: int | None = None,
) -> VectorscopeReport: ...
def metering_phase_scope(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
    max_points: int | None = None,
) -> PhaseScopeReport: ...
def metering_spectrum(
    samples: _FloatSamples,
    sample_rate: int,
    n_fft: int | None = None,
    apply_octave_smoothing: bool = False,
    octave_fraction: int | None = None,
    db_ref: float | None = None,
    db_amin: float | None = None,
) -> SpectrumReport: ...
def metering_spectrum_frame(
    samples: _FloatSamples,
    sample_rate: int,
    frame_offset: int = 0,
    n_fft: int | None = None,
    apply_octave_smoothing: bool = False,
    octave_fraction: int | None = None,
    db_ref: float | None = None,
    db_amin: float | None = None,
) -> SpectrumReport: ...
def waveform_peaks(
    samples: _FloatSamples,
    num_channels: int,
    *,
    samples_per_bucket: int = 512,
) -> WaveformPeaksReport: ...
def waveform_peak_pyramid(
    samples: _FloatSamples,
    num_channels: int,
    *,
    samples_per_bucket_levels: Sequence[int] = (512, 1024, 2048, 4096),
) -> list[WaveformPeaksReport]: ...
def metering_peak_db(samples: _FloatSamples, sample_rate: int) -> float: ...
def metering_rms_db(samples: _FloatSamples, sample_rate: int) -> float: ...
def metering_silence_ratio(
    samples: _FloatSamples,
    sample_rate: int,
    threshold_db: float = -45.0,
    frame_length: int = 1024,
    hop_length: int = 256,
) -> float: ...
def metering_crest_factor_db(samples: _FloatSamples, sample_rate: int) -> float | None: ...
def metering_crest_factor_db_stereo(
    left: _FloatSamples,
    right: _FloatSamples,
    sample_rate: int,
) -> float | None: ...
def metering_dc_offset(samples: _FloatSamples, sample_rate: int) -> float: ...
def metering_true_peak_dbtp(
    samples: _FloatSamples,
    sample_rate: int,
    oversample_factor: int | None = None,
) -> float: ...
def metering_detect_clipping(
    samples: _FloatSamples,
    sample_rate: int,
    threshold_db: float | None = None,
    min_region_samples: int | None = None,
) -> ClippingReport: ...
def metering_dynamic_range(
    samples: _FloatSamples,
    sample_rate: int,
    window_sec: float | None = None,
    hop_sec: float | None = None,
    low_percentile: float | None = None,
    high_percentile: float | None = None,
) -> DynamicRangeReport: ...
def scale_quantize_midi(
    root: int, mode_mask: int, midi: float, scale_reference_midi: float | None = None
) -> float: ...
def scale_correction_semitones(
    root: int, mode_mask: int, midi: float, scale_reference_midi: float | None = None
) -> float: ...
def scale_mask_for_mode(root: int, mode: KeyMode) -> int: ...
def scale_pitch_class_enabled(root: int, mode_mask: int, pitch_class: int) -> bool: ...
def mel_to_stft(
    mel: npt.ArrayLike,
    sr: int = 22050,
    n_fft: int = 2048,
    fmin: float = 0.0,
    fmax: float | None = None,
    htk: bool = False,
) -> np.ndarray[Any, Any]: ...
def cqt_to_audio(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    hop_length: int = 512,
    fmin_hz: float = 32.70319566257483,
    bins_per_octave: int = 12,
    n_iter: int = 32,
) -> np.ndarray[Any, Any]: ...
def vqt_to_audio(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    hop_length: int = 512,
    fmin_hz: float = 32.70319566257483,
    bins_per_octave: int = 12,
    gamma: float | None = None,
    n_iter: int = 32,
) -> np.ndarray[Any, Any]: ...
def mel_to_audio(
    mel: npt.ArrayLike,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int | None = None,
    fmin: float = 0.0,
    fmax: float | None = None,
    n_iter: int = 32,
    htk: bool = False,
) -> np.ndarray[Any, Any]: ...
def griffin_lim(
    magnitude: npt.ArrayLike,
    sample_rate: int,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int = 32,
    momentum: float = 0.99,
) -> np.ndarray[Any, Any]: ...
def istft(
    magnitude: npt.ArrayLike,
    phase: npt.ArrayLike,
    n_fft: int | None = None,
    hop_length: int | None = None,
    *,
    length_samples: int | None = None,
) -> np.ndarray[Any, Any]: ...
def mfcc_to_mel(
    coefficients: npt.ArrayLike,
    n_mels: int = 128,
    lifter: float = 0.0,
) -> np.ndarray[Any, Any]: ...
def mfcc_to_audio(
    coefficients: npt.ArrayLike,
    n_mels: int = 128,
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int | None = None,
    fmin: float = 0.0,
    fmax: float | None = None,
    n_iter: int = 32,
    htk: bool = False,
    lifter: float = 0.0,
) -> np.ndarray[Any, Any]: ...
def mastering_dynamics_compressor(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: CompressorDetector = "rms",
    sidechain_high_pass_hz: float | None = None,  # 0 also disables the filter.
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
) -> DynamicsProcessorResult: ...
def mastering_dynamics_gate(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_high_pass_hz: float | None = None,
) -> DynamicsProcessorResult: ...
def mastering_dynamics_transient_shaper(
    samples: _FloatSamples,
    sample_rate: int,
    *,
    attack_gain_db: float = 3.0,
    sustain_gain_db: float = 0.0,
    fast_attack_ms: float = 0.0,
    fast_release_ms: float = 20.0,
    slow_attack_ms: float = 15.0,
    slow_release_ms: float = 200.0,
    sensitivity: float = 1.0,
    max_gain_db: float = 12.0,
    gain_smoothing_ms: float = 0.0,
    lookahead_ms: float = 0.0,
) -> DynamicsProcessorResult: ...
def resample(
    samples: _FloatSamples, sample_rate: int, target_sample_rate: int
) -> np.ndarray[Any, Any]: ...
