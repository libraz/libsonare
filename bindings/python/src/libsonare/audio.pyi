from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any

from .analyzer import MasteringParams, MasteringPreset, SoloProcessor
from .types import (
    AcousticResult,
    AnalysisResult,
    BpmAnalysisResult,
    ChordAnalysisResult,
    ChromaResult,
    DynamicsResult,
    HpssResult,
    Key,
    KeyCandidate,
    KeyProfile,
    MasteringChainResult,
    MasteringResult,
    MelSpectrogramResult,
    MfccResult,
    Mode,
    PitchClass,
    PitchResult,
    RhythmResult,
    StftResult,
    TimbreResult,
)

class Audio:
    @classmethod
    def from_file(cls, path: str) -> Audio: ...
    @classmethod
    def from_buffer(
        cls, data: Sequence[float] | list[float], sample_rate: int = 22050
    ) -> Audio: ...
    @classmethod
    def from_memory(cls, data: bytes) -> Audio: ...
    @property
    def data(self) -> list[float]: ...
    @property
    def length(self) -> int: ...
    @property
    def sample_rate(self) -> int: ...
    @property
    def duration(self) -> float: ...
    def detect_bpm(self) -> float: ...
    def detect_key(
        self,
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
        self,
        n_fft: int = 4096,
        hop_length: int = 512,
        use_hpss: bool = False,
        loudness_weighted: bool = False,
        high_pass_hz: float = 0.0,
        modes: Sequence[Mode | str] | str | None = None,
        profile: KeyProfile | str | None = None,
        genre_hint: str | None = None,
    ) -> list[KeyCandidate]: ...
    def detect_beats(self) -> list[float]: ...
    def detect_downbeats(self) -> list[float]: ...
    def detect_onsets(self) -> list[float]: ...
    def analyze(self) -> AnalysisResult: ...
    def analyze_bpm(
        self,
        bpm_min: float = 30.0,
        bpm_max: float = 300.0,
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
        bpm_min: float = 60.0,
        bpm_max: float = 200.0,
        start_bpm: float = 120.0,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> RhythmResult: ...
    def analyze_dynamics(
        self, window_sec: float = 0.4, hop_length: int = 512, compression_threshold: float = 6.0
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
    def hpss(self, kernel_harmonic: int = 31, kernel_percussive: int = 31) -> HpssResult: ...
    def harmonic(self) -> list[float]: ...
    def percussive(self) -> list[float]: ...
    def time_stretch(self, rate: float = 1.0) -> list[float]: ...
    def pitch_shift(self, semitones: float = 0.0) -> list[float]: ...
    def normalize(self, target_db: float = -3.0) -> list[float]: ...
    def mastering(
        self, target_lufs: float = -14.0, ceiling_db: float = -1.0, true_peak_oversample: int = 4
    ) -> MasteringResult: ...
    def mastering_process(
        self, processor_name: SoloProcessor, params: MasteringParams | None = None
    ) -> MasteringResult: ...
    def mastering_chain(
        self,
        config: dict[str, Any] | None = None,
        on_progress: Callable[[float, str], None] | None = None,
    ) -> MasteringChainResult: ...
    def master_audio(
        self,
        preset: MasteringPreset = "pop",
        overrides: dict[str, Any] | None = None,
        on_progress: Callable[[float, str], None] | None = None,
    ) -> MasteringChainResult: ...
    def trim(self, threshold_db: float = -60.0) -> list[float]: ...
    def stft(self, n_fft: int = 2048, hop_length: int = 512) -> StftResult: ...
    def stft_db(self, n_fft: int = 2048, hop_length: int = 512) -> tuple[int, int, list[float]]: ...
    def mel_spectrogram(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128
    ) -> MelSpectrogramResult: ...
    def mfcc(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128, n_mfcc: int = 20
    ) -> MfccResult: ...
    def chroma(self, n_fft: int = 2048, hop_length: int = 512) -> ChromaResult: ...
    def spectral_centroid(self, n_fft: int = 2048, hop_length: int = 512) -> list[float]: ...
    def spectral_bandwidth(self, n_fft: int = 2048, hop_length: int = 512) -> list[float]: ...
    def spectral_rolloff(
        self, n_fft: int = 2048, hop_length: int = 512, roll_percent: float = 0.85
    ) -> list[float]: ...
    def spectral_flatness(self, n_fft: int = 2048, hop_length: int = 512) -> list[float]: ...
    def zero_crossing_rate(
        self, frame_length: int = 2048, hop_length: int = 512
    ) -> list[float]: ...
    def rms_energy(self, frame_length: int = 2048, hop_length: int = 512) -> list[float]: ...
    def pitch_yin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        fmin: float = 65.0,
        fmax: float = 2093.0,
        threshold: float = 0.3,
    ) -> PitchResult: ...
    def pitch_pyin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        fmin: float = 65.0,
        fmax: float = 2093.0,
        threshold: float = 0.3,
    ) -> PitchResult: ...
    def resample(self, target_sr: int) -> list[float]: ...
    def close(self) -> None: ...
    def __enter__(self) -> Audio: ...
    def __exit__(self, *args: object) -> None: ...
