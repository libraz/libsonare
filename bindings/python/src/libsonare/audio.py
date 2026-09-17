"""Audio wrapper for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._runtime import (
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
)
from .analyzer import (
    analyze_bpm as _analyze_bpm,
)
from .analyzer import (
    analyze_dynamics as _analyze_dynamics,
)
from .analyzer import (
    analyze_impulse_response as _analyze_impulse_response,
)
from .analyzer import (
    analyze_rhythm as _analyze_rhythm,
)
from .analyzer import (
    analyze_timbre as _analyze_timbre,
)
from .analyzer import (
    chroma as _chroma,
)
from .analyzer import (
    detect_acoustic as _detect_acoustic,
)
from .analyzer import (
    detect_chords as _detect_chords,
)
from .analyzer import (
    detect_key_candidates as _detect_key_candidates,
)
from .analyzer import (
    harmonic as _harmonic,
)
from .analyzer import (
    hpss as _hpss,
)
from .analyzer import (
    lufs as _lufs,
)
from .analyzer import (
    master_audio as _master_audio,
)
from .analyzer import (
    mastering as _mastering,
)
from .analyzer import (
    mastering_chain as _mastering_chain,
)
from .analyzer import (
    mastering_process as _mastering_process,
)
from .analyzer import (
    mel_spectrogram as _mel_spectrogram,
)
from .analyzer import (
    mfcc as _mfcc,
)
from .analyzer import (
    momentary_lufs as _momentary_lufs,
)
from .analyzer import (
    nnls_chroma as _nnls_chroma,
)
from .analyzer import (
    normalize as _normalize,
)
from .analyzer import (
    note_move as _note_move,
)
from .analyzer import (
    note_stretch as _note_stretch,
)
from .analyzer import (
    onset_envelope as _onset_envelope,
)
from .analyzer import (
    percussive as _percussive,
)
from .analyzer import (
    pitch_correct_to_midi as _pitch_correct_to_midi,
)
from .analyzer import (
    pitch_pyin as _pitch_pyin,
)
from .analyzer import (
    pitch_shift as _pitch_shift,
)
from .analyzer import (
    pitch_yin as _pitch_yin,
)
from .analyzer import (
    resample as _resample,
)
from .analyzer import (
    rms_energy as _rms_energy,
)
from .analyzer import (
    short_term_lufs as _short_term_lufs,
)
from .analyzer import (
    spectral_bandwidth as _spectral_bandwidth,
)
from .analyzer import (
    spectral_centroid as _spectral_centroid,
)
from .analyzer import (
    spectral_flatness as _spectral_flatness,
)
from .analyzer import (
    spectral_rolloff as _spectral_rolloff,
)
from .analyzer import (
    stft as _stft,
)
from .analyzer import (
    stft_db as _stft_db,
)
from .analyzer import (
    time_stretch as _time_stretch,
)
from .analyzer import (
    trim as _trim,
)
from .analyzer import (
    voice_change as _voice_change,
)
from .analyzer import (
    voice_change_realtime as _voice_change_realtime,
)
from .analyzer import (
    zero_crossing_rate as _zero_crossing_rate,
)
from .types import (
    AcousticResult,
    AnalysisResult,
    BpmAnalysisResult,
    ChordAnalysisResult,
    ChromaResult,
    ClippingRegion,
    ClippingReport,
    DynamicRangeReport,
    DynamicsResult,
    HpssResult,
    Key,
    KeyCandidate,
    KeyProfile,
    LufsResult,
    MasteringChainResult,
    MasteringResult,
    MelSpectrogramResult,
    MfccResult,
    Mode,
    PitchClass,
    PitchResult,
    RhythmResult,
    SpectrumReport,
    StftResult,
    TimbreResult,
)

if TYPE_CHECKING:
    pass


class Audio:
    """Wrapper around the SonareAudio opaque pointer.

    Supports context manager protocol for deterministic resource cleanup.
    """

    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def from_file(cls, path: str) -> Audio:
        """Load audio from a file path.

        Supported formats in default builds: WAV, MP3.
        With FFmpeg enabled (``-DSONARE_WITH_FFMPEG=ON``): M4A/AAC/FLAC/OGG/Opus
        and any other container/codec supported by the linked FFmpeg.

        Raises:
            RuntimeError: If the file does not exist, is too large, or has an
                unsupported format. The error message includes the offending
                extension and a copy-pasteable ``ffmpeg`` conversion hint.
        """
        lib = _get_lib()
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_file(
            path.encode("utf-8"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_file_channel(cls, path: str, channel_index: int) -> Audio:
        """Load one channel of a file, leaving the others out of it.

        An :class:`Audio` carries a single channel, so :meth:`from_file`
        downmixes a multi-channel source into it. This loads the requested
        channel instead, which is what rendering a stereo result needs: pair it
        with :meth:`file_channel_count` and load each channel in turn.

        The file is decoded once per call, so a stereo load costs two decodes.
        The format set and the decoded-buffer contract are :meth:`from_file`'s.

        Raises:
            RuntimeError: If the loaded native library predates this additive
                entry point.
            SonareError: If the file cannot be loaded, or if ``channel_index``
                names no channel the file has.
        """
        lib = _get_lib()
        load = getattr(lib, "sonare_audio_from_file_channel", None)
        if load is None:
            raise RuntimeError(
                "loaded libsonare does not expose sonare_audio_from_file_channel; "
                "rebuild or install a newer native library"
            )
        handle = ctypes.c_void_p()
        _check(
            load(
                path.encode("utf-8"),
                _to_c_int(channel_index, "channel_index"),
                ctypes.byref(handle),
            )
        )
        return cls(handle, lib)

    @classmethod
    def file_channel_count(cls, path: str) -> int:
        """Return the source channel count encoded in an audio file.

        Unlike :meth:`from_file`, this probe does not expose or alter the
        decoded mono :class:`Audio` representation. It is intended for file
        metadata such as the CLI ``info`` report, where the original source
        channel count must remain visible after the native decoder downmixes
        audio for the public mono API.

        Raises:
            RuntimeError: If the loaded native library predates this additive
                probe entry point.
            SonareError: If the file cannot be opened, parsed, or probed.
        """
        lib = _get_lib()
        probe = getattr(lib, "sonare_audio_file_channel_count", None)
        if probe is None:
            raise RuntimeError(
                "loaded libsonare does not expose sonare_audio_file_channel_count; "
                "rebuild or install a newer native library"
            )
        out_channels = ctypes.c_int()
        rc = probe(path.encode("utf-8"), ctypes.byref(out_channels))
        _check(rc)
        channels = int(out_channels.value)
        if channels <= 0:
            # The C API promises a positive count on success. Treat a broken
            # or incompatible implementation as an error rather than letting
            # callers report a successful but meaningless ``channels=0``.
            raise RuntimeError("libsonare returned an invalid audio channel count")
        return channels

    @classmethod
    @_guard_buffer("data")
    def from_buffer(
        cls,
        data: Sequence[float] | list[float],
        sample_rate: int = 48000,
    ) -> Audio:
        """Create audio from a float sample buffer.

        Args:
            data: Mono audio samples. Accepts ``list[float]``, ``tuple[float, ...]``,
                ``array.array``, or a numpy 1D array of dtype ``float32`` (or
                anything castable to ``float``). Stereo input must be downmixed
                first (e.g. ``samples.mean(axis=1, dtype=np.float32)``).
                Values are nominally in ``[-1.0, 1.0]``.
            sample_rate: Sample rate in Hz (default 48000).

        Raises:
            SonareValueError: If ``data`` is empty or holds a NaN or Inf sample.
        """
        lib = _get_lib()
        # Use the shared numpy fast path (zero-copy for contiguous float32
        # ndarrays, one bulk C-level copy otherwise) instead of the slow
        # per-element `(c_float * N)(*data)` marshalling.
        c_array, length = _to_c_float_array(data)
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_buffer(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_memory(cls, data: bytes) -> Audio:
        """Create audio from in-memory encoded audio bytes.

        Args:
            data: Raw file bytes (WAV / MP3 in default builds; also
                M4A/AAC/FLAC/OGG/Opus when libsonare is built with
                ``-DSONARE_WITH_FFMPEG=ON``).
        """
        lib = _get_lib()
        encoded = np.frombuffer(data, dtype=np.uint8)
        length = int(encoded.size)
        c_array = encoded.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_memory(
            c_array,
            _to_c_size_t(length, "length"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    def _require_handle(self) -> ctypes.c_void_p:
        """Return the live handle, or raise if this audio has been closed.

        The test is falsiness, not ``is None``: :meth:`close` leaves a NULL
        ``c_void_p`` behind rather than ``None``, so an identity test never
        fires while every C accessor reached through that handle silently
        returns a neutral value.
        """
        if not self._handle:
            raise RuntimeError("Audio is closed")
        return self._handle

    @property
    def data(self) -> np.ndarray:
        """Return audio samples as a ``float32`` numpy array.

        The returned array owns its memory (a copy of the native buffer), so it
        stays valid after the :class:`Audio` handle is closed.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        ptr = self._lib.sonare_audio_data(handle)
        length = int(self._lib.sonare_audio_length(handle))
        if length == 0:
            return np.empty(0, dtype=np.float32)
        view = np.ctypeslib.as_array(ptr, shape=(length,))
        return np.array(view, dtype=np.float32, copy=True)

    @property
    def length(self) -> int:
        """Return the number of audio samples.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        return int(self._lib.sonare_audio_length(self._require_handle()))

    @property
    def sample_rate(self) -> int:
        """Return the sample rate in Hz.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        return int(self._lib.sonare_audio_sample_rate(self._require_handle()))

    @property
    def duration(self) -> float:
        """Return the audio duration in seconds.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        return float(self._lib.sonare_audio_duration(self._require_handle()))

    def detect_bpm(self) -> float:
        """Detect BPM (tempo).

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Returns:
            Estimated tempo in beats per minute. For confidence and time
            signature, use :func:`libsonare.analyze` instead.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        out_bpm = ctypes.c_float()
        rc = self._lib.sonare_audio_detect_bpm(handle, ctypes.byref(out_bpm))
        _check(rc)
        return float(out_bpm.value)

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
    ) -> Key:
        """Detect musical key."""
        from .analyzer import detect_key

        return detect_key(
            self.data,
            self.sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            use_hpss=use_hpss,
            loudness_weighted=loudness_weighted,
            high_pass_hz=high_pass_hz,
            modes=modes,
            profile=profile,
            genre_hint=genre_hint,
        )

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
    ) -> list[KeyCandidate]:
        """Return ranked musical key candidates."""
        return _detect_key_candidates(
            self.data,
            self.sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            use_hpss=use_hpss,
            loudness_weighted=loudness_weighted,
            high_pass_hz=high_pass_hz,
            modes=modes,
            profile=profile,
            genre_hint=genre_hint,
        )

    def detect_beats(self) -> list[float]:
        """Detect beat times in seconds.

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Returns:
            Beat times in seconds. For the beat-level accent evidence
            (``beat_observations.onset_strength``), the time signature,
            confidence values, or the bar-start positions, use
            :func:`libsonare.analyze` instead — one call returns all of them.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        with _out_float_array(self._lib) as (out_times, out_count):
            rc = self._lib.sonare_audio_detect_beats(
                handle, ctypes.byref(out_times), ctypes.byref(out_count)
            )
            _check(rc)
            return [float(out_times[i]) for i in range(out_count.value)]

    def detect_downbeats(self) -> list[float]:
        """Detect downbeat times in seconds.

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Returns:
            Downbeat times in seconds. For the beat-level accent evidence
            (``beat_observations.onset_strength``), the time signature,
            confidence values, or the bar starts as indices into the beat
            list, use :func:`libsonare.analyze` instead — one call returns all
            of them.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        with _out_float_array(self._lib) as (out_times, out_count):
            rc = self._lib.sonare_audio_detect_downbeats(
                handle, ctypes.byref(out_times), ctypes.byref(out_count)
            )
            _check(rc)
            return [float(out_times[i]) for i in range(out_count.value)]

    def detect_onsets(self) -> list[float]:
        """Detect onset times in seconds.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        out_times = ctypes.POINTER(ctypes.c_float)()
        out_count = ctypes.c_size_t()
        rc = self._lib.sonare_audio_detect_onsets(
            handle, ctypes.byref(out_times), ctypes.byref(out_count)
        )
        _check(rc)
        try:
            return [float(out_times[i]) for i in range(out_count.value)]
        finally:
            if out_times and out_count.value > 0:
                self._lib.sonare_free_floats(out_times)

    def analyze(self) -> AnalysisResult:
        """Run music analysis over the flat native result.

        This path fills only bpm, key, time signature and beat times; the
        richer fields of :class:`~libsonare.AnalysisResult` — ``beat_strengths``
        (one raw onset-envelope frame per beat), ``beat_observations`` (the
        windowed beat-level evidence), ``downbeat_indices``, chords, sections,
        timbre, dynamics, rhythm, melody and form — are left at their defaults.
        Call the module-level :func:`libsonare.analyze` for the complete
        result, and for the tempo and meter options.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        from ._ffi import SonareAnalysisResult
        from .types import Mode, PitchClass, TimeSignature

        handle = self._require_handle()
        out = SonareAnalysisResult()
        rc = self._lib.sonare_audio_analyze(handle, ctypes.byref(out))
        _check(rc)
        try:
            beat_times = [float(out.beat_times[i]) for i in range(out.beat_count)]
            return AnalysisResult(
                bpm=float(out.bpm),
                bpm_confidence=float(out.bpm_confidence),
                key=Key(
                    root=PitchClass(out.key.root),
                    mode=Mode(out.key.mode),
                    confidence=float(out.key.confidence),
                ),
                time_signature=TimeSignature(
                    numerator=int(out.time_signature.numerator),
                    denominator=int(out.time_signature.denominator),
                    confidence=float(out.time_signature.confidence),
                ),
                beat_times=beat_times,
            )
        finally:
            self._lib.sonare_free_result(ctypes.byref(out))

    def analyze_bpm(
        self,
        bpm_min: float = 30.0,
        bpm_max: float = 300.0,
        start_bpm: float = 120.0,
        n_fft: int = 2048,
        hop_length: int = 512,
        max_candidates: int = 5,
    ) -> BpmAnalysisResult:
        """Analyze BPM with candidates and intermediate curves."""
        return _analyze_bpm(
            self.data,
            self.sample_rate,
            bpm_min,
            bpm_max,
            start_bpm,
            n_fft,
            hop_length,
            max_candidates,
        )

    def analyze_impulse_response(self, n_octave_bands: int = 6) -> AcousticResult:
        """Analyze RT60, EDT, and clarity metrics treating this audio as an impulse response."""
        return _analyze_impulse_response(self.data, self.sample_rate, n_octave_bands)

    def detect_acoustic(
        self,
        n_octave_bands: int = 6,
        n_third_octave_subbands: int = 24,
        min_decay_db: float = 30.0,
        noise_floor_margin_db: float = 10.0,
    ) -> AcousticResult:
        """Estimate blind RT60/EDT acoustic parameters from this audio."""
        return _detect_acoustic(
            self.data,
            self.sample_rate,
            n_octave_bands,
            n_third_octave_subbands,
            min_decay_db,
            noise_floor_margin_db,
        )

    def analyze_rhythm(
        self,
        bpm_min: float = 60.0,
        bpm_max: float = 200.0,
        start_bpm: float = 120.0,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> RhythmResult:
        """Analyze rhythm primitives."""
        return _analyze_rhythm(
            self.data,
            self.sample_rate,
            bpm_min,
            bpm_max,
            start_bpm,
            n_fft,
            hop_length,
        )

    def analyze_dynamics(
        self,
        window_sec: float = 0.4,
        hop_length: int = 512,
        compression_threshold: float = 6.0,
    ) -> DynamicsResult:
        """Analyze dynamics and loudness primitives."""
        return _analyze_dynamics(
            self.data,
            self.sample_rate,
            window_sec,
            hop_length,
            compression_threshold,
        )

    def analyze_timbre(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
        n_mfcc: int = 13,
        window_sec: float = 0.5,
    ) -> TimbreResult:
        """Analyze timbre and spectral-shape primitives."""
        return _analyze_timbre(
            self.data,
            self.sample_rate,
            n_fft,
            hop_length,
            n_mels,
            n_mfcc,
            window_sec,
        )

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
    ) -> ChordAnalysisResult:
        """Detect chord segments."""
        return _detect_chords(
            self.data,
            self.sample_rate,
            min_duration,
            smoothing_window,
            threshold,
            use_triads_only,
            n_fft,
            hop_length,
            use_beat_sync,
            use_hmm,
            hmm_beam_width,
            use_key_context,
            key_root,
            key_mode,
            detect_inversions,
            chroma_method,
        )

    # --- Effects ---

    def hpss(
        self,
        kernel_harmonic: int = 31,
        kernel_percussive: int = 31,
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

    def pitch_correct_to_midi(
        self, current_midi: float = 69.0, target_midi: float = 69.0
    ) -> list[float]:
        """Pitch-correct audio from a current MIDI note to a target MIDI note."""
        return _pitch_correct_to_midi(self.data, self.sample_rate, current_midi, target_midi)

    def note_stretch(
        self, onset_sample: int = 0, offset_sample: int | None = None, stretch_ratio: float = 1.0
    ) -> list[float]:
        """Time-stretch a single note region without changing pitch."""
        return _note_stretch(
            self.data, self.sample_rate, onset_sample, offset_sample, stretch_ratio
        )

    def note_move(
        self,
        onset_sample: int = 0,
        offset_sample: int | None = None,
        target_onset_sample: int = 0,
    ) -> list[float]:
        """Move a note region to a new onset without changing its duration."""
        return _note_move(
            self.data, self.sample_rate, onset_sample, offset_sample, target_onset_sample
        )

    def voice_change(
        self, pitch_semitones: float = 0.0, formant_factor: float = 1.0
    ) -> list[float]:
        """Apply a voice-change effect with independent pitch and formant control."""
        return _voice_change(self.data, self.sample_rate, pitch_semitones, formant_factor)

    def voice_change_realtime(self, preset: str = "neutral-monitor") -> list[float]:
        """Apply the integrated realtime voice changer chain offline.

        The default matches every other entry point for this operation — the
        module function, the CLI, the Node and WASM facades, and ordinal 0 of
        the C ABI's ``SonareVoiceCharacterPreset``. It is the monitoring preset:
        a named character preset such as ``"bright-idol"`` additionally applies
        retune, a formant lift, EQ and reverb.
        """
        return _voice_change_realtime(self.data, self.sample_rate, preset)

    def normalize(self, target_db: float = 0.0) -> list[float]:
        """Normalize audio to a target dB level."""
        return _normalize(self.data, self.sample_rate, target_db)

    def mastering(
        self,
        target_lufs: float = -14.0,
        ceiling_db: float = -1.0,
        true_peak_oversample: int = 4,
        release_ms: float = 0.0,
        apply_gain_at_input_rate: bool = False,
    ) -> MasteringResult:
        """Apply mastering loudness normalization with a true-peak ceiling."""
        return _mastering(
            self.data,
            self.sample_rate,
            target_lufs,
            ceiling_db,
            true_peak_oversample,
            release_ms,
            apply_gain_at_input_rate,
        )

    def mastering_process(
        self, processor_name: str, params: dict[str, float | int | bool] | None = None
    ) -> MasteringResult:
        """Apply a named mastering processor."""
        return _mastering_process(processor_name, self.data, self.sample_rate, params)

    def mastering_chain(
        self,
        config: dict | None = None,
        on_progress: Callable[[float, str], None] | None = None,
        *,
        cancel: Callable[[], bool] | None = None,
    ) -> MasteringChainResult:
        """Apply a configurable mastering chain (EQ, dynamics, saturation, etc.).

        See :func:`libsonare.mastering_chain` for the ``config`` schema and
        ``on_progress`` / ``cancel`` semantics.
        """
        return _mastering_chain(
            self.data, self.sample_rate, config, on_progress=on_progress, cancel=cancel
        )

    def master_audio(
        self,
        preset: str = "pop",
        overrides: dict | None = None,
        on_progress: Callable[[float, str], None] | None = None,
        *,
        cancel: Callable[[], bool] | None = None,
    ) -> MasteringChainResult:
        """Apply a named mastering preset chain to this audio.

        See :func:`libsonare.master_audio` for the ``preset``, ``overrides``,
        ``on_progress``, and ``cancel`` semantics.
        """
        return _master_audio(
            self.data, self.sample_rate, preset, overrides, on_progress=on_progress, cancel=cancel
        )

    def trim(self, threshold_db: float = -60.0) -> list[float]:
        """Trim silence from the beginning and end."""
        return _trim(self.data, self.sample_rate, threshold_db)

    # --- Features - Spectrogram ---

    def stft(self, n_fft: int = 2048, hop_length: int = 512) -> StftResult:
        """Compute the short-time Fourier transform."""
        return _stft(self.data, self.sample_rate, n_fft, hop_length)

    def stft_db(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> tuple[int, int, list[float]]:
        """Compute the STFT in decibels."""
        return _stft_db(self.data, self.sample_rate, n_fft, hop_length)

    # --- Features - Mel ---

    def mel_spectrogram(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
    ) -> MelSpectrogramResult:
        """Compute a Mel spectrogram."""
        return _mel_spectrogram(self.data, self.sample_rate, n_fft, hop_length, n_mels)

    def mfcc(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
        n_mfcc: int = 20,
    ) -> MfccResult:
        """Compute Mel-frequency cepstral coefficients."""
        return _mfcc(self.data, self.sample_rate, n_fft, hop_length, n_mels, n_mfcc)

    # --- Features - Chroma ---

    def chroma(self, n_fft: int = 2048, hop_length: int = 512) -> ChromaResult:
        """Compute chroma features."""
        return _chroma(self.data, self.sample_rate, n_fft, hop_length)

    def nnls_chroma(self) -> tuple[int, list[float]]:
        """Compute NNLS chroma. Returns (n_frames, row-major 12 x n_frames matrix)."""
        return _nnls_chroma(self.data, self.sample_rate)

    # --- Features - Onset ---

    def onset_envelope(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
    ) -> list[float]:
        """Compute the onset strength envelope (one value per frame)."""
        return _onset_envelope(self.data, self.sample_rate, n_fft, hop_length, n_mels)

    # --- Loudness (LUFS) ---

    def lufs(self) -> LufsResult:
        """Compute integrated/momentary/short-term LUFS and loudness range."""
        return _lufs(self.data, self.sample_rate)

    def momentary_lufs(self) -> list[float]:
        """Compute the per-block momentary LUFS time series."""
        return _momentary_lufs(self.data, self.sample_rate)

    def short_term_lufs(self) -> list[float]:
        """Compute the per-block short-term LUFS time series."""
        return _short_term_lufs(self.data, self.sample_rate)

    # --- Features - Spectral ---

    def spectral_centroid(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> list[float]:
        """Compute the spectral centroid per frame."""
        return _spectral_centroid(self.data, self.sample_rate, n_fft, hop_length)

    def spectral_bandwidth(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        p: float = 2.0,
    ) -> list[float]:
        """Compute the spectral bandwidth per frame."""
        return _spectral_bandwidth(self.data, self.sample_rate, n_fft, hop_length, p)

    def spectral_rolloff(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        roll_percent: float = 0.85,
    ) -> list[float]:
        """Compute the spectral rolloff per frame."""
        return _spectral_rolloff(self.data, self.sample_rate, n_fft, hop_length, roll_percent)

    def spectral_flatness(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> list[float]:
        """Compute the spectral flatness per frame."""
        return _spectral_flatness(self.data, self.sample_rate, n_fft, hop_length)

    def zero_crossing_rate(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
    ) -> list[float]:
        """Compute the zero-crossing rate per frame."""
        return _zero_crossing_rate(self.data, self.sample_rate, frame_length, hop_length)

    def rms_energy(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
    ) -> list[float]:
        """Compute the RMS energy per frame."""
        return _rms_energy(self.data, self.sample_rate, frame_length, hop_length)

    # --- Features - Pitch ---

    def pitch_yin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        fmin: float = 65.0,
        fmax: float = 2093.0,
        threshold: float = 0.1,
        fill_na: bool = False,
    ) -> PitchResult:
        """Estimate fundamental frequency using the YIN algorithm."""
        return _pitch_yin(
            self.data, self.sample_rate, frame_length, hop_length, fmin, fmax, threshold, fill_na
        )

    def pitch_pyin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        fmin: float = 65.0,
        fmax: float = 2093.0,
        threshold: float = 0.1,
        fill_na: bool = False,
    ) -> PitchResult:
        """Estimate fundamental frequency using the pYIN algorithm."""
        return _pitch_pyin(
            self.data, self.sample_rate, frame_length, hop_length, fmin, fmax, threshold, fill_na
        )

    # --- Core - Resample ---

    def resample(self, target_sr: int) -> list[float]:
        """Resample audio to a different sample rate."""
        return _resample(self.data, self.sample_rate, target_sr)

    # --- Metering (handle form) ---
    #
    # Each of these reads the handle's own validated samples instead of
    # ``self.data`` (a fresh 4N-byte copy per access), so they skip the
    # per-call finiteness scan and the defensive copy their ``metering_*``
    # free-function twins pay. Parameters, order and defaults mirror those
    # twins; see :mod:`libsonare._features_metering`.

    def peak_db(self) -> float:
        """Sample-peak in dBFS over the buffer."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_peak_db(self._require_handle(), ctypes.byref(out))
        _check(rc)
        return float(out.value)

    def rms_db(self) -> float:
        """RMS level in dBFS over the buffer."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_rms_db(self._require_handle(), ctypes.byref(out))
        _check(rc)
        return float(out.value)

    def dc_offset(self) -> float:
        """DC offset (mean) of the buffer in linear amplitude."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_dc_offset(self._require_handle(), ctypes.byref(out))
        _check(rc)
        return float(out.value)

    def crest_factor_db(self) -> float:
        """Crest factor in dB (peak_db - rms_db)."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_crest_factor_db(self._require_handle(), ctypes.byref(out))
        _check(rc)
        return float(out.value)

    def silence_ratio(
        self,
        threshold_db: float = -45.0,
        frame_length: int = 1024,
        hop_length: int = 256,
    ) -> float:
        """Fraction of analysis frames whose RMS is below ``threshold_db``."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_silence_ratio(
            self._require_handle(),
            _to_c_float(threshold_db, "threshold_db"),
            _to_c_int(frame_length, "frame_length"),
            _to_c_int(hop_length, "hop_length"),
            ctypes.byref(out),
        )
        _check(rc)
        return float(out.value)

    def true_peak_db(self, oversample_factor: int = 4) -> float:
        """Inter-sample (true) peak in dBFS.

        ``oversample_factor`` must be a power of two in [1, 16]; pass 0 for the
        library default (4).
        """
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_true_peak_db(
            self._require_handle(),
            _to_c_int(oversample_factor, "oversample_factor"),
            ctypes.byref(out),
        )
        _check(rc)
        return float(out.value)

    def detect_clipping(
        self,
        threshold: float = 0.999,
        min_region_samples: int = 1,
    ) -> ClippingReport:
        """Detect contiguous runs of clipped samples."""
        from ._ffi import SonareClippingResult

        handle = self._require_handle()
        out = SonareClippingResult()
        rc = self._lib.sonare_audio_detect_clipping(
            handle,
            _to_c_float(threshold, "threshold"),
            _to_c_size_t(min_region_samples, "min_region_samples"),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            regions = [
                ClippingRegion(
                    start_sample=int(out.regions[i].start_sample),
                    end_sample=int(out.regions[i].end_sample),
                    length=int(out.regions[i].length),
                    peak=float(out.regions[i].peak),
                )
                for i in range(int(out.region_count))
            ]
            return ClippingReport(
                clipped_samples=int(out.clipped_samples),
                clipping_ratio=float(out.clipping_ratio),
                max_clipped_peak=float(out.max_clipped_peak),
                regions=regions,
            )
        finally:
            self._lib.sonare_free_clipping_result(ctypes.byref(out))

    def dynamic_range(
        self,
        window_sec: float = 0.0,
        hop_sec: float = 0.0,
        low_percentile: float = -1.0,
        high_percentile: float = -1.0,
    ) -> DynamicRangeReport:
        """Sliding-window dynamic range (high_percentile - low_percentile, in dB).

        Pass 0.0 for ``window_sec`` / ``hop_sec`` to use the library default
        (window=3 s, hop=1 s). For ``low_percentile`` / ``high_percentile`` a
        NEGATIVE value (the default ``-1.0``) selects the library default
        percentiles (low=0.10, high=0.95); ``0.0`` is a real request for the
        0th percentile (the minimum-RMS window), not the default.
        """
        from ._ffi import SonareDynamicRangeResult

        handle = self._require_handle()
        out = SonareDynamicRangeResult()
        rc = self._lib.sonare_audio_dynamic_range(
            handle,
            _to_c_float(window_sec, "window_sec"),
            _to_c_float(hop_sec, "hop_sec"),
            _to_c_float(low_percentile, "low_percentile"),
            _to_c_float(high_percentile, "high_percentile"),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            windows = [float(out.window_rms_db[i]) for i in range(int(out.window_count))]
            return DynamicRangeReport(
                dynamic_range_db=float(out.dynamic_range_db),
                low_percentile_db=float(out.low_percentile_db),
                high_percentile_db=float(out.high_percentile_db),
                window_rms_db=windows,
            )
        finally:
            self._lib.sonare_free_dynamic_range_result(ctypes.byref(out))

    def spectrum(
        self,
        n_fft: int = 0,
        apply_octave_smoothing: bool = False,
        octave_fraction: int = 0,
        db_ref: float = 0.0,
        db_amin: float = 0.0,
    ) -> SpectrumReport:
        """Welch-averaged magnitude / power / dB spectrum over the whole buffer.

        This is NOT a single-frame snapshot: the signal is split into
        Hann-windowed, 50%-overlapping ``n_fft``-length frames whose power
        spectra are averaged across the entire buffer (Welch's method), so
        transients are smeared by the averaging. For a true single-frame FFT
        use :meth:`spectrum_frame`.

        Pass 0 for ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin``
        to use the library defaults (2048 / 3 / 1.0 / kEpsilon).
        """
        from ._ffi import SonareSpectrumResult

        handle = self._require_handle()
        out = SonareSpectrumResult()
        rc = self._lib.sonare_audio_spectrum(
            handle,
            _to_c_int(n_fft, "n_fft"),
            ctypes.c_int(1 if apply_octave_smoothing else 0),
            _to_c_int(octave_fraction, "octave_fraction"),
            _to_c_float(db_ref, "db_ref"),
            _to_c_float(db_amin, "db_amin"),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            count = int(out.bin_count)
            return SpectrumReport(
                frequencies=_from_c_float_array(out.frequencies, count),
                magnitude=_from_c_float_array(out.magnitude, count),
                power=_from_c_float_array(out.power, count),
                db=_from_c_float_array(out.db, count),
                n_fft=int(out.n_fft),
                sample_rate=int(out.sample_rate),
            )
        finally:
            self._lib.sonare_free_spectrum_result(ctypes.byref(out))

    def spectrum_frame(
        self,
        frame_offset: int = 0,
        n_fft: int = 0,
        apply_octave_smoothing: bool = False,
        octave_fraction: int = 0,
        db_ref: float = 0.0,
        db_amin: float = 0.0,
    ) -> SpectrumReport:
        """True single-frame mono magnitude / power / dB spectrum (one Hann-windowed FFT).

        Unlike :meth:`spectrum` (Welch-averaged), this is a single
        ``n_fft``-length FFT for spectrum-analyzer "moment" snapshots. The
        frame spans ``[frame_offset, frame_offset + n_fft)``; samples past the
        end are zero-padded. Pass 0 for ``frame_offset`` for the first frame
        and 0 for ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin``
        for the library defaults (2048 / 3 / 1.0 / kEpsilon).

        Cost per call is set by ``n_fft`` rather than by the buffer's length,
        so this may be polled over a long recording frame by frame -- and
        because it reads the handle's own samples, each poll costs no fresh
        copy of the buffer, unlike the ``metering_spectrum_frame`` free
        function over :attr:`data`.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        from ._ffi import SonareSpectrumResult

        handle = self._require_handle()
        out = SonareSpectrumResult()
        rc = self._lib.sonare_audio_spectrum_frame(
            handle,
            _to_c_size_t(frame_offset, "frame_offset"),
            _to_c_int(n_fft, "n_fft"),
            ctypes.c_int(1 if apply_octave_smoothing else 0),
            _to_c_int(octave_fraction, "octave_fraction"),
            _to_c_float(db_ref, "db_ref"),
            _to_c_float(db_amin, "db_amin"),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            count = int(out.bin_count)
            return SpectrumReport(
                frequencies=_from_c_float_array(out.frequencies, count),
                magnitude=_from_c_float_array(out.magnitude, count),
                power=_from_c_float_array(out.power, count),
                db=_from_c_float_array(out.db, count),
                n_fft=int(out.n_fft),
                sample_rate=int(out.sample_rate),
            )
        finally:
            self._lib.sonare_free_spectrum_result(ctypes.byref(out))

    def ebur128_loudness_range(self) -> float:
        """EBU R128 / Tech 3342 Loudness Range (LRA) in LU."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_ebur128_loudness_range(
            self._require_handle(), ctypes.byref(out)
        )
        _check(rc)
        return float(out.value)

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
