"""Audio wrapper for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING

from ._ffi import SONARE_OK, load_library
from .analyzer import (
    analyze as _analyze,
    chroma as _chroma,
    detect_beats as _detect_beats,
    detect_bpm as _detect_bpm,
    detect_key as _detect_key,
    detect_onsets as _detect_onsets,
    harmonic as _harmonic,
    hpss as _hpss,
    mel_spectrogram as _mel_spectrogram,
    mfcc as _mfcc,
    normalize as _normalize,
    percussive as _percussive,
    pitch_pyin as _pitch_pyin,
    pitch_shift as _pitch_shift,
    pitch_yin as _pitch_yin,
    resample as _resample,
    rms_energy as _rms_energy,
    spectral_bandwidth as _spectral_bandwidth,
    spectral_centroid as _spectral_centroid,
    spectral_flatness as _spectral_flatness,
    spectral_rolloff as _spectral_rolloff,
    stft as _stft,
    stft_db as _stft_db,
    time_stretch as _time_stretch,
    trim as _trim,
    zero_crossing_rate as _zero_crossing_rate,
)
from .types import (
    AnalysisResult,
    ChromaResult,
    HpssResult,
    Key,
    MelSpectrogramResult,
    MfccResult,
    PitchResult,
    StftResult,
)

if TYPE_CHECKING:
    pass

_lib: ctypes.CDLL | None = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure."""
    if rc != SONARE_OK:
        lib = _get_lib()
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


