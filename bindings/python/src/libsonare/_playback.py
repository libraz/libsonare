"""Movie-audio playback renderer: upmix, binaural, night mode, bass management.

Wraps ``sonare_playback_*`` and ``sonare_hrtf_set_*``: decoded mono/stereo/5.1/7.1
PCM in, either speaker-layout audio (upmix, speaker calibration, bass
management) or binaural headphone audio (HRTF convolution, head tracking, room
model) out. Configuration follows nested-JSON-plus-keywords, the same split
mixer scenes use elsewhere in this binding: structural shape (target, room,
per-speaker layout) is a JSON document, scalar controls are keyword arguments.
"""

from __future__ import annotations

import contextlib
import ctypes
import json
from collections.abc import Mapping, Sequence
from typing import Any, cast

import numpy as np

from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _narrow_int,
    _planar_channel_arrays,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
)


def _playback_config_to_json(config: Mapping[str, Any] | str) -> bytes:
    return (config if isinstance(config, str) else json.dumps(dict(config))).encode("utf-8")


def _json_out_result(lib: ctypes.CDLL, out: ctypes.c_char_p) -> dict[str, Any]:
    try:
        if not out:
            return {}
        return cast(dict[str, Any], json.loads(ctypes.string_at(out).decode("utf-8")))
    finally:
        if out:
            lib.sonare_free_string(out)


class HrtfSet:
    """Opaque SHRF v1 HRTF set consumed by a headphones-target :class:`PlaybackRenderer`."""

    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def default(cls) -> HrtfSet:
        """Build the embedded default set (SADIE II KU100, native builds only)."""
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _check(lib.sonare_hrtf_set_create_default(ctypes.byref(handle)))
        return cls(handle, lib)

    @classmethod
    def from_bytes(cls, data: bytes) -> HrtfSet:
        """Parse SHRF v1 bytes into an HRTF set; malformed data raises :class:`SonareError`."""
        lib = _get_lib()
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data) if data else None
        handle = ctypes.c_void_p()
        _check(
            lib.sonare_hrtf_set_create_from_memory(
                buf, _to_c_size_t(len(data), "data"), ctypes.byref(handle)
            )
        )
        return cls(handle, lib)

    def close(self) -> None:
        if self._handle:
            self._lib.sonare_hrtf_set_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> HrtfSet:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()


