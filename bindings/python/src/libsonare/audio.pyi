from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any, Literal

import numpy as np

from .analyzer import (
    MasteringPreset,
    MasteringSoloParams,
    NormalizeMode,
    RealtimeVoiceChangerPreset,
    SoloProcessor,
)
from .types import (
    AcousticResult,
    AnalysisResult,
    BpmAnalysisResult,
    ChannelLayout,
    ChordAnalysisResult,
    ChordChromaMethod,
    ChromaResult,
    ClippingReport,
    DynamicRangeReport,
    DynamicsResult,
    GenreHint,
    HpssResult,
    KeyCandidate,
    KeyDetection,
    KeyMode,
    KeyProfile,
    LufsResult,
    MasteringChainConfig,
    MasteringChainResult,
    MasteringResult,
    MelSpectrogramResult,
    MfccResult,
    PitchClass,
    PitchResult,
    RhythmResult,
    SpectrumReport,
    StftResult,
    TimbreResult,
    VoicePresetId,
)

class DecodedChannels:
    sample_rate: int
    channels: np.ndarray
    def __init__(self, sample_rate: int, channels: np.ndarray) -> None: ...

def decode_channels(data: bytes) -> DecodedChannels: ...
def downmix(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    target_layout: ChannelLayout,
    *,
    normalize: bool = False,
    include_lfe: bool = False,
) -> np.ndarray: ...

