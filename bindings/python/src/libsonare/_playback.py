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

from ._errors import _invalid_state, _not_supported
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _planar_channel_arrays,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
)


def _playback_config_to_json(config: Mapping[str, Any] | str) -> bytes:
    return _utf8_arg(config if isinstance(config, str) else json.dumps(dict(config)), "config")


def _require_playback(lib: ctypes.CDLL) -> None:
    """Raise a descriptive error rather than an AttributeError from ctypes.

    Playback is a real, independently disableable build option
    (BUILD_PLAYBACK), so an ABI-matched dylib built without it never sets up
    these symbols in _ffi_signatures_playback.py's hasattr-guarded configure
    step. Called at the top of every entry point that creates a handle;
    methods on an already-created handle need no re-check, since the same
    dylib backs every call on it.
    """
    if not hasattr(lib, "sonare_playback_renderer_create_json"):
        raise _not_supported(
            "the loaded libsonare does not expose playback (built without BUILD_PLAYBACK)"
        )


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
        _require_playback(lib)
        handle = ctypes.c_void_p()
        _check(lib.sonare_hrtf_set_create_default(ctypes.byref(handle)))
        return cls(handle, lib)

    @classmethod
    def from_bytes(cls, data: bytes) -> HrtfSet:
        """Parse SHRF v1 bytes into an HRTF set; malformed data raises :class:`SonareError`."""
        lib = _get_lib()
        _require_playback(lib)
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data) if data else None
        handle = ctypes.c_void_p()
        _check(
            lib.sonare_hrtf_set_create_from_memory(
                buf, _to_c_size_t(len(data), "data"), ctypes.byref(handle)
            )
        )
        return cls(handle, lib)

    def _require_handle(self) -> ctypes.c_void_p:
        """Return the live handle, or raise if this HRTF set has been closed.

        A closed handle is NULL (``close()`` overwrites it with a fresh
        ``c_void_p()``), and the C ABI's ``hrtf`` parameter treats a NULL as an
        explicit "use the embedded default" rather than as an error -- unlike
        every other handle in this binding, where a NULL reaching the C ABI is
        simply refused. Forwarding a closed handle's NULL as an ordinary NULL
        argument would silently render with the built-in HRTF set instead of
        the caller's own, with nothing to say so. Falsiness, not ``is None``,
        matching :meth:`Audio._require_handle`.
        """
        if not self._handle:
            raise _invalid_state("HrtfSet is closed")
        return self._handle

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
        _require_playback(self._lib)
        _check(
            self._lib.sonare_playback_renderer_create_json(
                _playback_config_to_json(config),
                hrtf._require_handle() if hrtf is not None else None,
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

    def output_channels(self) -> int:
        """Channel count of the output target; fixed for the renderer's lifetime."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_playback_renderer_output_channel_count(self._handle, ctypes.byref(out))
        )
        return int(out.value)

    def latency_samples(self) -> int:
        """Renderer latency in frames (headphones: near ear).

        Depends only on the output target, the sample rate and the speaker
        distance compensation -- never on realtime keys or the input layout.
        """
        out = ctypes.c_int()
        _check(self._lib.sonare_playback_renderer_latency_samples(self._handle, ctypes.byref(out)))
        return int(out.value)

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
        count must equal :meth:`input_channels`; with ``"auto"`` it must be 1,
        2, 6 or 8, and a change switches the input layout at the start of this
        block without changing :meth:`latency_samples`.
        """
        in_arrays, in_ptrs, frames = _planar_channel_arrays(planes, subject="planes")
        out_channels = self.output_channels()
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
        out_channels = self.output_channels()
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


@_guard_buffer("samples")
def render_playback(
    samples: Sequence[float] | list[float] | np.ndarray,
    *,
    channels: int,
    sample_rate: int,
    config: Mapping[str, Any] | str,
    hrtf: HrtfSet | None = None,
) -> np.ndarray:
    """Render a whole interleaved buffer offline, time-aligned with the input.

    Returns a ``(frames, channels)`` float32 ndarray -- ``channels`` is the
    output CHANNEL COUNT, which the caller cannot know in advance (it follows
    the render target ``config`` names, e.g. a 5.1 source rendered to
    headphones comes back stereo), so the shape itself is how this surface
    carries it, the same way :meth:`Project.bounce` does. Node's
    ``renderPlayback`` and WASM's carry the same fact as an explicit
    ``{samples, channels}`` result instead, since neither has a reshape-in-
    place idiom to lean on.

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
    _require_playback(lib)
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
            hrtf._require_handle() if hrtf is not None else None,
            ctypes.byref(out_ptr),
            ctypes.byref(out_frames),
            ctypes.byref(out_channels),
        )
    )
    try:
        flat = _from_c_float_array(out_ptr, int(out_frames.value) * int(out_channels.value))
        return flat.reshape(int(out_frames.value), int(out_channels.value))
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
        _require_playback(self._lib)
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

    @_guard_buffer("samples")
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

    def integrated_lufs(self) -> float:
        """Integrated loudness of everything pushed so far, in LUFS."""
        out = ctypes.c_float()
        _check(
            self._lib.sonare_playback_loudness_meter_integrated_lufs(
                self._handle, ctypes.byref(out)
            )
        )
        return float(out.value)