class PlaybackRenderer:
    """Renders decoded movie audio to speakers or binaural headphones.

    ``config`` is the nested document (``target``, ``room``, per-speaker
    layout, ...); it may be a JSON string or a ``Mapping`` and may be complete
    or partial (omitted keys take the schema defaults). ``hrtf`` is used only
    for a headphones target; on native builds a ``None`` hrtf falls back to
    the embedded default set.
    """

    def __init__(
        self,
        config: Mapping[str, Any] | str,
        *,
        hrtf: HrtfSet | None = None,
        sample_rate: int = 48000,
        max_block_size: int = 1024,
    ) -> None:
        # Set first so a failed create leaves a valid attribute for
        # __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        _check(
            self._lib.sonare_playback_renderer_create_json(
                _playback_config_to_json(config),
                hrtf._handle if hrtf is not None else None,
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(max_block_size, "max_block_size"),
                ctypes.byref(self._handle),
            )
        )

    def close(self) -> None:
        if self._handle:
            self._lib.sonare_playback_renderer_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> PlaybackRenderer:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def reset(self) -> None:
        """Clear DSP state (filters, FIFOs, DRC, convolution history, pending drains).

        Configuration and head pose are kept.
        """
        _check(self._lib.sonare_playback_renderer_reset(self._handle))

    def set_config(self, config: Mapping[str, Any] | str) -> None:
        """Apply a complete configuration document.

        A prepare key (``target.kind``, ``target.layout``, ``input.layout``, ...)
        that differs from the current value raises :class:`SonareError`; realtime
        keys take effect at the next block boundary.
        """
        _check(
            self._lib.sonare_playback_renderer_set_config_json(
                self._handle, _playback_config_to_json(config)
            )
        )

    @property
    def config(self) -> dict[str, Any]:
        """The current complete configuration document."""
        out = ctypes.c_char_p()
        _check(self._lib.sonare_playback_renderer_config_json(self._handle, ctypes.byref(out)))
        return _json_out_result(self._lib, out)

    def set_head_orientation(
        self, yaw_deg: float, pitch_deg: float = 0.0, roll_deg: float = 0.0
    ) -> None:
        """Publish the listener head orientation; ignored by a speakers target.

        Callable from any single thread concurrently with
        :meth:`process_planar` / :meth:`process_interleaved`.

        Raises:
            SonareValueError: An angle is not finite or does not fit a C float.
        """
        _check_realtime(
            self._lib.sonare_playback_renderer_set_head_orientation(
                self._handle,
                _to_c_float(yaw_deg, "yaw_deg"),
                _to_c_float(pitch_deg, "pitch_deg"),
                _to_c_float(roll_deg, "roll_deg"),
            )
        )

    @property
    def input_channels(self) -> int:
        """Channel count of the active input layout.

        With ``input.layout = "auto"`` this follows the channel count of the
        most recent non-empty ``process_*`` call (2 before the first call).
        """
        out = ctypes.c_int()
        _check(
            self._lib.sonare_playback_renderer_input_channel_count(self._handle, ctypes.byref(out))
        )
        return int(out.value)

    @property
    def output_channels(self) -> int:
        """Channel count of the output target; fixed for the renderer's lifetime."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_playback_renderer_output_channel_count(self._handle, ctypes.byref(out))
        )
        return int(out.value)

    @property
    def latency_samples(self) -> int:
        """Renderer latency in frames (headphones: near ear).

        Depends only on the output target, the sample rate and the speaker
        distance compensation -- never on realtime keys or the input layout.
        """
        out = ctypes.c_int()
        _check(self._lib.sonare_playback_renderer_latency_samples(self._handle, ctypes.byref(out)))
        return int(out.value)

    @property
    def non_finite_discard_count(self) -> int:
        """Non-finite input samples replaced with 0 since creation."""
        out = ctypes.c_uint32()
        _check(
            self._lib.sonare_playback_renderer_non_finite_discard_count(
                self._handle, ctypes.byref(out)
            )
        )
        return int(out.value)

    def diagnostics(self) -> dict[str, Any]:
        """Inactive stages, per-stage latency, clamps, active input layout and counters."""
        out = ctypes.c_char_p()
        _check(self._lib.sonare_playback_renderer_diagnostics_json(self._handle, ctypes.byref(out)))
        return _json_out_result(self._lib, out)

    def process_planar(self, planes: Sequence[Sequence[float]] | np.ndarray) -> np.ndarray:
        """Render one planar block; returns a ``(output_channels, frames)`` array.

        ``planes`` is a sequence of equal-length channel buffers, or a 2-D
        ``(input_channels, frames)`` array. With a fixed input layout the plane
        count must equal :attr:`input_channels`; with ``"auto"`` it must be 1,
        2, 6 or 8, and a change switches the input layout at the start of this
        block without changing :attr:`latency_samples`.
        """
        in_arrays, in_ptrs, frames = _planar_channel_arrays(planes, subject="planes")
        out_channels = self.output_channels
        out_arrays = [(ctypes.c_float * frames)() for _ in range(out_channels)]
        out_ptr_type = ctypes.POINTER(ctypes.c_float) * out_channels
        out_ptrs = out_ptr_type(
            *[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in out_arrays]
        )
        _check_realtime(
            self._lib.sonare_playback_renderer_process_planar(
                self._handle,
                in_ptrs,
                _to_c_int(len(in_arrays), "in_channels"),
                out_ptrs,
                _to_c_int(out_channels, "out_channels"),
                _to_c_int(frames, "frames"),
            )
        )
        return np.stack(
            [np.frombuffer(array, dtype=np.float32, count=frames).copy() for array in out_arrays]
        )

    def process_interleaved(
        self, samples: Sequence[float] | list[float] | np.ndarray, in_channels: int
    ) -> np.ndarray:
        """Render one interleaved block; returns an interleaved ``float32`` array.

        Unlike :meth:`process_planar`, ``in_channels`` is explicit rather than
        inferred from the buffer's shape.
        """
        ch = _narrow_int(in_channels, "in_channels", _C_INT_MIN, _C_INT_MAX)
        if ch <= 0:
            raise SonareValueError("in_channels must be positive")
        c_in, total = _to_c_float_array(samples)
        if total % ch != 0:
            raise SonareValueError("interleaved samples length must be divisible by in_channels")
        frames = total // ch
        out_channels = self.output_channels
        out_len = frames * out_channels
        out_buf = np.zeros(out_len, dtype=np.float32)
        c_out = (
            (ctypes.c_float * out_len).from_buffer(out_buf) if out_len else (ctypes.c_float * 0)()
        )
        _check_realtime(
            self._lib.sonare_playback_renderer_process_interleaved(
                self._handle,
                c_in,
                _to_c_int(ch, "in_channels"),
                c_out,
                _to_c_int(out_channels, "out_channels"),
                _to_c_int(frames, "frames"),
            )
        )
        return out_buf


def render_playback(
    samples: Sequence[float] | list[float] | np.ndarray,
    *,
    channels: int,
    sample_rate: int,
    config: Mapping[str, Any] | str,
    hrtf: HrtfSet | None = None,
) -> np.ndarray:
    """Render a whole interleaved buffer offline, time-aligned with the input.

    The renderer latency is removed internally, so the result has the same
    frame count as ``samples`` / ``channels``.
    """
    ch = _narrow_int(channels, "channels", _C_INT_MIN, _C_INT_MAX)
    if ch <= 0:
        raise SonareValueError("channels must be positive")
    c_in, total = _to_c_float_array(samples)
    if total % ch != 0:
        raise SonareValueError("interleaved samples length must be divisible by channels")
    frames = total // ch
    lib = _get_lib()
    out_ptr = ctypes.POINTER(ctypes.c_float)()
    out_frames = ctypes.c_size_t()
    out_channels = ctypes.c_int()
    _check(
        lib.sonare_playback_render_interleaved(
            c_in,
            _to_c_size_t(frames, "frames"),
            _to_c_int(ch, "channels"),
            _to_c_int(sample_rate, "sample_rate"),
            _playback_config_to_json(config),
            hrtf._handle if hrtf is not None else None,
            ctypes.byref(out_ptr),
            ctypes.byref(out_frames),
            ctypes.byref(out_channels),
        )
    )
    try:
        return _from_c_float_array(out_ptr, int(out_frames.value) * int(out_channels.value))
    finally:
        if out_ptr:
            lib.sonare_free_playback_render(out_ptr)


class PlaybackLoudnessMeter:
    """Integrated-loudness meter for multichannel program material (1/2/6/8 ch).

    Feeds :meth:`push_interleaved` in chunks to measure ``loudness.program_lufs``
    ahead of a :class:`PlaybackRenderer`.
    """

    def __init__(self, channels: int, sample_rate: int) -> None:
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        self._channels = _narrow_int(channels, "channels", _C_INT_MIN, _C_INT_MAX)
        if self._channels <= 0:
            raise SonareValueError("channels must be positive")
        _check(
            self._lib.sonare_playback_loudness_meter_create(
                _to_c_int(self._channels, "channels"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(self._handle),
            )
        )

    def close(self) -> None:
        if self._handle:
            self._lib.sonare_playback_loudness_meter_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> PlaybackLoudnessMeter:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def push_interleaved(self, samples: Sequence[float] | list[float] | np.ndarray) -> None:
        """Feed interleaved frames of any length."""
        c_array, total = _to_c_float_array(samples)
        if total % self._channels != 0:
            raise SonareValueError("interleaved samples length must be divisible by channels")
        frames = total // self._channels
        _check(
            self._lib.sonare_playback_loudness_meter_push_interleaved(
                self._handle, c_array, _to_c_size_t(frames, "frames")
            )
        )

    @property
    def integrated_lufs(self) -> float:
        """Integrated loudness of everything pushed so far, in LUFS."""
        out = ctypes.c_float()
        _check(
            self._lib.sonare_playback_loudness_meter_integrated_lufs(
                self._handle, ctypes.byref(out)
            )
        )
        return float(out.value)