class Audio:
    @classmethod
    def from_file(cls, path: str) -> Audio: ...
    @classmethod
    def from_file_channel(cls, path: str, channel_index: int) -> Audio: ...
    @classmethod
    def file_num_channels(cls, path: str) -> int: ...
    @classmethod
    def from_buffer(cls, samples: Sequence[float] | list[float], sample_rate: int) -> Audio: ...
    @classmethod
    def from_memory(cls, data: bytes) -> Audio: ...
    @property
    def data(self) -> np.ndarray: ...
    @property
    def length(self) -> int: ...
    @property
    def sample_rate(self) -> int: ...
    @property
    def duration_sec(self) -> float: ...
    def detect_bpm(
        self, *, bpm_min: float | None = None, bpm_max: float | None = None
    ) -> float: ...
    def detect_key(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        use_hpss: bool = False,
        loudness_weighted: bool = False,
        chroma_high_pass_hz: float = 80.0,
        modes: Sequence[KeyMode] | str | None = None,
        profile: KeyProfile | None = None,
        genre_hint: GenreHint | None = None,
        tuning_cents: float | str = "auto",
    ) -> KeyDetection: ...
    def detect_key_candidates(
        self,
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
        self, *, bpm_min: float | None = None, bpm_max: float | None = None
    ) -> np.ndarray: ...
    def detect_downbeats(self) -> np.ndarray: ...
    def detect_onsets(self) -> np.ndarray: ...
    def analyze(self, **kwargs: Any) -> AnalysisResult: ...
    def analyze_bpm(
        self,
        bpm_min: float | None = None,
        bpm_max: float | None = None,
        start_bpm: float = 120.0,
        n_fft: int = 2048,
        hop_length: int = 512,
        max_candidates: int = 5,
    ) -> BpmAnalysisResult: ...
    def analyze_impulse_response(self, n_octave_bands: int = 6) -> AcousticResult: ...
    def detect_acoustic(
        self,
        n_octave_bands: int = 6,
        n_third_octave_subbands: int = 24,
        min_decay_db: float = 30.0,
        noise_floor_margin_db: float = 10.0,
    ) -> AcousticResult: ...
    def analyze_rhythm(
        self,
        bpm_min: float | None = None,
        bpm_max: float | None = None,
        start_bpm: float = 120.0,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> RhythmResult: ...
    def analyze_dynamics(
        self, window_sec: float = 0.4, hop_length: int = 512, compression_threshold_db: float = 6.0
    ) -> DynamicsResult: ...
    def analyze_timbre(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
        n_mfcc: int = 13,
        window_sec: float = 0.5,
    ) -> TimbreResult: ...
    def detect_chords(
        self,
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
    ) -> ChordAnalysisResult: ...
    def hpss(self, kernel_harmonic: int = 31, kernel_percussive: int = 31) -> HpssResult: ...
    def harmonic(self) -> np.ndarray: ...
    def percussive(self) -> np.ndarray: ...
    def time_stretch(self, speed_ratio: float) -> np.ndarray: ...
    def pitch_shift(self, pitch_semitones: float) -> np.ndarray: ...
    def pitch_correct_to_midi(
        self, current_midi: float = 69.0, target_midi: float = 69.0
    ) -> np.ndarray: ...
    def note_stretch(
        self,
        onset_sample: int | None = None,
        offset_sample: int | None = None,
        stretch_ratio: float = 1.0,
        *,
        onset_sec: float | None = None,
        offset_sec: float | None = None,
    ) -> np.ndarray: ...
    def note_move(
        self,
        onset_sample: int | None = None,
        offset_sample: int | None = None,
        target_onset_sample: int | None = None,
        *,
        onset_sec: float | None = None,
        offset_sec: float | None = None,
        target_onset_sec: float | None = None,
    ) -> np.ndarray: ...
    def voice_change(
        self,
        pitch_semitones: float = 0.0,
        formant_factor: float = 1.0,
        formant_mode: Literal["relative", "absolute"] = "relative",
    ) -> np.ndarray: ...
    def voice_change_realtime(
        self, *, config: VoicePresetId | RealtimeVoiceChangerPreset = "neutralMonitor"
    ) -> np.ndarray: ...
    def normalize(
        self, *, mode: NormalizeMode = "peak", target_db: float | None = None
    ) -> np.ndarray: ...
    def mastering(
        self,
        target_lufs: float = -14.0,
        ceiling_dbtp: float = -1.0,
        true_peak_oversample: int | None = None,
        release_ms: float | None = None,
        apply_gain_at_input_rate: bool = False,
    ) -> MasteringResult: ...
    def mastering_apply_processor(
        self, processor: SoloProcessor, params: MasteringSoloParams | None = None
    ) -> MasteringResult: ...
    def master_audio(
        self,
        *,
        preset: MasteringPreset | None = None,
        overrides: MasteringChainConfig | None = None,
        on_progress: Callable[[float, str], None] | None = None,
        cancel: Callable[[], bool] | None = None,
    ) -> MasteringChainResult: ...
    def trim(self, threshold_db: float = -60.0) -> np.ndarray: ...
    def stft(self, n_fft: int = 2048, hop_length: int = 512) -> StftResult: ...
    def stft_db(self, n_fft: int = 2048, hop_length: int = 512) -> np.ndarray: ...
    def mel_spectrogram(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128
    ) -> MelSpectrogramResult: ...
    def mfcc(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128, n_mfcc: int = 20
    ) -> MfccResult: ...
    def chroma(self, n_fft: int = 2048, hop_length: int = 512) -> ChromaResult: ...
    def nnls_chroma(self) -> np.ndarray: ...
    def onset_strength(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128
    ) -> np.ndarray: ...
    def lufs(self) -> LufsResult: ...
    def momentary_lufs(self) -> np.ndarray: ...
    def short_term_lufs(self) -> np.ndarray: ...
    def spectral_centroid(self, n_fft: int = 2048, hop_length: int = 512) -> np.ndarray: ...
    def spectral_bandwidth(
        self, n_fft: int = 2048, hop_length: int = 512, p: float = 2.0
    ) -> np.ndarray: ...
    def spectral_rolloff(
        self, n_fft: int = 2048, hop_length: int = 512, roll_percent: float = 0.85
    ) -> np.ndarray: ...
    def spectral_flatness(self, n_fft: int = 2048, hop_length: int = 512) -> np.ndarray: ...
    def zero_crossing_rate(self, frame_length: int = 2048, hop_length: int = 512) -> np.ndarray: ...
    def rms_energy(self, frame_length: int = 2048, hop_length: int = 512) -> np.ndarray: ...
    def pitch_yin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        f0_min_hz: float = 65.0,
        f0_max_hz: float = 2093.0,
        threshold: float = 0.1,
    ) -> PitchResult: ...
    def pitch_pyin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        f0_min_hz: float = 65.0,
        f0_max_hz: float = 2093.0,
        threshold: float = 0.1,
        fill_na: bool = False,
    ) -> PitchResult: ...
    def resample(self, target_sample_rate: int) -> np.ndarray: ...
    def peak_db(self) -> float: ...
    def rms_db(self) -> float: ...
    def dc_offset(self) -> float: ...
    def crest_factor_db(self) -> float: ...
    def silence_ratio(
        self,
        threshold_db: float = -45.0,
        frame_length: int = 1024,
        hop_length: int = 256,
    ) -> float: ...
    def true_peak_dbtp(self, oversample_factor: int | None = None) -> float: ...
    def detect_clipping(
        self, threshold_db: float | None = None, min_region_samples: int | None = None
    ) -> ClippingReport: ...
    def dynamic_range(
        self,
        window_sec: float | None = None,
        hop_sec: float | None = None,
        low_percentile: float | None = None,
        high_percentile: float | None = None,
    ) -> DynamicRangeReport: ...
    def spectrum(
        self,
        n_fft: int | None = None,
        apply_octave_smoothing: bool = False,
        octave_fraction: int | None = None,
        db_ref: float | None = None,
        db_amin: float | None = None,
    ) -> SpectrumReport: ...
    def spectrum_frame(
        self,
        frame_offset: int = 0,
        n_fft: int | None = None,
        apply_octave_smoothing: bool = False,
        octave_fraction: int | None = None,
        db_ref: float | None = None,
        db_amin: float | None = None,
    ) -> SpectrumReport: ...
    def ebur128_loudness_range_lu(self) -> float: ...
    def close(self) -> None: ...
    def __enter__(self) -> Audio: ...
    def __exit__(self, *args: object) -> None: ...
