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

from ._errors import _entry, _refuse, _refuse_type, _require_open
from ._narrowing import _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _max_block_size,
    _measured_level,
    _narrow_int,
    _planar_channel_arrays,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
    _validate_samples,
)

PlaybackRendererConfig = dict[str, Any]
"""A nested playback renderer config document with camelCase keys.

Keys such as ``target``, ``room``, ``loudness`` and ``outputLimiter``; speaker roles
are spelled out (``left``, ``center``, ``leftSurround``, ...). Complete or partial; an
omitted key takes the schema default, and an earlier snake_case key or short role
token raises ``SonareError`` (InvalidFormat) naming the spelling to write. An unset
optional value (``input.channelMap``, a speaker's ``distanceM``,
``loudness.programLufs``) is an omitted key: ``None`` is refused as InvalidFormat
naming the path, and :meth:`PlaybackRenderer.config` leaves the key out. JSON text
is parsed by the caller (``json.loads``) before it is passed.
"""


def _playback_config_to_json(config: PlaybackRendererConfig) -> bytes:
    if not isinstance(config, Mapping):
        raise _refuse_type("config must be a mapping (parse JSON text with json.loads first)")
    return _utf8_arg(json.dumps(dict(config)), "config")


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

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("HrtfSet")
    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    @_entry("HrtfSet.default")
    def default(cls) -> HrtfSet:
        """Build the embedded default set (SADIE II KU100, native builds only)."""
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _check(lib.sonare_hrtf_set_create_default(ctypes.byref(handle)))
        return cls(handle, lib)

    @classmethod
    @_entry("HrtfSet.from_bytes")
    def from_bytes(cls, data: bytes) -> HrtfSet:
        """Parse SHRF v1 bytes into an HRTF set; malformed data raises :class:`SonareError`."""
        lib = _get_lib()
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
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
        return _require_open(self._handle, "HrtfSet")

    @_entry("HrtfSet.close")
    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle:
            self._lib.sonare_hrtf_set_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> HrtfSet:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()


