from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Any, Literal, TypeAlias

import numpy as np

from .types import (
    AcousticResult,
    AnalysisResult,
    AutomationCurve,
    BpmAnalysisResult,
    ChordAnalysisResult,
    ChromaResult,
    ClippingReport,
    CqtResult,
    DynamicRangeReport,
    DynamicsResult,
    EqSpectrumSnapshot,
    GoniometerPoint,
    HpssResult,
    InverseResult,
    Key,
    KeyCandidate,
    KeyProfile,
    LufsResult,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringResult,
    MasteringStereoResult,
    MelodyResult,
    MelSpectrogramResult,
    MeterTap,
    MfccResult,
    MixMeterSnapshot,
    MixResult,
    Mode,
    PanLaw,
    PhaseScopeReport,
    PitchClass,
    PitchResult,
    RhythmResult,
    SectionResult,
    SendTiming,
    SpectrumReport,
    StftResult,
    TimbreResult,
    VectorscopeReport,
)

TempogramMode: TypeAlias = Literal["autocorrelation", "auto", "ac", "cosine"]

FloatSamples: TypeAlias = Sequence[float] | list[float] | np.ndarray[Any, Any]
StripRef: TypeAlias = int | str

def engine_abi_version() -> int: ...

AutomationCurveArg: TypeAlias = AutomationCurve | str | int
IntSamples: TypeAlias = Sequence[int] | list[int]
MasteringParamValue: TypeAlias = float | int | bool
MasteringParams: TypeAlias = dict[str, MasteringParamValue]
ProgressCallback: TypeAlias = Callable[[float, str], None]

MasteringPreset: TypeAlias = Literal[
    "pop",
    "edm",
    "acoustic",
    "hipHop",
    "aiMusic",
    "speech",
    "streaming",
    "youtube",
    "broadcast",
    "podcast",
    "audiobook",
    "cinema",
    "jpop",
    "ambient",
    "lofi",
    "classical",
    "drumAndBass",
    "techno",
    "metal",
    "trap",
    "rnb",
    "jazz",
    "kpop",
    "trance",
    "gameOst",
]
SoloProcessor: TypeAlias = Literal[
    "dynamics.brickwallLimiter",
    "dynamics.compressor",
    "dynamics.deesser",
    "dynamics.expander",
    "dynamics.gate",
    "dynamics.limiter",
    "dynamics.parallelComp",
    "dynamics.sidechainRouter",
    "dynamics.duckingProcessor",
    "dynamics.transientShaper",
    "dynamics.upwardCompressor",
    "dynamics.upwardExpander",
    "dynamics.vocalRider",
    "eq.apiStyle",
    "eq.bandPass",
    "eq.cutFilter",
    "eq.dynamic",
    "eq.equalizer",
    "eq.graphic",
    "eq.linearPhase",
    "eq.midSide",
    "eq.minimumPhase",
    "eq.parametric",
    "eq.pultec",
    "eq.shelving",
    "eq.tilt",
    "final.bitDepth",
    "final.dither",
    "final.outputChain",
    "maximizer.adaptiveRelease",
    "maximizer.loudnessOptimize",
    "maximizer.maximizer",
    "maximizer.softKneeMax",
    "maximizer.truePeakLimiter",
    "multiband.compressor",
    "multiband.dynamicEq",
    "multiband.expander",
    "multiband.imager",
    "multiband.limiter",
    "multiband.saturation",
    "repair.declick",
    "repair.declip",
    "repair.decrackle",
    "repair.dehum",
    "repair.denoiseClassical",
    "repair.dereverbClassical",
    "repair.trimSilence",
    "saturation.bitcrusher",
    "saturation.exciter",
    "saturation.hardClipper",
    "saturation.multibandExciter",
    "saturation.softClipper",
    "saturation.tape",
    "saturation.transformer",
    "saturation.tube",
    "saturation.waveshaper",
    "spectral.airBand",
    "spectral.lowEndFocus",
    "spectral.presenceEnhancer",
    "spectral.spectralShaper",
    "stereo.autoPan",
    "stereo.haasEnhancer",
    "stereo.imager",
    "stereo.monoMaker",
    "stereo.phaseAlign",
    "stereo.stereoBalance",
]
PairProcessor: TypeAlias = Literal[
    "match.applyMatchEq",
    "match.alignReferenceToSource",
    "match.abSwitch",
    "match.abCrossfade",
]
PairAnalysis: TypeAlias = Literal[
    "match.referenceLoudness",
    "match.tonalBalance",
    "match.tonalBalanceLogBands",
    "match.matchEqCurve",
    "match.estimateReferenceDelaySamples",
]
StereoAnalysis: TypeAlias = Literal["stereo.monoCompatCheck", "stereo.monoCompatCheckLogBands"]

