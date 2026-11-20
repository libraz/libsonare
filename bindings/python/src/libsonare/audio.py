"""Audio wrapper for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

import numpy as np

from . import _ffi_types_core as _ffi_core
from ._analysis_detection import _DEFAULT_BPM_MAX, _DEFAULT_BPM_MIN
from ._effects_repair_common import _linked_channel_planes
from ._errors import (
    _entry,
    _refuse,
    _refuse_type,
    _require_open,
    _unknown_error,
)
from ._features_metering import (
    _clipping_args,
    _dynamic_range_args,
    _oversample_factor,
    _spectrum_args,
)
from ._narrowing import _domain_float
from ._runtime import (
    _channel_layout_value,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
)
from .analyzer import (
    NormalizeMode,
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
    mastering_apply_processor as _mastering_process,
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
    onset_strength as _onset_strength,
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
    ChannelLayout,
    ChordAnalysisResult,
    ChromaResult,
    ClippingRegion,
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
)

if TYPE_CHECKING:
    from ._effects_voice import RealtimeVoiceChangerPreset
    from ._types_enums import VoicePresetId

# Plane count of each layout, the same table as the core's channel_count().
_LAYOUT_CHANNEL_COUNTS = {"mono": 1, "stereo": 2, "5.1": 6, "7.1": 8}


@dataclass(frozen=True, slots=True, eq=False)
class DecodedChannels:
    """Result of :func:`decode_channels`."""

    sample_rate: int
    """Sample rate of the decoded audio in Hz."""
    channels: np.ndarray
    """Channel-planar float32 array of shape ``(num_channels, frames)``."""


@_entry("decode_channels")
def decode_channels(data: bytes) -> DecodedChannels:
    """Decode audio bytes once and keep every source channel.

    :meth:`Audio.from_memory` folds a multi-channel source to mono as it
    decodes; this returns the channels instead. The format set, size ceiling and
    decoded-buffer contract are :meth:`Audio.from_memory`'s.

    Args:
        data: Raw file bytes (WAV / MP3 in default builds; also
            M4A/AAC/FLAC/OGG/Opus when libsonare is built with
            ``-DSONARE_WITH_FFMPEG=ON``).

    Returns:
        A :class:`DecodedChannels`. ``channels`` is a channel-planar 2-D
        ``float32`` array of shape ``(num_channels, frames)`` in the source's
        own channel order (5.1 is L R C LFE Ls Rs), the shape the N-channel
        entry points take.

    Raises:
        SonareError: If the bytes are empty, cannot be decoded, hold a
            non-finite sample, or declare a sample rate outside the supported
            range.
    """
    lib = _get_lib()
    decode = lib.sonare_decode_channels
    encoded = np.frombuffer(data, dtype=np.uint8)
    c_array = encoded.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
    channels = ctypes.c_int()
    sample_rate = ctypes.c_int()
    with _out_float_array(lib) as (out, frames):
        _check(
            decode(
                c_array,
                _to_c_size_t(int(encoded.size), "length"),
                ctypes.byref(out),
                ctypes.byref(frames),
                ctypes.byref(channels),
                ctypes.byref(sample_rate),
            )
        )
        planar = _from_c_float_array(out, channels.value * frames.value)
        return DecodedChannels(
            sample_rate=int(sample_rate.value),
            channels=planar.reshape(channels.value, frames.value),
        )


@_entry("downmix")
def downmix(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    target_layout: ChannelLayout,
    *,
    normalize: bool = False,
    include_lfe: bool = False,
) -> np.ndarray:
    """Downmix channels to a narrower layout with the ITU-R BS.775 rule.

    Center and surround enter the front pair at -3 dB and the LFE plane is
    dropped; a stereo, 5.1 or 7.1 source folds to mono through the same matrix
    the decoders use. The source layout is the one the channel count names (1, 2,
    6 or 8). Supported targets narrow the bed (7.1 to 5.1, 5.1 or 7.1 to stereo,
    anything modelled to mono) or copy it. A channel count outside 1/2/6/8 folds
    to ``"mono"`` as the unweighted mean of its channels, as the
    decoders do, and has no other target.

    The fold is not normalised: the coefficients sum, so a source with several
    full-scale channels can leave the output past full scale (six channels of 1.0
    fold to 2.414 on a stereo target). Pass ``normalize=True`` to scale the output
    so the worst case stays at or below full scale, or leave headroom first.

    Args:
        channels: One buffer per channel, or a 2-D ``(channels, frames)`` array.
            All channels must be the same length.
        target_layout: A :data:`ChannelLayout` token (``"mono"``, ``"stereo"``,
            ``"5.1"`` or ``"7.1"``).
        normalize: Scale the fold so the worst case (every input at full scale, in
            phase) cannot exceed unity. A peak divisor, not a loudness match, so
            typical material comes out a few dB quieter.
        include_lfe: Fold the LFE channel into the front pair at -3 dB instead of
            dropping it.

    Returns:
        A channel-planar 2-D ``float32`` array of shape
        ``(target_channel_count, frames)``.

    Raises:
        TypeError: If ``normalize`` or ``include_lfe`` is not a ``bool``.
        SonareValueError: If ``channels`` is empty, not planar, or its channels
            differ in length.
        SonareError: For an upmix or an unknown ``target_layout``.
    """
    lib = _get_lib()
    mix = lib.sonare_downmix
    if not isinstance(normalize, bool):
        raise _refuse_type("normalize must be a bool")
    if not isinstance(include_lfe, bool):
        raise _refuse_type("include_lfe must be a bool")
    layout = _channel_layout_value(target_layout, "target_layout")
    planes = _linked_channel_planes(channels)
    frame_count = int(planes[0].shape[0])
    if any(int(plane.shape[0]) != frame_count for plane in planes):
        raise _refuse("all channels must have the same length")
    packed = np.ascontiguousarray(np.stack(planes), dtype=np.float32)
    fold = _ffi_core.SonareDownmixOptions()
    lib.sonare_downmix_options_init(ctypes.byref(fold))
    fold.include_lfe = 1 if include_lfe else 0
    fold.normalize = 1 if normalize else 0
    with _out_float_array(lib) as (out, _unused):
        _check(
            mix(
                packed.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                _to_c_size_t(frame_count, "frames"),
                _to_c_size_t(len(planes), "num_channels"),
                _to_c_int(layout, "target_layout"),
                ctypes.byref(fold),
                ctypes.byref(out),
            ),
            flattened="options",
        )
        out_channels = _LAYOUT_CHANNEL_COUNTS[target_layout]
        planar = _from_c_float_array(out, out_channels * frame_count)
        return planar.reshape(out_channels, frame_count)


class Audio:
    """Wrapper around the SonareAudio opaque pointer.

    Supports context manager protocol for deterministic resource cleanup.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("Audio")
    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    @_entry("Audio.from_file")
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
            _utf8_arg(path, "path"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    @_entry("Audio.from_file_channel")
    def from_file_channel(cls, path: str, channel_index: int) -> Audio:
        """Load one channel of a file, leaving the others out of it.

        An :class:`Audio` carries a single channel, so :meth:`from_file`
        downmixes a multi-channel source into it. This loads the requested
        channel instead, which is what rendering a stereo result needs: pair it
        with :meth:`file_num_channels` and load each channel in turn.

        The file is decoded once per call, so a stereo load costs two decodes.
        The format set and the decoded-buffer contract are :meth:`from_file`'s.

        Raises:
            SonareError: If the file cannot be loaded, or if ``channel_index``
                names no channel the file has.
        """
        lib = _get_lib()
        load = lib.sonare_audio_from_file_channel
        handle = ctypes.c_void_p()
        _check(
            load(
                _utf8_arg(path, "path"),
                _to_c_int(channel_index, "channel_index"),
                ctypes.byref(handle),
            )
        )
        return cls(handle, lib)

    @classmethod
    @_entry("Audio.file_num_channels")
    def file_num_channels(cls, path: str) -> int:
        """Return the source channel count encoded in an audio file.

        Unlike :meth:`from_file`, this probe does not expose or alter the
        decoded mono :class:`Audio` representation. It is intended for file
        metadata such as the CLI ``info`` report, where the original source
        channel count must remain visible after the native decoder downmixes
        audio for the public mono API.

        Raises:
            SonareError: If the file cannot be opened, parsed, or probed.
        """
        lib = _get_lib()
        probe = lib.sonare_audio_file_num_channels
        out_channels = ctypes.c_int()
        rc = probe(_utf8_arg(path, "path"), ctypes.byref(out_channels))
        _check(rc)
        channels = int(out_channels.value)
        if channels <= 0:
            # The C API promises a positive count on success. Treat a broken
            # or incompatible implementation as an error rather than letting
            # callers report a successful but meaningless ``channels=0``.
            raise _unknown_error("libsonare returned an invalid audio channel count")
        return channels

    @classmethod
    @_entry("Audio.from_buffer")
    @_guard_buffer("samples")
    def from_buffer(
        cls,
        samples: Sequence[float] | list[float],
        sample_rate: int,
    ) -> Audio:
        """Create audio from a float sample buffer.

        Args:
            samples: Mono audio samples. Accepts ``list[float]``, ``tuple[float, ...]``,
                ``array.array``, or a numpy 1D array of dtype ``float32`` (or
                anything castable to ``float``). Multi-channel input must be
                folded first with :func:`downmix` (``downmix(channels, 0)[0]``).
                Values are nominally in ``[-1.0, 1.0]``.
            sample_rate: Sample rate of ``samples`` in Hz.

        Raises:
            SonareValueError: If ``samples`` is empty or holds a NaN or Inf sample.
        """
        lib = _get_lib()
        # Use the shared numpy fast path (zero-copy for contiguous float32
        # ndarrays, one bulk C-level copy otherwise) instead of the slow
        # per-element `(c_float * N)(*data)` marshalling.
        c_array, length = _to_c_float_array(samples)
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
    @_entry("Audio.from_memory")
    def from_memory(cls, data: bytes) -> Audio:
        """Create audio from in-memory encoded audio bytes.

        Args:
            data: Raw file bytes (WAV / MP3 in default builds; also
                M4A/AAC/FLAC/OGG/Opus when libsonare is built with
                ``-DSONARE_WITH_FFMPEG=ON``).

        A multi-channel source is folded to mono; :func:`decode_channels` keeps
        its channels.
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
        return _require_open(self._handle, "Audio")

    def _audio_float(self, symbol: str, handle: ctypes.c_void_p, *args: Any) -> float:
        """Call ``symbol(handle, *args, &out)`` and return the ``float`` out-parameter."""
        out = ctypes.c_float(0.0)
        _check(getattr(self._lib, symbol)(handle, *args, ctypes.byref(out)))
        return float(out.value)

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
    def duration_sec(self) -> float:
        """Return the audio duration in seconds.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        return float(self._lib.sonare_audio_duration_sec(self._require_handle()))

    @_entry("Audio.detect_bpm")
    def detect_bpm(self, *, bpm_min: float | None = None, bpm_max: float | None = None) -> float:
        """Detect BPM (tempo).

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Args:
            bpm_min: Lower bound of the tempo search in BPM (default 60).
            bpm_max: Upper bound of the tempo search in BPM (default 200).

        Returns:
            Estimated tempo in beats per minute. For confidence and time
            signature, use :func:`libsonare.analyze` instead.

        Raises:
            SonareValueError: If the range is non-finite, non-positive or inverted.
            RuntimeError: If the audio has been closed.
        """
        return self._audio_float(
            "sonare_audio_detect_bpm",
            self._require_handle(),
            _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
            _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
        )

    @_entry("Audio.detect_key")
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
    ) -> KeyDetection:
        """Detect musical key and the tuning its chroma was built with.

        ``tuning_cents`` defaults to ``"auto"`` (measured from the audio); pass 0 for A440.
        """
        from .analyzer import detect_key

        return detect_key(
            self.data,
            self.sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            use_hpss=use_hpss,
            loudness_weighted=loudness_weighted,
            chroma_high_pass_hz=chroma_high_pass_hz,
            modes=modes,
            profile=profile,
            genre_hint=genre_hint,
            tuning_cents=tuning_cents,
        )

    @_entry("Audio.detect_key_candidates")
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
    ) -> list[KeyCandidate]:
        """Return ranked musical key candidates; ``tuning_cents`` is as for :meth:`detect_key`."""
        return _detect_key_candidates(
            self.data,
            self.sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            use_hpss=use_hpss,
            loudness_weighted=loudness_weighted,
            chroma_high_pass_hz=chroma_high_pass_hz,
            modes=modes,
            profile=profile,
            genre_hint=genre_hint,
            tuning_cents=tuning_cents,
        )

    @_entry("Audio.detect_beats")
    def detect_beats(
        self, *, bpm_min: float | None = None, bpm_max: float | None = None
    ) -> np.ndarray:
        """Detect beat times in seconds.

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Args:
            bpm_min: Lower bound of the tempo search in BPM (default 60).
            bpm_max: Upper bound of the tempo search in BPM (default 200).

        Returns:
            A float32 array of beat times in seconds. For the beat-level accent evidence
            (``beat_observations.onset_strength``), the time signature,
            confidence values, or the bar-start positions, use
            :func:`libsonare.analyze` instead — one call returns all of them.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        handle = self._require_handle()
        with _out_float_array(self._lib) as (out_times, out_count):
            rc = self._lib.sonare_audio_detect_beats(
                handle,
                _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
                _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
                ctypes.byref(out_times),
                ctypes.byref(out_count),
            )
            _check(rc)
            return _from_c_float_array(out_times, out_count.value)

    @_entry("Audio.detect_downbeats")
    def detect_downbeats(self) -> np.ndarray:
        """Detect downbeat times in seconds.

        Each ``detect_*`` call runs its own analysis; nothing is cached, so
        four calls run the pipeline four times.

        Returns:
            A float32 array of downbeat times in seconds. For the beat-level accent evidence
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
            return _from_c_float_array(out_times, out_count.value)

    @_entry("Audio.detect_onsets")
    def detect_onsets(self) -> np.ndarray:
        """Detect onset times in seconds as a float32 array.

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
            return _from_c_float_array(out_times, out_count.value)
        finally:
            if out_times and out_count.value > 0:
                self._lib.sonare_free_floats(out_times)

    @_entry("Audio.analyze")
    def analyze(self, **kwargs: Any) -> AnalysisResult:
        """Run full music analysis, forwarding to the module-level :func:`libsonare.analyze`.

        Accepts the same keyword options (``bpm_min``, ``use_hpss``,
        ``meter_candidate_numerators``, ``tuning_cents``, ...) and returns the same
        fully populated :class:`~libsonare.AnalysisResult` -- chords, sections,
        timbre, dynamics, rhythm, melody and form included, not just bpm, key,
        time signature and beat times. This handle contributes only its
        decoded ``data`` / ``sample_rate``; the analysis itself carries no
        handle-lifetime state of its own, so there is nothing this method adds
        over calling the module-level function directly except not having to
        pass them again.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        from ._analysis_reports import analyze as _analyze

        # .data itself calls _require_handle(), so a closed Audio still raises
        # RuntimeError here without a second explicit check.
        return _analyze(self.data, self.sample_rate, **kwargs)

    @_entry("Audio.analyze_bpm")
    def analyze_bpm(
        self,
        bpm_min: float | None = None,
        bpm_max: float | None = None,
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

    @_entry("Audio.analyze_impulse_response")
    def analyze_impulse_response(self, n_octave_bands: int = 6) -> AcousticResult:
        """Analyze RT60, EDT, and clarity metrics treating this audio as an impulse response."""
        return _analyze_impulse_response(self.data, self.sample_rate, n_octave_bands)

    @_entry("Audio.detect_acoustic")
    def detect_acoustic(
        self,
        n_octave_bands: int = 6,
        n_third_octave_subbands: int = 24,
        min_decay_db: float = 30.0,
        noise_floor_margin_db: float = 10.0,
    ) -> AcousticResult:
        """Estimate blind RT60 from this audio (EDT and clarity read as NaN)."""
        return _detect_acoustic(
            self.data,
            self.sample_rate,
            n_octave_bands,
            n_third_octave_subbands,
            min_decay_db,
            noise_floor_margin_db,
        )

    @_entry("Audio.analyze_rhythm")
    def analyze_rhythm(
        self,
        bpm_min: float | None = None,
        bpm_max: float | None = None,
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

    @_entry("Audio.analyze_dynamics")
    def analyze_dynamics(
        self,
        window_sec: float = 0.4,
        hop_length: int = 512,
        compression_threshold_db: float = 6.0,
    ) -> DynamicsResult:
        """Analyze dynamics and loudness primitives."""
        return _analyze_dynamics(
            self.data,
            self.sample_rate,
            window_sec,
            hop_length,
            compression_threshold_db,
        )

    @_entry("Audio.analyze_timbre")
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

    @_entry("Audio.detect_chords")
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
        chroma_method: str = "cqt",
        tuning_cents: float | str = "auto",
    ) -> ChordAnalysisResult:
        """Detect chord segments."""
        return _detect_chords(
            self.data,
            self.sample_rate,
            min_duration_sec,
            smoothing_window_sec,
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
            tuning_cents,
        )

    # --- Effects ---

    @_entry("Audio.hpss")
    def hpss(
        self,
        kernel_harmonic: int = 31,
        kernel_percussive: int = 31,
    ) -> HpssResult:
        """Perform harmonic-percussive source separation."""
        return _hpss(self.data, self.sample_rate, kernel_harmonic, kernel_percussive)

    @_entry("Audio.harmonic")
    def harmonic(self) -> np.ndarray:
        """Extract the harmonic component."""
        return _harmonic(self.data, self.sample_rate)

    @_entry("Audio.percussive")
    def percussive(self) -> np.ndarray:
        """Extract the percussive component."""
        return _percussive(self.data, self.sample_rate)

    @_entry("Audio.time_stretch")
    def time_stretch(self, speed_ratio: float) -> np.ndarray:
        """Time-stretch audio without changing pitch; ``speed_ratio`` > 1 shortens."""
        return _time_stretch(self.data, self.sample_rate, speed_ratio)

    @_entry("Audio.pitch_shift")
    def pitch_shift(self, pitch_semitones: float) -> np.ndarray:
        """Shift the pitch of audio by ``pitch_semitones`` (positive = up)."""
        return _pitch_shift(self.data, self.sample_rate, pitch_semitones)

    @_entry("Audio.pitch_correct_to_midi")
    def pitch_correct_to_midi(
        self, current_midi: float = 69.0, target_midi: float = 69.0
    ) -> np.ndarray:
        """Pitch-correct audio from a current MIDI note to a target MIDI note."""
        return _pitch_correct_to_midi(self.data, self.sample_rate, current_midi, target_midi)

    @_entry("Audio.note_stretch")
    def note_stretch(
        self,
        onset_sample: int | None = None,
        offset_sample: int | None = None,
        stretch_ratio: float = 1.0,
        *,
        onset_sec: float | None = None,
        offset_sec: float | None = None,
    ) -> np.ndarray:
        """Time-stretch a single note region without changing pitch."""
        return _note_stretch(
            self.data,
            self.sample_rate,
            onset_sample,
            offset_sample,
            stretch_ratio,
            onset_sec=onset_sec,
            offset_sec=offset_sec,
        )

    @_entry("Audio.note_move")
    def note_move(
        self,
        onset_sample: int | None = None,
        offset_sample: int | None = None,
        target_onset_sample: int | None = None,
        *,
        onset_sec: float | None = None,
        offset_sec: float | None = None,
        target_onset_sec: float | None = None,
    ) -> np.ndarray:
        """Move a note region to a new onset without changing its duration."""
        return _note_move(
            self.data,
            self.sample_rate,
            onset_sample,
            offset_sample,
            target_onset_sample,
            onset_sec=onset_sec,
            offset_sec=offset_sec,
            target_onset_sec=target_onset_sec,
        )

    @_entry("Audio.voice_change")
    def voice_change(
        self,
        pitch_semitones: float = 0.0,
        formant_factor: float = 1.0,
        formant_mode: str = "relative",
    ) -> np.ndarray:
        """Apply a voice-change effect with independent pitch and formant control."""
        return _voice_change(
            self.data, self.sample_rate, pitch_semitones, formant_factor, formant_mode
        )

    @_entry("Audio.voice_change_realtime")
    def voice_change_realtime(
        self, *, config: VoicePresetId | RealtimeVoiceChangerPreset = "neutralMonitor"
    ) -> np.ndarray:
        """Apply the integrated realtime voice changer chain offline.

        The default matches every other entry point for this operation — the
        module function, the CLI, the Node and WASM facades, and ordinal 0 of
        the C ABI's ``SonareVoiceCharacterPreset``. It is the monitoring preset:
        a named character preset such as ``"brightIdol"`` additionally applies
        retune, a formant lift, EQ and reverb.
        """
        return _voice_change_realtime(self.data, self.sample_rate, config=config)

    @_entry("Audio.normalize")
    def normalize(
        self, *, mode: NormalizeMode = "peak", target_db: float | None = None
    ) -> np.ndarray:
        """Normalize the peak or RMS level; see :func:`libsonare.normalize`."""
        return _normalize(self.data, self.sample_rate, mode=mode, target_db=target_db)

    @_entry("Audio.mastering")
    def mastering(
        self,
        target_lufs: float = -14.0,
        ceiling_dbtp: float = -1.0,
        true_peak_oversample: int | None = None,
        release_ms: float | None = None,
        apply_gain_at_input_rate: bool = False,
    ) -> MasteringResult:
        """Apply mastering loudness normalization with a true-peak ceiling."""
        return _mastering(
            self.data,
            self.sample_rate,
            target_lufs,
            ceiling_dbtp,
            true_peak_oversample,
            release_ms,
            apply_gain_at_input_rate,
        )

    @_entry("Audio.mastering_apply_processor")
    def mastering_apply_processor(
        self, processor: str, params: dict[str, float | int | bool | str] | None = None
    ) -> MasteringResult:
        """Apply a named mastering processor."""
        return _mastering_process(processor, self.data, self.sample_rate, params)

    @_entry("Audio.master_audio")
    def master_audio(
        self,
        *,
        preset: str | None = None,
        overrides: MasteringChainConfig | None = None,
        on_progress: Callable[[float, str], None] | None = None,
        cancel: Callable[[], bool] | None = None,
    ) -> MasteringChainResult:
        """Master this audio with a named preset, a chain of overrides, or both.

        See :func:`libsonare.master_audio` for the ``preset``, ``overrides``,
        ``on_progress``, and ``cancel`` semantics; neither given raises SonareValueError.
        """
        return _master_audio(
            self.data,
            self.sample_rate,
            preset=preset,
            overrides=overrides,
            on_progress=on_progress,
            cancel=cancel,
        )

    @_entry("Audio.trim")
    def trim(self, threshold_db: float = -60.0) -> np.ndarray:
        """Trim silence from the beginning and end."""
        return _trim(self.data, self.sample_rate, threshold_db)

    # --- Features - Spectrogram ---

    @_entry("Audio.stft")
    def stft(self, n_fft: int = 2048, hop_length: int = 512) -> StftResult:
        """Compute the short-time Fourier transform."""
        return _stft(self.data, self.sample_rate, n_fft, hop_length)

    @_entry("Audio.stft_db")
    def stft_db(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> np.ndarray:
        """Compute the STFT in decibels as a float32 array of shape ``(n_bins, n_frames)``."""
        return _stft_db(self.data, self.sample_rate, n_fft, hop_length)

    # --- Features - Mel ---

    @_entry("Audio.mel_spectrogram")
    def mel_spectrogram(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
    ) -> MelSpectrogramResult:
        """Compute a Mel spectrogram."""
        return _mel_spectrogram(self.data, self.sample_rate, n_fft, hop_length, n_mels)

    @_entry("Audio.mfcc")
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

    @_entry("Audio.chroma")
    def chroma(self, n_fft: int = 2048, hop_length: int = 512) -> ChromaResult:
        """Compute chroma features."""
        return _chroma(self.data, self.sample_rate, n_fft, hop_length)

    @_entry("Audio.nnls_chroma")
    def nnls_chroma(self) -> np.ndarray:
        """Compute NNLS chroma as a float32 array of shape ``(12, n_frames)``."""
        return _nnls_chroma(self.data, self.sample_rate)

    # --- Features - Onset ---

    @_entry("Audio.onset_strength")
    def onset_strength(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
    ) -> np.ndarray:
        """Compute the onset strength envelope (float32, one value per frame)."""
        return _onset_strength(self.data, self.sample_rate, n_fft, hop_length, n_mels)

    # --- Loudness (LUFS) ---

    @_entry("Audio.lufs")
    def lufs(self) -> LufsResult:
        """Compute integrated/momentary/short-term LUFS and loudness range."""
        return _lufs(self.data, self.sample_rate)

    @_entry("Audio.momentary_lufs")
    def momentary_lufs(self) -> np.ndarray:
        """Compute the per-block momentary LUFS time series as a float32 array."""
        return _momentary_lufs(self.data, self.sample_rate)

    @_entry("Audio.short_term_lufs")
    def short_term_lufs(self) -> np.ndarray:
        """Compute the per-block short-term LUFS time series as a float32 array."""
        return _short_term_lufs(self.data, self.sample_rate)

    # --- Features - Spectral ---

    @_entry("Audio.spectral_centroid")
    def spectral_centroid(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> np.ndarray:
        """Compute the spectral centroid per frame as a float32 array."""
        return _spectral_centroid(self.data, self.sample_rate, n_fft, hop_length)

    @_entry("Audio.spectral_bandwidth")
    def spectral_bandwidth(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        p: float = 2.0,
    ) -> np.ndarray:
        """Compute the spectral bandwidth per frame as a float32 array."""
        return _spectral_bandwidth(self.data, self.sample_rate, n_fft, hop_length, p)

    @_entry("Audio.spectral_rolloff")
    def spectral_rolloff(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        roll_percent: float = 0.85,
    ) -> np.ndarray:
        """Compute the spectral rolloff per frame as a float32 array."""
        return _spectral_rolloff(self.data, self.sample_rate, n_fft, hop_length, roll_percent)

    @_entry("Audio.spectral_flatness")
    def spectral_flatness(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
    ) -> np.ndarray:
        """Compute the spectral flatness per frame as a float32 array."""
        return _spectral_flatness(self.data, self.sample_rate, n_fft, hop_length)

    @_entry("Audio.zero_crossing_rate")
    def zero_crossing_rate(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
    ) -> np.ndarray:
        """Compute the zero-crossing rate per frame as a float32 array."""
        return _zero_crossing_rate(self.data, self.sample_rate, frame_length, hop_length)

    @_entry("Audio.rms_energy")
    def rms_energy(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
    ) -> np.ndarray:
        """Compute the RMS energy per frame as a float32 array."""
        return _rms_energy(self.data, self.sample_rate, frame_length, hop_length)

    # --- Features - Pitch ---

    @_entry("Audio.pitch_yin")
    def pitch_yin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        f0_min_hz: float = 65.0,
        f0_max_hz: float = 2093.0,
        threshold: float = 0.1,
    ) -> PitchResult:
        """Estimate fundamental frequency using the YIN algorithm."""
        return _pitch_yin(
            self.data,
            self.sample_rate,
            frame_length,
            hop_length,
            f0_min_hz,
            f0_max_hz,
            threshold,
        )

    @_entry("Audio.pitch_pyin")
    def pitch_pyin(
        self,
        frame_length: int = 2048,
        hop_length: int = 512,
        f0_min_hz: float = 65.0,
        f0_max_hz: float = 2093.0,
        threshold: float = 0.1,
        fill_na: bool = False,
    ) -> PitchResult:
        """Estimate fundamental frequency using the pYIN algorithm."""
        return _pitch_pyin(
            self.data,
            self.sample_rate,
            frame_length,
            hop_length,
            f0_min_hz,
            f0_max_hz,
            threshold,
            fill_na,
        )

    # --- Core - Resample ---

    @_entry("Audio.resample")
    def resample(self, target_sample_rate: int) -> np.ndarray:
        """Resample audio to a different sample rate; returns a float32 array."""
        return _resample(self.data, self.sample_rate, target_sample_rate)

    # --- Metering (handle form) ---
    #
    # Each of these reads the handle's own validated samples instead of
    # ``self.data`` (a fresh 4N-byte copy per access), so they skip the
    # per-call finiteness scan and the defensive copy their ``metering_*``
    # free-function twins pay. Parameters, order and defaults mirror those
    # twins; see :mod:`libsonare._features_metering`.

    @_entry("Audio.peak_db")
    def peak_db(self) -> float:
        """Sample-peak in dBFS over the buffer."""
        return self._audio_float("sonare_audio_peak_db", self._require_handle())

    @_entry("Audio.rms_db")
    def rms_db(self) -> float:
        """RMS level in dBFS over the buffer."""
        return self._audio_float("sonare_audio_rms_db", self._require_handle())

    @_entry("Audio.dc_offset")
    def dc_offset(self) -> float:
        """DC offset (mean) of the buffer in linear amplitude."""
        return self._audio_float("sonare_audio_dc_offset", self._require_handle())

    @_entry("Audio.crest_factor_db")
    def crest_factor_db(self) -> float:
        """Crest factor in dB (peak_db - rms_db)."""
        return self._audio_float("sonare_audio_crest_factor_db", self._require_handle())

    @_entry("Audio.silence_ratio")
    def silence_ratio(
        self,
        threshold_db: float = -45.0,
        frame_length: int = 1024,
        hop_length: int = 256,
    ) -> float:
        """Fraction of analysis frames whose RMS is below ``threshold_db``."""
        return self._audio_float(
            "sonare_audio_silence_ratio",
            self._require_handle(),
            _to_c_float(threshold_db, "threshold_db"),
            _to_c_int(frame_length, "frame_length"),
            _to_c_int(hop_length, "hop_length"),
        )

    @_entry("Audio.true_peak_dbtp")
    def true_peak_dbtp(self, oversample_factor: int | None = None) -> float:
        """Inter-sample (true) peak in dBFS.

        ``oversample_factor`` must be a power of two in [1, 16]; omit it for 4.
        """
        return self._audio_float(
            "sonare_audio_true_peak_dbtp",
            self._require_handle(),
            _to_c_int(_oversample_factor(oversample_factor), "oversample_factor"),
        )

    @_entry("Audio.detect_clipping")
    def detect_clipping(
        self,
        threshold_db: float | None = None,
        min_region_samples: int | None = None,
    ) -> ClippingReport:
        """Detect clipped runs; see :func:`libsonare.metering_detect_clipping`."""
        from ._ffi import SonareClippingResult

        c_threshold, c_min_region = _clipping_args(threshold_db, min_region_samples)
        handle = self._require_handle()
        out = SonareClippingResult()
        rc = self._lib.sonare_audio_detect_clipping(
            handle,
            _domain_float(c_threshold, "threshold_db"),
            _to_c_size_t(c_min_region, "min_region_samples"),
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

    @_entry("Audio.dynamic_range")
    def dynamic_range(
        self,
        window_sec: float | None = None,
        hop_sec: float | None = None,
        low_percentile: float | None = None,
        high_percentile: float | None = None,
    ) -> DynamicRangeReport:
        """Sliding-window dynamic range (high_percentile - low_percentile, in dB).

        Omit ``window_sec`` / ``hop_sec`` for the library default (window=3 s,
        hop=1 s); an explicit 0 is refused. ``low_percentile`` /
        ``high_percentile`` are ranks in [0, 1] with low at most high; omitted,
        they are 0.10 / 0.95, and ``0.0`` is a real request for the minimum-RMS
        window.
        """
        from ._ffi import SonareDynamicRangeResult

        window_value, hop_value, low_value, high_value = _dynamic_range_args(
            window_sec, hop_sec, low_percentile, high_percentile
        )
        handle = self._require_handle()
        out = SonareDynamicRangeResult()
        rc = self._lib.sonare_audio_dynamic_range(
            handle,
            _to_c_float(window_value, "window_sec"),
            _to_c_float(hop_value, "hop_sec"),
            _to_c_float(low_value, "low_percentile"),
            _to_c_float(high_value, "high_percentile"),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            windows = _from_c_float_array(out.window_rms_db, int(out.window_count))
            return DynamicRangeReport(
                dynamic_range_db=float(out.dynamic_range_db),
                low_percentile_db=float(out.low_percentile_db),
                high_percentile_db=float(out.high_percentile_db),
                window_rms_db=windows,
            )
        finally:
            self._lib.sonare_free_dynamic_range_result(ctypes.byref(out))

    @_entry("Audio.spectrum")
    def spectrum(
        self,
        n_fft: int | None = None,
        apply_octave_smoothing: bool = False,
        octave_fraction: int | None = None,
        db_ref: float | None = None,
        db_amin: float | None = None,
    ) -> SpectrumReport:
        """Welch-averaged magnitude / power / dB spectrum over the whole buffer.

        This is NOT a single-frame snapshot: the signal is split into
        Hann-windowed, 50%-overlapping ``n_fft``-length frames whose power
        spectra are averaged across the entire buffer (Welch's method), so
        transients are smeared by the averaging. For a true single-frame FFT
        use :meth:`spectrum_frame`.

        Omit ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin`` for the
        library defaults (2048 / 3 / 1.0 / kEpsilon); an explicit 0 is refused.
        """
        from ._ffi import SonareSpectrumResult

        n_fft_value, octave_fraction_value, db_ref_value, db_amin_value = _spectrum_args(
            n_fft, octave_fraction, db_ref, db_amin
        )
        handle = self._require_handle()
        out = SonareSpectrumResult()
        rc = self._lib.sonare_audio_spectrum(
            handle,
            _to_c_int(n_fft_value, "n_fft"),
            ctypes.c_int(1 if apply_octave_smoothing else 0),
            _to_c_int(octave_fraction_value, "octave_fraction"),
            _to_c_float(db_ref_value, "db_ref"),
            _to_c_float(db_amin_value, "db_amin"),
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

    @_entry("Audio.spectrum_frame")
    def spectrum_frame(
        self,
        frame_offset: int = 0,
        n_fft: int | None = None,
        apply_octave_smoothing: bool = False,
        octave_fraction: int | None = None,
        db_ref: float | None = None,
        db_amin: float | None = None,
    ) -> SpectrumReport:
        """True single-frame mono magnitude / power / dB spectrum (one Hann-windowed FFT).

        Unlike :meth:`spectrum` (Welch-averaged), this is a single
        ``n_fft``-length FFT for spectrum-analyzer "moment" snapshots. The
        frame spans ``[frame_offset, frame_offset + n_fft)``; samples past the
        end are zero-padded. ``frame_offset`` 0 is the first frame. Omit
        ``n_fft`` / ``octave_fraction`` / ``db_ref`` / ``db_amin`` for the
        library defaults (2048 / 3 / 1.0 / kEpsilon); an explicit 0 is refused.

        Cost per call is set by ``n_fft`` rather than by the buffer's length,
        so this may be polled over a long recording frame by frame -- and
        because it reads the handle's own samples, each poll costs no fresh
        copy of the buffer, unlike the ``metering_spectrum_frame`` free
        function over :attr:`data`.

        Raises:
            RuntimeError: If the audio has been closed.
        """
        from ._ffi import SonareSpectrumResult

        n_fft_value, octave_fraction_value, db_ref_value, db_amin_value = _spectrum_args(
            n_fft, octave_fraction, db_ref, db_amin
        )
        handle = self._require_handle()
        out = SonareSpectrumResult()
        rc = self._lib.sonare_audio_spectrum_frame(
            handle,
            _to_c_size_t(frame_offset, "frame_offset"),
            _to_c_int(n_fft_value, "n_fft"),
            ctypes.c_int(1 if apply_octave_smoothing else 0),
            _to_c_int(octave_fraction_value, "octave_fraction"),
            _to_c_float(db_ref_value, "db_ref"),
            _to_c_float(db_amin_value, "db_amin"),
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

    @_entry("Audio.ebur128_loudness_range_lu")
    def ebur128_loudness_range_lu(self) -> float:
        """EBU R128 / Tech 3342 Loudness Range (LRA) in LU."""
        out = ctypes.c_float(0.0)
        rc = self._lib.sonare_audio_ebur128_loudness_range_lu(
            self._require_handle(), ctypes.byref(out)
        )
        _check(rc)
        return float(out.value)

    @_entry("Audio.close")
    def close(self) -> None:
        """Free the underlying audio resource."""
        if self._handle:
            self._lib.sonare_audio_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> Audio:
        """Return this audio; leaving the ``with`` block closes it."""
        return self

    def __exit__(self, *args: object) -> None:
        """Close the audio when the ``with`` block exits."""
        self.close()

    def __del__(self) -> None:
        self.close()