class PlaybackRenderer:
    """Renders decoded movie audio to speakers or binaural headphones.

    ``config`` is the nested :data:`PlaybackRendererConfig` document (``target``,
    ``room``, per-speaker layout, ...); it may be complete or partial (omitted
    keys take the schema defaults). ``hrtf`` is used only
    for a headphones target; on native builds a ``None`` hrtf falls back to
    the embedded default set.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("PlaybackRenderer")
    def __init__(
        self,
        config: PlaybackRendererConfig,
        *,
        sample_rate: int,
        hrtf: HrtfSet | None = None,
        max_block_size: int | None = None,
    ) -> None:
        """Create a renderer at ``sample_rate``.

        ``max_block_size`` bounds the frames of every ``process_*`` call; omitted,
        the library default applies.
        """
        # Set first so a failed create leaves a valid attribute for
        # __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        _check(
            self._lib.sonare_playback_renderer_create_json(
                _playback_config_to_json(config),
                hrtf._require_handle() if hrtf is not None else None,
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(
                    _resolve_zero_default(max_block_size, "max_block_size"), "max_block_size"
                ),
                ctypes.byref(self._handle),
            )
        )

    @_entry("PlaybackRenderer.close")
    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle:
            self._lib.sonare_playback_renderer_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> PlaybackRenderer:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        """Return the live renderer handle, or reject a closed renderer."""
        return _require_open(self._handle, "PlaybackRenderer")

    @_entry("PlaybackRenderer.reset")
    def reset(self) -> None:
        """Clear DSP state (filters, FIFOs, DRC, convolution history, pending drains).

        Configuration and head pose are kept.
        """
        handle = self._require_handle()
        _check(self._lib.sonare_playback_renderer_reset(handle))

    @_entry("PlaybackRenderer.set_config")
    def set_config(self, config: PlaybackRendererConfig) -> None:
        """Apply a complete configuration document.

        A prepare key (``target.kind``, ``target.layout``, ``input.layout``, ...)
        that differs from the current value raises :class:`SonareError`; realtime
        keys take effect at the next block boundary.
        """
        handle = self._require_handle()
        _check(
            self._lib.sonare_playback_renderer_set_config_json(
                handle, _playback_config_to_json(config)
            )
        )

    @_entry("PlaybackRenderer.config")
    def config(self) -> PlaybackRendererConfig:
        """The current complete configuration document."""
        handle = self._require_handle()
        out = ctypes.c_char_p()
        _check(self._lib.sonare_playback_renderer_config_json(handle, ctypes.byref(out)))
        return _json_out_result(self._lib, out)

    @_entry("PlaybackRenderer.set_head_orientation")
    def set_head_orientation(
        self, yaw_deg: float, pitch_deg: float = 0.0, roll_deg: float = 0.0
    ) -> None:
        """Publish the listener head orientation; ignored by a speakers target.

        Callable from any single thread concurrently with
        :meth:`process_planar` / :meth:`process_interleaved`.

        Raises:
            SonareValueError: An angle is not finite or does not fit a C float.
        """
        handle = self._require_handle()
        _check_realtime(
            self._lib.sonare_playback_renderer_set_head_orientation(
                handle,
                _to_c_float(yaw_deg, "yaw_deg"),
                _to_c_float(pitch_deg, "pitch_deg"),
                _to_c_float(roll_deg, "roll_deg"),
            )
        )

    @property
    def max_block_size(self) -> int:
        """The largest frame count one ``process_*`` call accepts."""
        return _max_block_size(
            self._lib.sonare_playback_renderer_max_block_size, self._require_handle()
        )

    @property
    def num_input_channels(self) -> int:
        """Channel count of the active input layout.

        With ``input.layout = "auto"`` this follows the channel count of the
        most recent non-empty ``process_*`` call (2 before the first call).
        """
        handle = self._require_handle()
        out = ctypes.c_int()
        _check(self._lib.sonare_playback_renderer_num_input_channels(handle, ctypes.byref(out)))
        return int(out.value)

    @property
    def num_output_channels(self) -> int:
        """Channel count of the output target; fixed for the renderer's lifetime."""
        handle = self._require_handle()
        out = ctypes.c_int()
        _check(self._lib.sonare_playback_renderer_num_output_channels(handle, ctypes.byref(out)))
        return int(out.value)

    @property
    def latency_samples(self) -> int:
        """Renderer latency in frames (headphones: near ear).

        Depends only on the output target, the sample rate and the speaker
        distance compensation -- never on realtime keys or the input layout.
        """
        handle = self._require_handle()
        out = ctypes.c_int()
        _check(self._lib.sonare_playback_renderer_latency_samples(handle, ctypes.byref(out)))
        return int(out.value)

    @property
    def non_finite_discard_count(self) -> int:
        """Non-finite input samples replaced with 0 since creation."""
        handle = self._require_handle()
        out = ctypes.c_size_t()
        _check(
            self._lib.sonare_playback_renderer_non_finite_discard_count(handle, ctypes.byref(out))
        )
        return int(out.value)

    @_entry("PlaybackRenderer.diagnostics")
    def diagnostics(self) -> dict[str, Any]:
        """Inactive stages, per-stage latency, clamps, active input layout and counters."""
        handle = self._require_handle()
        out = ctypes.c_char_p()
        _check(self._lib.sonare_playback_renderer_diagnostics_json(handle, ctypes.byref(out)))
        return _json_out_result(self._lib, out)

    @_entry("PlaybackRenderer.process_planar")
    def process_planar(self, planes: Sequence[Sequence[float]] | np.ndarray) -> np.ndarray:
        """Render one planar block; returns a ``(num_output_channels, frames)`` array.

        ``planes`` is a sequence of equal-length channel buffers, or a 2-D
        ``(num_input_channels, frames)`` array. With a fixed input layout the plane
        count must equal :meth:`num_input_channels`; with ``"auto"`` it must be 1,
        2, 6 or 8, and a change switches the input layout at the start of this
        block without changing :meth:`latency_samples`.
        """
        handle = self._require_handle()
        in_arrays, in_ptrs, frames = _planar_channel_arrays(planes, subject="planes")
        out_channels = self.num_output_channels
        # The renderer writes each plane straight into its row of the returned array.
        out = np.zeros((out_channels, frames), dtype=np.float32)
        row_ptr = ctypes.POINTER(ctypes.c_float)
        out_ptrs = (row_ptr * out_channels)(
            *[out[row].ctypes.data_as(row_ptr) for row in range(out_channels)]
        )
        _check_realtime(
            self._lib.sonare_playback_renderer_process_planar(
                handle,
                in_ptrs,
                _to_c_int(len(in_arrays), "num_input_channels"),
                out_ptrs,
                _to_c_int(out_channels, "num_output_channels"),
                _to_c_int(frames, "frames"),
            )
        )
        return out

    @_entry("PlaybackRenderer.process_silence")
    def process_silence(self, frames: int) -> np.ndarray:
        """Render one planar block of silence; returns a ``(num_output_channels, frames)`` array.

        For a host with no input connected yet: every stage advances as for a block of zeros
        through the active input layout, and the layout is neither fixed nor switched, so under
        ``"auto"`` the first block carrying input still sets it. ``frames`` must be an integer
        in ``[0, max_block_size]``; 0 is a no-op.
        """
        handle = self._require_handle()
        count = _narrow_int(frames, "frames", 0, _C_INT_MAX)
        out_channels = self.num_output_channels
        out = np.zeros((out_channels, count), dtype=np.float32)
        row_ptr = ctypes.POINTER(ctypes.c_float)
        out_ptrs = (row_ptr * out_channels)(
            *[out[row].ctypes.data_as(row_ptr) for row in range(out_channels)]
        )
        _check_realtime(
            self._lib.sonare_playback_renderer_process_silence(
                handle, out_ptrs, _to_c_int(out_channels, "num_output_channels"), count
            )
        )
        return out

    @_entry("PlaybackRenderer.process_interleaved")
    def process_interleaved(
        self, samples: Sequence[float] | list[float] | np.ndarray, num_input_channels: int
    ) -> np.ndarray:
        """Render one interleaved block; returns an interleaved ``float32`` array.

        Unlike :meth:`process_planar`, ``num_input_channels`` is explicit rather than
        inferred from the buffer's shape.
        """
        handle = self._require_handle()
        ch = _narrow_int(num_input_channels, "num_input_channels", 1, _C_INT_MAX)
        c_in, total = _to_c_float_array(samples)
        if total % ch != 0:
            raise _refuse("samples length must be a multiple of num_input_channels")
        frames = total // ch
        out_channels = self.num_output_channels
        out_len = frames * out_channels
        out_buf = np.zeros(out_len, dtype=np.float32)
        c_out = (
            (ctypes.c_float * out_len).from_buffer(out_buf) if out_len else (ctypes.c_float * 0)()
        )
        _check_realtime(
            self._lib.sonare_playback_renderer_process_interleaved(
                handle,
                c_in,
                _to_c_int(ch, "num_input_channels"),
                c_out,
                _to_c_int(out_channels, "num_output_channels"),
                _to_c_int(frames, "frames"),
            )
        )
        return out_buf

    @_entry("PlaybackRenderer.render_offline")
    def render_offline(
        self, samples: Sequence[float] | list[float] | np.ndarray, num_input_channels: int
    ) -> np.ndarray:
        """Render a whole interleaved buffer offline; returns an interleaved ``float32`` array.

        The renderer is reset first and the result is time-aligned with the
        input, matching :func:`render_playback` for the same configuration.
        Unlike that function, the render runs through this handle, so
        :meth:`diagnostics` afterwards describes it. The result holds
        ``frames * num_output_channels()`` samples.
        """
        handle = self._require_handle()
        ch = _narrow_int(num_input_channels, "num_input_channels", 1, _C_INT_MAX)
        # An empty buffer is the library's to refuse, naming the frame count it derives.
        validated = _validate_samples(samples, allow_empty=True)
        c_in, total = _to_c_float_array(validated)
        if total % ch != 0:
            raise _refuse("samples length must be a multiple of num_input_channels")
        out_ptr = ctypes.POINTER(ctypes.c_float)()
        out_frames = ctypes.c_size_t()
        out_channels = ctypes.c_int()
        _check(
            self._lib.sonare_playback_renderer_render_offline(
                handle,
                c_in,
                _to_c_size_t(total // ch, "frames"),
                _to_c_int(ch, "num_input_channels"),
                ctypes.byref(out_ptr),
                ctypes.byref(out_frames),
                ctypes.byref(out_channels),
            )
        )
        try:
            return _from_c_float_array(out_ptr, int(out_frames.value) * int(out_channels.value))
        finally:
            if out_ptr:
                self._lib.sonare_free_floats(out_ptr)


@_entry("render_playback")
def render_playback(
    samples: Sequence[float] | list[float] | np.ndarray,
    *,
    num_channels: int,
    sample_rate: int,
    config: PlaybackRendererConfig,
    hrtf: HrtfSet | None = None,
) -> np.ndarray:
    """Render a whole interleaved buffer offline, time-aligned with the input.

    Returns a ``(frames, channels)`` float32 ndarray; the second axis is the
    output channel count, which the caller cannot know in advance (it follows
    the render target ``config`` names, e.g. a 5.1 source rendered to
    headphones comes back stereo), so the shape itself is how this surface
    carries it, the same way :meth:`Project.bounce` does. Node's
    ``renderPlayback`` and WASM's carry the same fact as an explicit
    ``{samples, numChannels}`` result instead, since neither has a reshape-in-
    place idiom to lean on.

    The renderer latency is removed internally, so the result has the same
    frame count as ``samples`` / ``num_channels``.
    """
    ch = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
    # An empty buffer is the library's to refuse, naming the frame count it derives.
    c_in, total = _to_c_float_array(_validate_samples(samples, allow_empty=True))
    if total % ch != 0:
        raise _refuse("interleaved samples length must be divisible by num_channels")
    frames = total // ch
    lib = _get_lib()
    out_ptr = ctypes.POINTER(ctypes.c_float)()
    out_frames = ctypes.c_size_t()
    out_channels = ctypes.c_int()
    _check(
        lib.sonare_playback_render_interleaved(
            c_in,
            _to_c_size_t(frames, "frames"),
            _to_c_int(ch, "num_channels"),
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
            lib.sonare_free_floats(out_ptr)


class PlaybackLoudnessMeter:
    """Integrated-loudness meter for multichannel program material (1/2/6/8 ch).

    Feeds :meth:`push_interleaved` in chunks to measure ``loudness.programLufs``
    ahead of a :class:`PlaybackRenderer`.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("PlaybackLoudnessMeter")
    def __init__(self, *, num_channels: int, sample_rate: int) -> None:
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        self._channels = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
        _check(
            self._lib.sonare_playback_loudness_meter_create(
                _to_c_int(self._channels, "num_channels"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(self._handle),
            )
        )

    @_entry("PlaybackLoudnessMeter.close")
    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle:
            self._lib.sonare_playback_loudness_meter_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> PlaybackLoudnessMeter:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        """Return the live meter handle, or reject a closed meter."""
        return _require_open(self._handle, "PlaybackLoudnessMeter")

    @_entry("PlaybackLoudnessMeter.push_interleaved")
    def push_interleaved(self, samples: Sequence[float] | list[float] | np.ndarray) -> None:
        """Feed interleaved frames of any length."""
        handle = self._require_handle()
        validated = _validate_samples(samples)
        c_array, total = _to_c_float_array(validated)
        if total % self._channels != 0:
            raise _refuse("interleaved samples length must be divisible by num_channels")
        frames = total // self._channels
        _check(
            self._lib.sonare_playback_loudness_meter_push_interleaved(
                handle, c_array, _to_c_size_t(frames, "frames")
            )
        )

    @property
    def integrated_lufs(self) -> float | None:
        """Integrated loudness of everything pushed so far, in LUFS.

        ``None`` before the first gating block is complete; ``-inf`` while nothing passes the gate.
        """
        handle = self._require_handle()
        out = ctypes.c_float()
        _check(self._lib.sonare_playback_loudness_meter_integrated_lufs(handle, ctypes.byref(out)))
        return _measured_level(out.value)