def detect_bpm(samples: FloatSamples, sample_rate: int = 22050) -> float: ...
def detect_key(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> Key: ...
def detect_key_candidates(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> list[KeyCandidate]: ...
def detect_beats(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def detect_downbeats(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def detect_onsets(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def analyze(samples: FloatSamples, sample_rate: int = 22050) -> AnalysisResult: ...
def analyze_bpm(
    samples: FloatSamples,
    sample_rate: int = 22050,
    bpm_min: float = 30.0,
    bpm_max: float = 300.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
    max_candidates: int = 5,
) -> BpmAnalysisResult: ...
def analyze_impulse_response(
    samples: FloatSamples, sample_rate: int = 48000, n_octave_bands: int = 6
) -> AcousticResult: ...
def detect_acoustic(
    samples: FloatSamples,
    sample_rate: int = 48000,
    n_octave_bands: int = 6,
    n_third_octave_subbands: int = 24,
    min_decay_db: float = 30.0,
    noise_floor_margin_db: float = 10.0,
) -> AcousticResult: ...
def analyze_rhythm(
    samples: FloatSamples,
    sample_rate: int = 22050,
    bpm_min: float = 60.0,
    bpm_max: float = 200.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> RhythmResult: ...
def analyze_dynamics(
    samples: FloatSamples,
    sample_rate: int = 22050,
    window_sec: float = 0.4,
    hop_length: int = 512,
    compression_threshold: float = 6.0,
) -> DynamicsResult: ...
def analyze_timbre(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 13,
    window_sec: float = 0.5,
) -> TimbreResult: ...
def detect_chords(
    samples: FloatSamples,
    sample_rate: int = 22050,
    min_duration: float = 0.3,
    smoothing_window: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = False,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = False,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    key_root: PitchClass = PitchClass.C,
    key_mode: Mode = Mode.MAJOR,
    detect_inversions: bool = False,
    chroma_method: str = "stft",
) -> ChordAnalysisResult: ...
def analyze_sections(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    min_section_sec: float = 4.0,
) -> SectionResult: ...
def analyze_melody(
    samples: FloatSamples,
    sample_rate: int = 22050,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    frame_length: int = 2048,
    hop_length: int = 256,
    threshold: float = 0.1,
) -> MelodyResult: ...
def cqt(
    samples: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
) -> CqtResult: ...
def vqt(
    samples: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    fmin: float = 32.70319566257483,
    n_bins: int = 84,
    bins_per_octave: int = 12,
    gamma: float = 0.0,
) -> CqtResult: ...
def version() -> str: ...
def has_ffmpeg_support() -> bool: ...
def hpss(
    samples: FloatSamples,
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
) -> HpssResult: ...
def harmonic(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def percussive(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def time_stretch(
    samples: FloatSamples, sample_rate: int = 22050, rate: float = 1.0
) -> list[float]: ...
def pitch_shift(
    samples: FloatSamples, sample_rate: int = 22050, semitones: float = 0.0
) -> list[float]: ...
def pitch_correct_to_midi(
    samples: FloatSamples,
    sample_rate: int = 22050,
    current_midi: float = 69.0,
    target_midi: float = 69.0,
) -> list[float]: ...
def note_stretch(
    samples: FloatSamples,
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int = 0,
    stretch_ratio: float = 1.0,
) -> list[float]: ...
def voice_change(
    samples: FloatSamples,
    sample_rate: int = 22050,
    pitch_semitones: float = 0.0,
    formant_factor: float = 1.0,
) -> list[float]: ...

class RealtimeVoiceChanger:
    def __init__(
        self,
        sample_rate: int,
        preset: str | Mapping[str, object] = "neutral-monitor",
        *,
        max_block_size: int = 128,
        channels: int = 1,
    ) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> RealtimeVoiceChanger: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def reset(self) -> None: ...
    def set_config(self, preset: str | Mapping[str, object]) -> None: ...
    def latency_samples(self) -> int: ...
    def config_json(self) -> str: ...
    def process_mono(self, samples: FloatSamples) -> np.ndarray: ...
    def process_interleaved(
        self, samples: FloatSamples, channels: int | None = None
    ) -> np.ndarray: ...
    def process_planar_stereo(
        self, left: FloatSamples, right: FloatSamples
    ) -> tuple[np.ndarray, np.ndarray]: ...

def voice_change_realtime(
    samples: FloatSamples,
    sample_rate: int = 48000,
    preset: str | Mapping[str, object] = "neutral-monitor",
    *,
    channels: int = 1,
) -> np.ndarray: ...
def realtime_voice_changer_preset_names() -> list[str]: ...
def realtime_voice_changer_preset_json(name: str) -> str: ...
def validate_realtime_voice_changer_preset_json(json_text: str) -> dict[str, object]: ...
def normalize(
    samples: FloatSamples, sample_rate: int = 22050, target_db: float = 0.0
) -> list[float]: ...
def mastering(
    samples: FloatSamples,
    sample_rate: int = 22050,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    true_peak_oversample: int = 4,
) -> MasteringResult: ...
def mastering_assistant_suggest(
    samples: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> str: ...
def mastering_audio_profile(
    samples: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> str: ...
def mastering_processor_names() -> list[SoloProcessor]: ...
def mastering_pair_processor_names() -> list[PairProcessor]: ...
def mastering_pair_analysis_names() -> list[PairAnalysis]: ...
def mastering_stereo_analysis_names() -> list[StereoAnalysis]: ...
def mastering_process(
    processor_name: SoloProcessor,
    samples: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> MasteringResult: ...
def mastering_process_stereo(
    processor_name: SoloProcessor,
    left: FloatSamples,
    right: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> MasteringStereoResult: ...
def mastering_chain(
    samples: FloatSamples,
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
    on_progress: ProgressCallback | None = None,
) -> MasteringChainResult: ...
def mastering_chain_stereo(
    left: FloatSamples,
    right: FloatSamples,
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
    on_progress: ProgressCallback | None = None,
) -> MasteringChainStereoResult: ...
def mastering_preset_names() -> list[MasteringPreset]: ...
def mixing_scene_preset_names() -> list[str]: ...
def mixing_scene_preset_json(preset_name: str) -> str: ...

class Mixer:
    def __init__(self, handle: int, sample_rate: int, block_size: int) -> None: ...
    @classmethod
    def from_scene_json(
        cls, json: str, sample_rate: int = 48000, block_size: int = 512
    ) -> Mixer: ...
    def compile(self) -> None: ...
    def strip_count(self) -> int: ...
    def strip_by_id(self, strip_id: str) -> int: ...
    def add_bus(self, bus_id: str, role: str = "aux") -> None: ...
    def remove_bus(self, bus_id: str) -> None: ...
    def bus_count(self) -> int: ...
    def add_vca_group(
        self, group_id: str, gain_db: float = 0.0, members: Sequence[str] | None = None
    ) -> None: ...
    def remove_vca_group(self, group_id: str) -> None: ...
    def vca_group_count(self) -> int: ...
    def set_soloed(self, strip: StripRef, soloed: bool) -> None: ...
    def set_solo_safe(self, strip: StripRef, solo_safe: bool) -> None: ...
    def set_polarity_invert(
        self, strip: StripRef, invert_left: bool, invert_right: bool
    ) -> None: ...
    def set_pan_law(self, strip: StripRef, pan_law: PanLaw | str | int) -> None: ...
    def set_channel_delay_samples(self, strip: StripRef, delay_samples: int) -> None: ...
    def set_vca_offset_db(self, strip: StripRef, offset_db: float) -> None: ...
    def set_dual_pan(self, strip: StripRef, left: float, right: float) -> None: ...
    def set_fader_db(self, strip: StripRef, db: float) -> None: ...
    def set_input_trim_db(self, strip: StripRef, db: float) -> None: ...
    def set_pan(self, strip: StripRef, pan: float, pan_mode: int = 0) -> None: ...
    def set_width(self, strip: StripRef, width: float) -> None: ...
    def set_muted(self, strip: StripRef, muted: bool) -> None: ...
    def add_send(
        self,
        strip: StripRef,
        send_id: str,
        destination_bus_id: str,
        send_db: float = 0.0,
        timing: SendTiming | str | int = SendTiming.POST_FADER,
    ) -> int: ...
    def set_send_db(self, strip: StripRef, index: int, db: float) -> None: ...
    def strip_meter(
        self, strip: StripRef, tap: MeterTap | str | int = MeterTap.POST_FADER
    ) -> MixMeterSnapshot: ...
    def meter_tap(
        self, strip: StripRef, tap: MeterTap | str | int = MeterTap.POST_FADER
    ) -> MixMeterSnapshot: ...
    def read_goniometer_latest(self, strip: StripRef, max_points: int) -> list[GoniometerPoint]: ...
    def schedule_fader_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        fader_db: float,
        curve: AutomationCurveArg = AutomationCurve.LINEAR,
    ) -> None: ...
    def schedule_pan_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        pan: float,
        curve: AutomationCurveArg = AutomationCurve.LINEAR,
    ) -> None: ...
    def schedule_width_automation(
        self,
        strip: StripRef,
        sample_pos: int,
        width: float,
        curve: AutomationCurveArg = AutomationCurve.LINEAR,
    ) -> None: ...
    def schedule_send_automation(
        self,
        strip: StripRef,
        send_index: int,
        sample_pos: int,
        db: float,
        curve: AutomationCurveArg = AutomationCurve.LINEAR,
    ) -> None: ...
    def schedule_insert_automation(
        self,
        strip_index: StripRef,
        insert_index: int,
        param_id: int,
        sample_pos: int,
        value: float,
        curve: AutomationCurveArg = AutomationCurve.LINEAR,
    ) -> None: ...
    def process_stereo(
        self,
        left_channels: Sequence[Sequence[float]],
        right_channels: Sequence[Sequence[float]],
    ) -> tuple[list[float], list[float]]: ...
    def to_scene_json(self) -> str: ...
    def close(self) -> None: ...
    def __del__(self) -> None: ...

def mix_stereo(
    strips: Sequence[tuple[FloatSamples, FloatSamples]],
    sample_rate: int = 48000,
    fader_db: Sequence[float] | None = None,
    pan: Sequence[float] | None = None,
    pan_mode: Sequence[str | int] | str | int = "balance",
    width: Sequence[float] | None = None,
    muted: Sequence[bool] | None = None,
    input_trim_db: Sequence[float] | None = None,
) -> MixResult: ...
def master_audio(
    samples: FloatSamples,
    sample_rate: int = 22050,
    preset_name: MasteringPreset = "pop",
    overrides: MasteringParams | None = None,
    on_progress: ProgressCallback | None = None,
) -> MasteringChainResult: ...
def master_audio_stereo(
    left: FloatSamples,
    right: FloatSamples,
    sample_rate: int = 22050,
    preset_name: MasteringPreset = "pop",
    overrides: MasteringParams | None = None,
    on_progress: ProgressCallback | None = None,
) -> MasteringChainStereoResult: ...

class StreamingMasteringChain:
    def __init__(self, config: dict[str, Any] | None = None) -> None: ...
    def prepare(self, sample_rate: int, max_block_size: int, num_channels: int) -> None: ...
    def process_mono(self, samples: FloatSamples) -> list[float]: ...
    def process_stereo(
        self, left: FloatSamples, right: FloatSamples
    ) -> tuple[list[float], list[float]]: ...
    def reset(self) -> None: ...
    def latency_samples(self) -> int: ...
    def close(self) -> None: ...
    def __enter__(self) -> StreamingMasteringChain: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...

class StreamingEqualizer:
    sample_rate: int
    max_block_size: int
    def __init__(self, sample_rate: int = 48000, max_block_size: int = 512) -> None: ...
    def set_band(self, index: int, band: dict[str, Any] | str) -> None: ...
    def clear(self) -> None: ...
    def set_phase_mode(self, mode: int | str) -> None: ...
    def set_auto_gain(self, enabled: bool) -> None: ...
    def set_gain_scale(self, scale: float) -> None: ...
    def set_output_gain_db(self, gain_db: float) -> None: ...
    def set_output_pan(self, pan: float) -> None: ...
    def set_sidechain_mono(self, samples: FloatSamples) -> None: ...
    def set_sidechain_stereo(self, left: FloatSamples, right: FloatSamples) -> None: ...
    def clear_sidechain(self) -> None: ...
    def match(self, source: FloatSamples, reference: FloatSamples, max_bands: int = 8) -> None: ...
    def process_mono(self, samples: FloatSamples) -> list[float]: ...
    def process_stereo(
        self, left: FloatSamples, right: FloatSamples
    ) -> tuple[list[float], list[float]]: ...
    def spectrum(self) -> EqSpectrumSnapshot: ...
    @property
    def latency_samples(self) -> int: ...
    @property
    def last_auto_gain_db(self) -> float: ...
    def close(self) -> None: ...
    def __enter__(self) -> StreamingEqualizer: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...

def mastering_pair_process(
    processor_name: PairProcessor,
    source: FloatSamples,
    reference: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> MasteringResult: ...
def mastering_pair_analyze(
    analysis_name: PairAnalysis,
    source: FloatSamples,
    reference: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> str: ...
def mastering_stereo_analyze(
    analysis_name: StereoAnalysis,
    left: FloatSamples,
    right: FloatSamples,
    sample_rate: int = 22050,
    params: MasteringParams | None = None,
) -> str: ...
def mastering_streaming_preview(
    samples: FloatSamples,
    sample_rate: int = 22050,
    platforms: Sequence[dict[str, float | str]] | None = None,
) -> str: ...
def trim(
    samples: FloatSamples, sample_rate: int = 22050, threshold_db: float = -60.0
) -> list[float]: ...
def stft(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> StftResult: ...
def stft_db(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> tuple[int, int, list[float]]: ...
def mel_spectrogram(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
) -> MelSpectrogramResult: ...
def mfcc(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 20,
) -> MfccResult: ...
def chroma(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> ChromaResult: ...
def spectral_centroid(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> list[float]: ...
def spectral_bandwidth(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> list[float]: ...
def spectral_rolloff(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    roll_percent: float = 0.85,
) -> list[float]: ...
def spectral_flatness(
    samples: FloatSamples, sample_rate: int = 22050, n_fft: int = 2048, hop_length: int = 512
) -> list[float]: ...
def zero_crossing_rate(
    samples: FloatSamples, sample_rate: int = 22050, frame_length: int = 2048, hop_length: int = 512
) -> list[float]: ...
def rms_energy(
    samples: FloatSamples, sample_rate: int = 22050, frame_length: int = 2048, hop_length: int = 512
) -> list[float]: ...
def pitch_yin(
    samples: FloatSamples,
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.3,
    fill_na: bool = False,
) -> PitchResult: ...
def pitch_pyin(
    samples: FloatSamples,
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.3,
    fill_na: bool = False,
) -> PitchResult: ...
def spectral_contrast(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_bands: int = 6,
    fmin: float = 200.0,
    quantile: float = 0.02,
) -> np.ndarray[Any, Any]: ...
def poly_features(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    order: int = 1,
) -> np.ndarray[Any, Any]: ...
def zero_crossings(
    samples: FloatSamples,
    threshold: float = 1e-10,
    ref_magnitude: bool = False,
    pad: bool = True,
    zero_pos: bool = True,
) -> np.ndarray[Any, Any]: ...
def pitch_tuning(
    frequencies: FloatSamples,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float: ...
def estimate_tuning(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float: ...
def lufs_interleaved(
    samples: FloatSamples,
    channels: int,
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> LufsResult: ...
def ebur128_loudness_range(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> float: ...
def decompose(
    s: FloatSamples,
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int = 50,
    beta: float = 2.0,
) -> tuple[np.ndarray[Any, Any], np.ndarray[Any, Any]]: ...
def nn_filter(
    s: FloatSamples,
    n_features: int,
    n_frames: int,
    aggregate: str = "mean",
    k: int = 7,
    width: int = 1,
) -> np.ndarray[Any, Any]: ...
def remix(
    samples: FloatSamples,
    intervals: Sequence[int] | list[int],
    sample_rate: int = 22050,
    align_zeros: bool = False,
) -> np.ndarray[Any, Any]: ...
def hpss_with_residual(
    samples: FloatSamples,
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
) -> dict[str, object]: ...
def phase_vocoder(
    samples: FloatSamples,
    sample_rate: int = 22050,
    rate: float = 1.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> np.ndarray[Any, Any]: ...
def hz_to_mel(hz: float) -> float: ...
def mel_to_hz(mel: float) -> float: ...
def hz_to_midi(hz: float) -> float: ...
def midi_to_hz(midi: float) -> float: ...
def hz_to_note(hz: float) -> str: ...
def note_to_hz(note: str) -> float: ...
def frames_to_time(frames: int, sr: int = 22050, hop_length: int = 512) -> float: ...
def time_to_frames(time: float, sr: int = 22050, hop_length: int = 512) -> int: ...
def frames_to_samples(frames: int, hop_length: int = 512, n_fft: int = 0) -> int: ...
def samples_to_frames(samples: int, hop_length: int = 512, n_fft: int = 0) -> int: ...
def power_to_db(
    values: FloatSamples, ref: float = 1.0, amin: float = 1e-10, top_db: float = 80.0
) -> list[float]: ...
def amplitude_to_db(
    values: FloatSamples, ref: float = 1.0, amin: float = 1e-5, top_db: float = 80.0
) -> list[float]: ...
def db_to_power(values: FloatSamples, ref: float = 1.0) -> list[float]: ...
def db_to_amplitude(values: FloatSamples, ref: float = 1.0) -> list[float]: ...
def preemphasis(
    samples: FloatSamples, coef: float = 0.97, zi: float | None = None
) -> list[float]: ...
def deemphasis(
    samples: FloatSamples, coef: float = 0.97, zi: float | None = None
) -> list[float]: ...
def trim_silence(
    samples: FloatSamples, top_db: float = 60.0, frame_length: int = 2048, hop_length: int = 512
) -> tuple[list[float], int, int]: ...
def split_silence(
    samples: FloatSamples, top_db: float = 60.0, frame_length: int = 2048, hop_length: int = 512
) -> list[tuple[int, int]]: ...
def frame_signal(
    samples: FloatSamples, frame_length: int, hop_length: int
) -> tuple[int, list[float]]: ...
def pad_center(values: FloatSamples, target_size: int, pad_value: float = 0.0) -> list[float]: ...
def fix_length(values: FloatSamples, target_size: int, pad_value: float = 0.0) -> list[float]: ...
def fix_frames(
    frames: IntSamples, x_min: int = 0, x_max: int = -1, pad: bool = True
) -> list[int]: ...
def peak_pick(
    values: FloatSamples,
    pre_max: int,
    post_max: int,
    pre_avg: int,
    post_avg: int,
    delta: float,
    wait: int,
) -> list[int]: ...
def vector_normalize(
    values: FloatSamples, norm_type: int = 0, threshold: float = 0.0
) -> list[float]: ...
def pcen(
    values: FloatSamples,
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    hop_length: int = 512,
    time_constant: float = 0.4,
    gain: float = 0.98,
    bias: float = 2.0,
    power: float = 0.5,
    eps: float = 1e-6,
) -> list[float]: ...
def tonnetz(chromagram: FloatSamples, n_chroma: int, n_frames: int) -> list[float]: ...
def tempogram(
    onset_envelope: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
    mode: TempogramMode = "autocorrelation",
) -> tuple[int, list[float]]: ...
def cyclic_tempogram(
    onset_envelope: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    bpm_min: float = 60.0,
    n_bins: int = 60,
) -> tuple[int, list[float]]: ...
def plp(
    onset_envelope: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    tempo_min: float = 30.0,
    tempo_max: float = 300.0,
    win_length: int = 384,
) -> list[float]: ...
def onset_envelope(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
) -> list[float]: ...
def fourier_tempogram(
    onset_envelope: FloatSamples,
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
) -> tuple[int, list[float]]: ...
def tempogram_ratio(
    tempogram_data: FloatSamples,
    win_length: int = 384,
    sample_rate: int = 22050,
    hop_length: int = 512,
    factors: FloatSamples | None = None,
) -> list[float]: ...
def nnls_chroma(samples: FloatSamples, sample_rate: int = 22050) -> tuple[int, list[float]]: ...
def mastering_repair_declick(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_denoise_classical(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    gain_floor: float = 0.05,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_declip(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_decrackle(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_dehum(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
) -> np.ndarray[Any, Any]: ...
def mastering_repair_dereverb_classical(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.05,
    attenuation: float = 0.5,
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
) -> np.ndarray[Any, Any]: ...
def mastering_repair_trim_silence(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> np.ndarray[Any, Any]: ...
def lufs(samples: FloatSamples, sample_rate: int = 22050) -> LufsResult: ...
def momentary_lufs(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def short_term_lufs(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def metering_stereo_correlation(
    left: FloatSamples, right: FloatSamples, sample_rate: int = 22050
) -> float: ...
def metering_stereo_width(
    left: FloatSamples, right: FloatSamples, sample_rate: int = 22050
) -> float: ...
def metering_vectorscope(
    left: FloatSamples, right: FloatSamples, sample_rate: int = 22050
) -> VectorscopeReport: ...
def metering_phase_scope(
    left: FloatSamples, right: FloatSamples, sample_rate: int = 22050
) -> PhaseScopeReport: ...
def metering_spectrum(
    samples: FloatSamples,
    sample_rate: int = 22050,
    n_fft: int = 0,
    apply_octave_smoothing: bool = False,
    octave_fraction: int = 0,
    db_ref: float = 0.0,
    db_amin: float = 0.0,
) -> SpectrumReport: ...
def metering_peak_db(
    samples: FloatSamples, sample_rate: int = 22050, *, validate: bool = True
) -> float: ...
def metering_rms_db(
    samples: FloatSamples, sample_rate: int = 22050, *, validate: bool = True
) -> float: ...
def metering_crest_factor_db(
    samples: FloatSamples, sample_rate: int = 22050, *, validate: bool = True
) -> float: ...
def metering_dc_offset(
    samples: FloatSamples, sample_rate: int = 22050, *, validate: bool = True
) -> float: ...
def metering_true_peak_db(
    samples: FloatSamples,
    sample_rate: int = 22050,
    oversample_factor: int = 4,
    *,
    validate: bool = True,
) -> float: ...
def metering_detect_clipping(
    samples: FloatSamples,
    sample_rate: int = 22050,
    threshold: float = 0.999,
    min_region_samples: int = 1,
    *,
    validate: bool = True,
) -> ClippingReport: ...
def metering_dynamic_range(
    samples: FloatSamples,
    sample_rate: int = 22050,
    window_sec: float = 0.0,
    hop_sec: float = 0.0,
    low_percentile: float = 0.0,
    high_percentile: float = 0.0,
    *,
    validate: bool = True,
) -> DynamicRangeReport: ...
def scale_quantize_midi(
    root: int, mode_mask: int, midi: float, reference_midi: float = 0.0
) -> float: ...
def scale_correction_semitones(
    root: int, mode_mask: int, midi: float, reference_midi: float = 0.0
) -> float: ...
def scale_pitch_class_enabled(root: int, mode_mask: int, pitch_class: int) -> bool: ...
def mel_to_stft(
    mel: FloatSamples,
    n_mels: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    fmin: float = 0.0,
    fmax: float = 0.0,
) -> InverseResult: ...
def mel_to_audio(
    mel: FloatSamples,
    n_mels: int,
    n_frames: int,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 0.0,
    fmax: float = 0.0,
    n_iter: int = 32,
) -> list[float]: ...
def mfcc_to_mel(
    mfcc_coeffs: FloatSamples,
    n_mfcc: int,
    n_frames: int,
    n_mels: int = 128,
) -> InverseResult: ...
def mfcc_to_audio(
    mfcc_coeffs: FloatSamples,
    n_mfcc: int,
    n_frames: int,
    n_mels: int = 128,
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 0.0,
    fmax: float = 0.0,
    n_iter: int = 32,
) -> list[float]: ...
def mastering_dynamics_compressor(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: int | str = "rms",
    sidechain_hpf_enabled: bool = False,
    sidechain_hpf_hz: float = 100.0,
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
    validate: bool = True,
) -> tuple[np.ndarray[Any, Any], int]: ...
def mastering_dynamics_gate(
    samples: FloatSamples,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_hpf_hz: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray[Any, Any], int]: ...
def mastering_dynamics_transient_shaper(
    samples: FloatSamples,
    sample_rate: int = 22050,
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
    validate: bool = True,
) -> tuple[np.ndarray[Any, Any], int]: ...
def resample(samples: FloatSamples, src_sr: int, target_sr: int) -> list[float]: ...
