"""Real-time streaming frame analyzer wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._runtime import (
    SonareStreamConfig,
    SonareStreamFrames,
    SonareStreamStats,
    StreamConfig,
    StreamFrames,
    StreamStats,
    _check,
    _get_lib,
    _to_c_float_array,
)


class StreamAnalyzer:
    """Thin Python wrapper around the native streaming frame analyzer handle.

    Feed audio chunks with :meth:`process`, then drain analyzed frames with
    :meth:`read_frames`. Query running estimates via :meth:`stats`.
    """

    def __init__(self, config: StreamConfig | None = None) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_stream_analyzer_create"):
            raise RuntimeError("libsonare was built without StreamAnalyzer support")
        raw = SonareStreamConfig()
        # Seed real-time defaults from the native layer, then override.
        _check(lib.sonare_stream_analyzer_config_default(ctypes.byref(raw)))
        if config is not None:
            raw.sample_rate = int(config.sample_rate)
            raw.n_fft = int(config.n_fft)
            raw.hop_length = int(config.hop_length)
            raw.n_mels = int(config.n_mels)
            raw.compute_mel = int(config.compute_mel)
            raw.compute_chroma = int(config.compute_chroma)
            raw.compute_onset = int(config.compute_onset)
            raw.emit_every_n_frames = int(config.emit_every_n_frames)
        handle = ctypes.c_void_p()
        _check(lib.sonare_stream_analyzer_create(ctypes.byref(raw), ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle

    def close(self) -> None:
        if self._handle is not None:
            _get_lib().sonare_stream_analyzer_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> StreamAnalyzer:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("StreamAnalyzer is closed")
        return self._handle

    def process(self, samples: Sequence[float] | list[float]) -> None:
        """Feed an audio chunk (cumulative offset tracked internally)."""
        c_array, length = _to_c_float_array(samples)
        _check(
            _get_lib().sonare_stream_analyzer_process(
                self._require_handle(), c_array, ctypes.c_size_t(length)
            )
        )

    def process_with_offset(
        self, samples: Sequence[float] | list[float], sample_offset: int
    ) -> None:
        """Feed an audio chunk with an explicit external sample offset."""
        c_array, length = _to_c_float_array(samples)
        _check(
            _get_lib().sonare_stream_analyzer_process_with_offset(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_size_t(int(sample_offset)),
            )
        )

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
        raw = SonareStreamFrames()
        _check(
            lib.sonare_stream_analyzer_read_frames(
                self._require_handle(), ctypes.c_size_t(int(max_frames)), ctypes.byref(raw)
            )
        )
        try:
            return _stream_frames_from_c(raw)
        finally:
            lib.sonare_free_stream_frames(ctypes.byref(raw))

    def reset(self, base_sample_offset: int = 0) -> None:
        """Reset analyzer state for a new stream."""
        _check(
            _get_lib().sonare_stream_analyzer_reset(
                self._require_handle(), ctypes.c_size_t(int(base_sample_offset))
            )
        )

    def stats(self) -> StreamStats:
        """Read the current statistics and progressive estimate snapshot."""
        raw = SonareStreamStats()
        _check(_get_lib().sonare_stream_analyzer_stats(self._require_handle(), ctypes.byref(raw)))
        return _stream_stats_from_c(raw)

    def frame_count(self) -> int:
        """Return the total number of frames processed."""
        out = ctypes.c_int()
        _check(
            _get_lib().sonare_stream_analyzer_frame_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def current_time(self) -> float:
        """Return the current stream time position in seconds."""
        out = ctypes.c_float()
        _check(
            _get_lib().sonare_stream_analyzer_current_time(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return float(out.value)

    def sample_rate(self) -> int:
        """Return the configured input sample rate in Hz."""
        out = ctypes.c_int()
        _check(
            _get_lib().sonare_stream_analyzer_sample_rate(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def set_expected_duration(self, duration_seconds: float) -> None:
        """Set the expected total duration (s) for pattern-lock timing."""
        _check(
            _get_lib().sonare_stream_analyzer_set_expected_duration(
                self._require_handle(), ctypes.c_float(float(duration_seconds))
            )
        )

    def set_normalization_gain(self, gain: float) -> None:
        """Set a normalization gain applied to input samples."""
        _check(
            _get_lib().sonare_stream_analyzer_set_normalization_gain(
                self._require_handle(), ctypes.c_float(float(gain))
            )
        )

    def set_tuning_ref_hz(self, ref_hz: float) -> None:
        """Set the A4 tuning reference (Hz) and rebuild the chroma filterbank."""
        _check(
            _get_lib().sonare_stream_analyzer_set_tuning_ref_hz(
                self._require_handle(), ctypes.c_float(float(ref_hz))
            )
        )


def _floats(ptr: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    if not ptr or count <= 0:
        return []
    return [float(ptr[i]) for i in range(count)]


def _ints(ptr: ctypes.POINTER(ctypes.c_int32), count: int) -> list[int]:
    if not ptr or count <= 0:
        return []
    return [int(ptr[i]) for i in range(count)]


def _stream_frames_from_c(raw: SonareStreamFrames) -> StreamFrames:
    n = int(raw.n_frames)
    n_mels = int(raw.n_mels)
    return StreamFrames(
        n_frames=n,
        n_mels=n_mels,
        timestamps=_floats(raw.timestamps, n),
        mel=_floats(raw.mel, n * n_mels),
        chroma=_floats(raw.chroma, n * 12),
        onset_strength=_floats(raw.onset_strength, n),
        rms_energy=_floats(raw.rms_energy, n),
        spectral_centroid=_floats(raw.spectral_centroid, n),
        spectral_flatness=_floats(raw.spectral_flatness, n),
        chord_root=_ints(raw.chord_root, n),
        chord_quality=_ints(raw.chord_quality, n),
        chord_confidence=_floats(raw.chord_confidence, n),
    )


def _stream_stats_from_c(raw: SonareStreamStats) -> StreamStats:
    return StreamStats(
        total_frames=int(raw.total_frames),
        total_samples=int(raw.total_samples),
        duration_seconds=float(raw.duration_seconds),
        bpm=float(raw.bpm),
        bpm_confidence=float(raw.bpm_confidence),
        bpm_candidate_count=int(raw.bpm_candidate_count),
        key=int(raw.key),
        key_minor=bool(raw.key_minor),
        key_confidence=float(raw.key_confidence),
        chord_root=int(raw.chord_root),
        chord_quality=int(raw.chord_quality),
        chord_confidence=float(raw.chord_confidence),
        chord_start_time=float(raw.chord_start_time),
        current_bar=int(raw.current_bar),
        bar_duration=float(raw.bar_duration),
        accumulated_seconds=float(raw.accumulated_seconds),
        used_frames=int(raw.used_frames),
        updated=bool(raw.updated),
    )
