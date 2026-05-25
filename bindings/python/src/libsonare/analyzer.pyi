from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any, Literal, TypeAlias

from .types import (
    AcousticResult,
    AnalysisResult,
    AutomationCurve,
    BpmAnalysisResult,
    ChordAnalysisResult,
    ChromaResult,
    DynamicsResult,
    EqSpectrumSnapshot,
    GoniometerPoint,
    HpssResult,
    Key,
    KeyCandidate,
    KeyProfile,
    LufsResult,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringResult,
    MasteringStereoResult,
    MelSpectrogramResult,
    MeterTap,
    MfccResult,
    MixMeterSnapshot,
    MixResult,
    Mode,
    PanLaw,
    PitchClass,
    PitchResult,
    RhythmResult,
    StftResult,
    TimbreResult,
)

FloatSamples: TypeAlias = Sequence[float] | list[float]
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
def normalize(
    samples: FloatSamples, sample_rate: int = 22050, target_db: float = -3.0
) -> list[float]: ...
def mastering(
    samples: FloatSamples,
    sample_rate: int = 22050,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    true_peak_oversample: int = 4,
) -> MasteringResult: ...
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
def mixing_scene_preset_json(preset: str) -> str: ...

class Mixer:
    def __init__(self, handle: int, sample_rate: int, block_size: int) -> None: ...
    @classmethod
    def from_scene_json(
        cls, json: str, sample_rate: int = 48000, block_size: int = 512
    ) -> Mixer: ...
    def compile(self) -> None: ...
    def strip_count(self) -> int: ...
    def strip_by_id(self, strip_id: str) -> int: ...
    def set_soloed(self, strip: StripRef, soloed: bool) -> None: ...
    def set_solo_safe(self, strip: StripRef, solo_safe: bool) -> None: ...
    def set_polarity_invert(
        self, strip: StripRef, invert_left: bool, invert_right: bool
    ) -> None: ...
    def set_pan_law(self, strip: StripRef, pan_law: PanLaw | str | int) -> None: ...
    def set_channel_delay_samples(self, strip: StripRef, delay_samples: int) -> None: ...
    def set_vca_offset_db(self, strip: StripRef, offset_db: float) -> None: ...
    def set_dual_pan(self, strip: StripRef, left: float, right: float) -> None: ...
    def add_send(
        self,
        strip: StripRef,
        send_id: str,
        destination_bus_id: str,
        send_db: float = 0.0,
        timing: int = 0,
    ) -> int: ...
    def set_send_db(self, strip: StripRef, index: int, db: float) -> None: ...
    def strip_meter(self, strip: StripRef, tap: MeterTap | str | int = ...) -> MixMeterSnapshot: ...
    def meter_tap(self, strip: StripRef, tap: MeterTap | str | int = ...) -> MixMeterSnapshot: ...
    def read_goniometer_latest(self, strip: StripRef, max_points: int) -> list[GoniometerPoint]: ...
    def schedule_fader_automation(
        self, strip: StripRef, sample_pos: int, fader_db: float, curve: AutomationCurveArg = ...
    ) -> None: ...
    def schedule_pan_automation(
        self, strip: StripRef, sample_pos: int, pan: float, curve: AutomationCurveArg = ...
    ) -> None: ...
    def schedule_width_automation(
        self, strip: StripRef, sample_pos: int, width: float, curve: AutomationCurveArg = ...
    ) -> None: ...
    def schedule_send_automation(
        self,
        strip: StripRef,
        send_index: int,
        sample_pos: int,
        db: float,
        curve: AutomationCurveArg = ...,
    ) -> None: ...
    def schedule_insert_automation(
        self,
        strip_index: StripRef,
        insert_index: int,
        param_id: int,
        sample_pos: int,
        value: float,
        curve: AutomationCurveArg = ...,
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
    preset: MasteringPreset = "pop",
    overrides: MasteringParams | None = None,
    on_progress: ProgressCallback | None = None,
) -> MasteringChainResult: ...
def master_audio_stereo(
    left: FloatSamples,
    right: FloatSamples,
    sample_rate: int = 22050,
    preset: MasteringPreset = "pop",
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
) -> PitchResult: ...
def pitch_pyin(
    samples: FloatSamples,
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.3,
) -> PitchResult: ...
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
def pad_center(values: FloatSamples, size: int, pad_value: float = 0.0) -> list[float]: ...
def fix_length(values: FloatSamples, size: int, pad_value: float = 0.0) -> list[float]: ...
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
def lufs(samples: FloatSamples, sample_rate: int = 22050) -> LufsResult: ...
def momentary_lufs(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def short_term_lufs(samples: FloatSamples, sample_rate: int = 22050) -> list[float]: ...
def resample(samples: FloatSamples, src_sr: int, target_sr: int) -> list[float]: ...
