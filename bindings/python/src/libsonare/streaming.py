"""Real-time streaming frame analyzer wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any, cast

import numpy as np

from . import _ffi_types_streaming as _ffi_streaming
from ._analysis_detection import _UNKNOWN_QUALITY_ORDINAL
from ._errors import _not_supported, _require_open
from ._narrowing import _narrow_double, _resolve_zero_default
from ._runtime import (
    _WINDOW_TYPES,
    Key,
    PitchClass,
    StreamBarChord,
    StreamChordChange,
    StreamConfig,
    StreamFrames,
    StreamFramesI16,
    StreamFramesU8,
    StreamPatternScore,
    StreamProgressiveEstimate,
    StreamQuantizeConfig,
    StreamStats,
    _check,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _to_c_float,
    _to_c_float_array,
    _to_c_size_t,
    _token_ordinal,
)
from ._types_analysis import CHORD_QUALITIES, _chord_symbol


def _quantize_config_to_c(
    config: StreamQuantizeConfig | None,
) -> _ffi_streaming.SonareStreamQuantizeConfig | None:
    """Marshal a :class:`StreamQuantizeConfig` to a C struct (``None`` stays ``None`` = defaults).

    The caller keeps the returned struct alive and passes ``ctypes.byref`` of it
    (or ``None`` for the default ranges) to the native ``*_ex`` entry point.
    """
    if config is None:
        return None
    raw = _ffi_streaming.SonareStreamQuantizeConfig()
    raw.mel_db_min = _narrow_double(config.mel_db_min, "config.mel_db_min")
    raw.mel_db_max = _narrow_double(config.mel_db_max, "config.mel_db_max")
    raw.onset_max = _narrow_double(config.onset_max, "config.onset_max")
    raw.rms_max = _narrow_double(config.rms_max, "config.rms_max")
    raw.centroid_max = _narrow_double(config.centroid_max, "config.centroid_max")
    return raw


class StreamAnalyzer:
    """Thin Python wrapper around the native streaming frame analyzer handle.

    Feed audio chunks with :meth:`process`, then drain analyzed frames with
    :meth:`read_frames`. Query running estimates via :meth:`stats`.

    One serialized producer thread may call :meth:`process`,
    :meth:`process_with_offset`, or :meth:`finalize` concurrently with one
    serialized consumer thread reading :attr:`available_frames`, a ``read_frames*``
    method, :meth:`stats`, :attr:`frame_count`, or :attr:`current_time_sec`. Native frame and
    stats publication is an allocation-free release/acquire handoff. Do not call
    :meth:`reset`, setters, :meth:`close`, or transfer ownership until both roles
    are stopped. If the pending ring is full, the newly produced output frame is
    dropped while analysis totals continue advancing.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    def __init__(self, config: StreamConfig) -> None:
        # Set first so a failed create (e.g. rejected malformed config) leaves a
        # valid attribute for __del__/close() instead of raising AttributeError.
        self._handle: ctypes.c_void_p | None = None
        if not isinstance(config, StreamConfig):
            raise TypeError(
                f"StreamAnalyzer: config must be a StreamConfig, got {type(config).__name__}"
            )
        lib = _get_lib()
        if not hasattr(lib, "sonare_stream_analyzer_create"):
            raise _not_supported("libsonare was built without StreamAnalyzer support")
        raw = _ffi_streaming.SonareStreamConfig()
        # Seed real-time defaults from the native layer, then override.
        _check(lib.sonare_stream_analyzer_config_default(ctypes.byref(raw)))
        # Integer fields are assigned bare: SonareStreamConfig is a CStruct, so
        # each field narrows onto its own declared type and refuses a fraction
        # an int() here would have truncated into a legal setting.
        raw.sample_rate = config.sample_rate
        raw.n_fft = config.n_fft
        raw.hop_length = config.hop_length
        raw.n_mels = config.n_mels
        raw.fmin_hz = _narrow_double(config.fmin_hz, "config.fmin_hz")
        # The C ABI reads 0 as Nyquist, so only an absent bound may reach it.
        raw.fmax_hz = _narrow_double(
            _resolve_zero_default(config.fmax_hz, "config.fmax_hz"), "config.fmax_hz"
        )
        raw.reference_hz = _narrow_double(config.reference_hz, "config.reference_hz")
        # The flags are the exception: that guard refuses a bool, so the 0/1 is
        # spelled out here.
        raw.compute_mel = int(config.compute_mel)
        raw.compute_chroma = int(config.compute_chroma)
        raw.compute_onset = int(config.compute_onset)
        raw.compute_spectral = int(config.compute_spectral)
        raw.emit_every_n_frames = config.emit_every_n_frames
        raw.magnitude_downsample = config.magnitude_downsample
        raw.max_pending_frames = config.max_pending_frames
        raw.max_progression_entries = config.max_progression_entries
        raw.key_update_interval_sec = _narrow_double(
            config.key_update_interval_sec, "config.key_update_interval_sec"
        )
        raw.bpm_update_interval_sec = _narrow_double(
            config.bpm_update_interval_sec, "config.bpm_update_interval_sec"
        )
        raw.window = _token_ordinal(config.window, _WINDOW_TYPES, "config.window")
        handle = ctypes.c_void_p()
        _check(lib.sonare_stream_analyzer_create(ctypes.byref(raw), ctypes.byref(handle)))
        self._handle = handle

    def close(self) -> None:
        """Release the native handle. Idempotent, so a double close is a no-op."""
        if self._handle is not None:
            _get_lib().sonare_stream_analyzer_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> StreamAnalyzer:
        """Return self so the object works as a context manager."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close on leaving the ``with`` block; never suppresses an exception."""
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "StreamAnalyzer")

    def process(self, samples: Sequence[float] | list[float]) -> None:
        """Feed an audio chunk (cumulative offset tracked internally).

        Feeding a finalized analyzer raises an invalid-state error; call
        :meth:`reset` first to start a new stream.
        """
        c_array, length = _to_c_float_array(samples)
        _check(
            _get_lib().sonare_stream_analyzer_process(
                self._require_handle(), c_array, _to_c_size_t(length, "length")
            )
        )

    def process_with_offset(
        self, samples: Sequence[float] | list[float], start_sample: int
    ) -> None:
        """Feed a chunk with a contiguous external sample offset.

        A gap, seek, a switch from :meth:`process`, or a chunk fed after
        :meth:`finalize` is rejected. Call :meth:`reset` first to discard the
        buffered partial frame and anchor a new timeline segment.
        """
        offset = _to_c_size_t(start_sample, "start_sample")
        c_array, length = _to_c_float_array(samples)
        _check(
            _get_lib().sonare_stream_analyzer_process_with_offset(
                self._require_handle(),
                c_array,
                _to_c_size_t(length, "length"),
                offset,
            )
        )

    def finalize(self) -> None:
        """Drain any high-rate resampler tail, then zero-pad the final frame.

        Repeating a successful call is a no-op, and a call that fails leaves the
        stream un-finalized so a retry resumes from the same point. Call
        :meth:`reset` before reusing the analyzer for another stream: more audio
        fed to a finalized analyzer is rejected rather than silently analyzed
        without the overlap context the finalized tail consumed.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_stream_analyzer_finalize"):
            raise _not_supported("libsonare was built without StreamAnalyzer.finalize support")
        _check(lib.sonare_stream_analyzer_finalize(self._require_handle()))

    @property
    def available_frames(self) -> int:
        """Return the number of frames available to read."""
        out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_stream_analyzer_available_frames(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    def read_frames(self, max_frames: int) -> StreamFrames:
        """Read up to ``max_frames`` analyzed frames (consumes them)."""
        lib = _get_lib()
        raw = _ffi_streaming.SonareStreamFrames()
        _check(
            lib.sonare_stream_analyzer_read_frames(
                self._require_handle(), _to_c_size_t(max_frames, "max_frames"), ctypes.byref(raw)
            )
        )
        try:
            return _stream_frames_from_c(raw)
        finally:
            lib.sonare_free_stream_frames(ctypes.byref(raw))

    def read_frames_u8(
        self, max_frames: int, quantize_config: StreamQuantizeConfig | None = None
    ) -> StreamFramesU8:
        """Read up to ``max_frames`` frames as 8-bit quantized arrays.

        Pass ``quantize_config`` to widen the quantization ranges for a stream
        louder or quieter than the defaults (``None`` keeps the defaults).
        """
        lib = _get_lib()
        raw = _ffi_streaming.SonareStreamFramesU8()
        qconfig = _quantize_config_to_c(quantize_config)
        _check(
            lib.sonare_stream_analyzer_read_frames_u8_ex(
                self._require_handle(),
                ctypes.byref(qconfig) if qconfig is not None else None,
                _to_c_size_t(max_frames, "max_frames"),
                ctypes.byref(raw),
            )
        )
        try:
            return _stream_frames_u8_from_c(raw)
        finally:
            lib.sonare_free_stream_frames_u8(ctypes.byref(raw))

    def read_frames_i16(
        self, max_frames: int, quantize_config: StreamQuantizeConfig | None = None
    ) -> StreamFramesI16:
        """Read up to ``max_frames`` frames as 16-bit quantized arrays.

        Pass ``quantize_config`` to widen the quantization ranges for a stream
        louder or quieter than the defaults (``None`` keeps the defaults).
        """
        lib = _get_lib()
        raw = _ffi_streaming.SonareStreamFramesI16()
        qconfig = _quantize_config_to_c(quantize_config)
        _check(
            lib.sonare_stream_analyzer_read_frames_i16_ex(
                self._require_handle(),
                ctypes.byref(qconfig) if qconfig is not None else None,
                _to_c_size_t(max_frames, "max_frames"),
                ctypes.byref(raw),
            )
        )
        try:
            return _stream_frames_i16_from_c(raw)
        finally:
            lib.sonare_free_stream_frames_i16(ctypes.byref(raw))

    def reset(self, start_sample: int = 0) -> None:
        """Reset analyzer state for a new stream starting at ``start_sample``."""
        _check(
            _get_lib().sonare_stream_analyzer_reset(
                self._require_handle(), _to_c_size_t(start_sample, "start_sample")
            )
        )

    def stats(self) -> StreamStats:
        """Read the current statistics and progressive estimate snapshot."""
        raw = _ffi_streaming.SonareStreamStats()
        lib = _get_lib()
        _check(lib.sonare_stream_analyzer_stats(self._require_handle(), ctypes.byref(raw)))
        try:
            return _stream_stats_from_c(raw)
        finally:
            if hasattr(lib, "sonare_free_stream_stats"):
                lib.sonare_free_stream_stats(ctypes.byref(raw))

    @property
    def frame_count(self) -> int:
        """Return the total number of frames processed."""
        out = ctypes.c_int()
        _check(
            _get_lib().sonare_stream_analyzer_frame_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    @property
    def current_time_sec(self) -> float:
        """Return the current stream time position in seconds."""
        out = ctypes.c_float()
        _check(
            _get_lib().sonare_stream_analyzer_current_time_sec(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return float(out.value)

    @property
    def sample_rate(self) -> int:
        """Return the configured input sample rate in Hz."""
        out = ctypes.c_int()
        _check(
            _get_lib().sonare_stream_analyzer_sample_rate(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def set_expected_duration(self, duration_sec: float) -> None:
        """Set the expected total duration (s) for pattern-lock timing."""
        _check(
            _get_lib().sonare_stream_analyzer_set_expected_duration(
                self._require_handle(), _to_c_float(duration_sec, "duration_sec")
            )
        )

    def set_normalization_gain(self, gain: float) -> None:
        """Set a normalization gain applied to input samples.

        ``gain`` is a linear factor within 0.01..100, assuming input in the
        conventional ±1 float domain. A value outside it raises instead of being
        clamped into range: the usual recipe (``target_level / measured_level``)
        can land outside for a buffer on another scale — an integer-scaled one
        asks for about 3e-4 — and no getter exposes the effective gain, so a
        clamped request would leave the analysis far off target with nothing to
        detect it by. Convert such a buffer before feeding it instead.
        """
        _check(
            _get_lib().sonare_stream_analyzer_set_normalization_gain(
                self._require_handle(), _to_c_float(gain, "gain")
            )
        )

    def set_reference_hz(self, reference_hz: float) -> None:
        """Set the A4 tuning reference (Hz) and rebuild the chroma filterbank.

        ``reference_hz`` must be within 220..880, the same range
        :attr:`StreamConfig.reference_hz` accepts at create time. A value
        outside it raises instead of being clamped into range.
        """
        _check(
            _get_lib().sonare_stream_analyzer_set_reference_hz(
                self._require_handle(), _to_c_float(reference_hz, "reference_hz")
            )
        )


# Feature bits of the native frame buffers.
_FEATURE_MEL = 1
_FEATURE_CHROMA = 2
_FEATURE_ONSET = 4
_FEATURE_SPECTRAL = 8


def _copy(ptr: object, count: int, ctype: Any, dtype: Any) -> np.ndarray:
    """Copy ``count`` elements at a C pointer into an independent ndarray."""
    if not ptr or count <= 0:
        return np.empty(0, dtype=dtype)
    raw_ptr = cast(Any, ptr)
    view = (ctype * count).from_address(ctypes.addressof(raw_ptr.contents))
    return np.frombuffer(memoryview(view), dtype=dtype, count=count).copy()


def _band_major(flat: np.ndarray, n_frames: int, n_bands: int) -> np.ndarray:
    """The native buffers are frame-major; the public layout is ``(bands, n_frames)``."""
    return np.ascontiguousarray(flat.reshape(n_frames, n_bands).T)


def _frame_arrays(raw: Any, ctype: Any, dtype: Any) -> dict[str, np.ndarray | None]:
    """The arrays every read path shares, ``None`` where the feature was not computed."""
    n = int(raw.n_frames)
    flags = int(raw.feature_flags)
    n_mels = int(raw.n_mels)
    n_chroma = int(raw.n_chroma)

    def present(bit: int, ptr: object, count: int) -> np.ndarray | None:
        return _copy(ptr, count, ctype, dtype) if flags & bit else None

    mel = present(_FEATURE_MEL, raw.mel, n * n_mels)
    chroma = present(_FEATURE_CHROMA, raw.chroma, n * n_chroma)
    return {
        "mel": None if mel is None else _band_major(mel, n, n_mels),
        "chroma": None if chroma is None else _band_major(chroma, n, n_chroma),
        "onset_strength": present(_FEATURE_ONSET, raw.onset_strength, n),
        "rms_energy": _copy(raw.rms_energy, n, ctype, dtype),
        "spectral_centroid": present(_FEATURE_SPECTRAL, raw.spectral_centroid, n),
        "spectral_flatness": present(_FEATURE_SPECTRAL, raw.spectral_flatness, n),
    }


def _chord_quality_token(quality: int) -> str:
    ordinal = quality if 0 <= quality < len(CHORD_QUALITIES) else _UNKNOWN_QUALITY_ORDINAL
    return CHORD_QUALITIES[ordinal]


def _chord_change(
    root: int, quality: int, start_sec: float, confidence: float
) -> StreamChordChange:
    token = _chord_quality_token(quality)
    return StreamChordChange(
        root=PitchClass(root),
        quality=token,
        name=_chord_symbol(PitchClass(root), token),
        start_sec=start_sec,
        confidence=confidence,
    )


def _chord_changes(ptr: object, count: int) -> list[StreamChordChange]:
    if not ptr or count <= 0:
        return []
    raw_ptr = cast(Any, ptr)
    return [
        _chord_change(
            int(raw_ptr[i].root),
            int(raw_ptr[i].quality),
            float(raw_ptr[i].start_sec),
            float(raw_ptr[i].confidence),
        )
        for i in range(count)
    ]


def _bar_chord(raw: Any) -> StreamBarChord:
    token = _chord_quality_token(int(raw.quality))
    root = PitchClass(int(raw.root))
    return StreamBarChord(
        bar_index=int(raw.bar_index),
        root=root,
        quality=token,
        name=_chord_symbol(root, token),
        start_sec=float(raw.start_sec),
        confidence=float(raw.confidence),
    )


def _bar_chords(ptr: object, count: int) -> list[StreamBarChord]:
    if not ptr or count <= 0:
        return []
    raw_ptr = cast(Any, ptr)
    return [_bar_chord(raw_ptr[i]) for i in range(count)]


def _voted_pattern(ptr: object, count: int) -> list[StreamBarChord | None]:
    """One entry per pattern position; ``None`` where no chord was voted (root -1 in C)."""
    if not ptr or count <= 0:
        return []
    raw_ptr = cast(Any, ptr)
    return [None if int(raw_ptr[i].root) < 0 else _bar_chord(raw_ptr[i]) for i in range(count)]


def _pattern_scores(ptr: object, count: int) -> list[StreamPatternScore]:
    if not ptr or count <= 0:
        return []
    raw_ptr = cast(Any, ptr)
    return [
        StreamPatternScore(
            name=bytes(raw_ptr[i].name).split(b"\0", 1)[0].decode("utf-8"),
            score=float(raw_ptr[i].score),
        )
        for i in range(count)
    ]


def _stream_frames_from_c(raw: _ffi_streaming.SonareStreamFrames) -> StreamFrames:
    n = int(raw.n_frames)
    chords = bool(int(raw.feature_flags) & _FEATURE_CHROMA)
    return StreamFrames(
        n_frames=n,
        timestamps_sec=_from_c_float_array(raw.timestamps_sec, n),
        **_frame_arrays(raw, ctypes.c_float, np.float32),
        chord_root=_from_c_int_array(raw.chord_root, n) if chords else None,
        chord_quality=_from_c_int_array(raw.chord_quality, n) if chords else None,
        chord_confidence=_from_c_float_array(raw.chord_confidence, n) if chords else None,
    )


def _stream_frames_u8_from_c(raw: _ffi_streaming.SonareStreamFramesU8) -> StreamFramesU8:
    n = int(raw.n_frames)
    return StreamFramesU8(
        n_frames=n,
        timestamps_sec=_from_c_float_array(raw.timestamps_sec, n),
        **_frame_arrays(raw, ctypes.c_uint8, np.uint8),
    )


def _stream_frames_i16_from_c(raw: _ffi_streaming.SonareStreamFramesI16) -> StreamFramesI16:
    n = int(raw.n_frames)
    return StreamFramesI16(
        n_frames=n,
        timestamps_sec=_from_c_float_array(raw.timestamps_sec, n),
        **_frame_arrays(raw, ctypes.c_int16, np.int16),
    )


def _estimate_from_c(raw: _ffi_streaming.SonareStreamStats) -> StreamProgressiveEstimate:
    has_bpm = float(raw.bpm) > 0
    key_root = int(raw.key)
    has_key = 0 <= key_root < len(PitchClass)
    chord_root = int(raw.chord_root)
    has_bar = int(raw.current_bar) >= 0
    pattern = bytes(raw.detected_pattern_name).split(b"\0", 1)[0].decode("utf-8")
    return StreamProgressiveEstimate(
        bpm=float(raw.bpm) if has_bpm else None,
        bpm_confidence=float(raw.bpm_confidence) if has_bpm else None,
        bpm_candidate_count=int(raw.bpm_candidate_count),
        key=Key(
            root=PitchClass(key_root),
            mode="minor" if raw.key_minor else "major",
            confidence=float(raw.key_confidence),
        )
        if has_key
        else None,
        chord=_chord_change(
            chord_root,
            int(raw.chord_quality),
            float(raw.chord_start_sec),
            float(raw.chord_confidence),
        )
        if 0 <= chord_root < len(PitchClass)
        else None,
        chord_progression=_chord_changes(raw.chord_progression, int(raw.chord_progression_count)),
        bar_chord_progression=_bar_chords(
            raw.bar_chord_progression, int(raw.bar_chord_progression_count)
        ),
        current_bar=int(raw.current_bar) if has_bar else None,
        bar_duration_sec=float(raw.bar_duration_sec) if has_bar else None,
        voted_pattern=_voted_pattern(raw.voted_pattern, int(raw.voted_pattern_count)),
        pattern_length=int(raw.pattern_length),
        detected_pattern_name=pattern or None,
        detected_pattern_score=float(raw.detected_pattern_score) if pattern else None,
        all_pattern_scores=_pattern_scores(
            raw.all_pattern_scores, int(raw.all_pattern_scores_count)
        ),
        accumulated_sec=float(raw.accumulated_sec),
        used_frames=int(raw.used_frames),
        updated=bool(raw.updated),
    )


def _stream_stats_from_c(raw: _ffi_streaming.SonareStreamStats) -> StreamStats:
    return StreamStats(
        total_frames=int(raw.total_frames),
        total_samples=int(raw.total_samples),
        duration_sec=float(raw.duration_sec),
        pending_frames=int(raw.pending_frames),
        dropped_output_frames=int(raw.dropped_output_frames),
        dropped_chord_progression_entries=int(raw.dropped_chord_progression_entries),
        dropped_bar_progression_entries=int(raw.dropped_bar_progression_entries),
        non_finite_discard_blocks=int(raw.non_finite_discard_blocks),
        estimate=_estimate_from_c(raw),
    )