class Audio:
    """Wrapper around the SonareAudio opaque pointer.

    Supports context manager protocol for deterministic resource cleanup.
    """

    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def from_file(cls, path: str) -> Audio:
        """Load audio from a file path (WAV, MP3, etc.)."""
        lib = _get_lib()
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_file(
            path.encode("utf-8"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_buffer(
        cls,
        data: Sequence[float] | list[float],
        sample_rate: int = 22050,
    ) -> Audio:
        """Create audio from a float sample buffer.

        Args:
            data: Audio samples as a list of floats or any sequence/array-like.
                  numpy arrays are accepted via the buffer protocol.
            sample_rate: Sample rate in Hz (default 22050).
        """
        lib = _get_lib()
        length = len(data)
        c_array = (ctypes.c_float * length)(*data)
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_buffer(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_memory(cls, data: bytes) -> Audio:
        """Create audio from in-memory WAV/MP3 binary data.

        Args:
            data: Raw file bytes (WAV, MP3, etc.).
        """
        lib = _get_lib()
        length = len(data)
        c_array = (ctypes.c_uint8 * length).from_buffer_copy(data)
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_memory(
            c_array,
            ctypes.c_size_t(length),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @property
    def data(self) -> list[float]:
        """Return audio samples as a list of floats."""
        ptr = self._lib.sonare_audio_data(self._handle)
        length = self._lib.sonare_audio_length(self._handle)
        return [ptr[i] for i in range(length)]

    @property
    def length(self) -> int:
        """Return the number of audio samples."""
        return int(self._lib.sonare_audio_length(self._handle))

    @property
    def sample_rate(self) -> int:
        """Return the sample rate in Hz."""
        return int(self._lib.sonare_audio_sample_rate(self._handle))

    @property
    def duration(self) -> float:
        """Return the audio duration in seconds."""
        return float(self._lib.sonare_audio_duration(self._handle))

    def detect_bpm(self) -> float:
        """Detect BPM (tempo)."""
        return _detect_bpm(self.data, self.sample_rate)

    def detect_key(self) -> Key:
        """Detect musical key."""
        return _detect_key(self.data, self.sample_rate)

    def detect_beats(self) -> list[float]:
        """Detect beat times in seconds."""
        return _detect_beats(self.data, self.sample_rate)

    def detect_onsets(self) -> list[float]:
        """Detect onset times in seconds."""
        return _detect_onsets(self.data, self.sample_rate)

    def analyze(self) -> AnalysisResult:
        """Run full music analysis."""
        return _analyze(self.data, self.sample_rate)

    # --- Effects ---

    def hpss(
        self, kernel_harmonic: int = 31, kernel_percussive: int = 31,
    ) -> HpssResult:
        """Perform harmonic-percussive source separation."""
        return _hpss(self.data, self.sample_rate, kernel_harmonic, kernel_percussive)

    def harmonic(self) -> list[float]:
        """Extract the harmonic component."""
        return _harmonic(self.data, self.sample_rate)

    def percussive(self) -> list[float]:
        """Extract the percussive component."""
        return _percussive(self.data, self.sample_rate)

    def time_stretch(self, rate: float = 1.0) -> list[float]:
        """Time-stretch audio without changing pitch."""
        return _time_stretch(self.data, self.sample_rate, rate)

    def pitch_shift(self, semitones: float = 0.0) -> list[float]:
        """Shift the pitch of audio."""
        return _pitch_shift(self.data, self.sample_rate, semitones)

    def normalize(self, target_db: float = -3.0) -> list[float]:
        """Normalize audio to a target dB level."""
        return _normalize(self.data, self.sample_rate, target_db)

    def trim(self, threshold_db: float = -60.0) -> list[float]:
        """Trim silence from the beginning and end."""
        return _trim(self.data, self.sample_rate, threshold_db)

    # --- Features - Spectrogram ---

    def stft(self, n_fft: int = 2048, hop_length: int = 512) -> StftResult:
        """Compute the short-time Fourier transform."""
        return _stft(self.data, self.sample_rate, n_fft, hop_length)

    def stft_db(
        self, n_fft: int = 2048, hop_length: int = 512,
    ) -> tuple[int, int, list[float]]:
        """Compute the STFT in decibels."""
        return _stft_db(self.data, self.sample_rate, n_fft, hop_length)

    # --- Features - Mel ---

    def mel_spectrogram(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128,
    ) -> MelSpectrogramResult:
        """Compute a Mel spectrogram."""
        return _mel_spectrogram(self.data, self.sample_rate, n_fft, hop_length, n_mels)

    def mfcc(
        self, n_fft: int = 2048, hop_length: int = 512, n_mels: int = 128, n_mfcc: int = 20,
    ) -> MfccResult:
        """Compute Mel-frequency cepstral coefficients."""
        return _mfcc(self.data, self.sample_rate, n_fft, hop_length, n_mels, n_mfcc)

    # --- Features - Chroma ---

    def chroma(self, n_fft: int = 2048, hop_length: int = 512) -> ChromaResult:
        """Compute chroma features."""
        return _chroma(self.data, self.sample_rate, n_fft, hop_length)

    # --- Features - Spectral ---

    def spectral_centroid(
        self, n_fft: int = 2048, hop_length: int = 512,
    ) -> list[float]:
        """Compute the spectral centroid per frame."""
        return _spectral_centroid(self.data, self.sample_rate, n_fft, hop_length)

    def spectral_bandwidth(
        self, n_fft: int = 2048, hop_length: int = 512,
    ) -> list[float]:
        """Compute the spectral bandwidth per frame."""
        return _spectral_bandwidth(self.data, self.sample_rate, n_fft, hop_length)

    def spectral_rolloff(
        self, n_fft: int = 2048, hop_length: int = 512, roll_percent: float = 0.85,
    ) -> list[float]:
        """Compute the spectral rolloff per frame."""
        return _spectral_rolloff(self.data, self.sample_rate, n_fft, hop_length, roll_percent)

    def spectral_flatness(
        self, n_fft: int = 2048, hop_length: int = 512,
    ) -> list[float]:
        """Compute the spectral flatness per frame."""
        return _spectral_flatness(self.data, self.sample_rate, n_fft, hop_length)

    def zero_crossing_rate(
        self, frame_length: int = 2048, hop_length: int = 512,
    ) -> list[float]:
        """Compute the zero-crossing rate per frame."""
        return _zero_crossing_rate(self.data, self.sample_rate, frame_length, hop_length)

    def rms_energy(
        self, frame_length: int = 2048, hop_length: int = 512,
    ) -> list[float]:
        """Compute the RMS energy per frame."""
        return _rms_energy(self.data, self.sample_rate, frame_length, hop_length)

    # --- Features - Pitch ---

    def pitch_yin(
        self, frame_length: int = 2048, hop_length: int = 512,
        fmin: float = 65.0, fmax: float = 2093.0, threshold: float = 0.3,
    ) -> PitchResult:
        """Estimate fundamental frequency using the YIN algorithm."""
        return _pitch_yin(self.data, self.sample_rate, frame_length, hop_length,
                          fmin, fmax, threshold)

    def pitch_pyin(
        self, frame_length: int = 2048, hop_length: int = 512,
        fmin: float = 65.0, fmax: float = 2093.0, threshold: float = 0.3,
    ) -> PitchResult:
        """Estimate fundamental frequency using the pYIN algorithm."""
        return _pitch_pyin(self.data, self.sample_rate, frame_length, hop_length,
                           fmin, fmax, threshold)

    # --- Core - Resample ---

    def resample(self, target_sr: int) -> list[float]:
        """Resample audio to a different sample rate."""
        return _resample(self.data, self.sample_rate, target_sr)

    def close(self) -> None:
        """Free the underlying audio resource."""
        if self._handle:
            self._lib.sonare_audio_free(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> Audio:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
