"""Python facade for live audio and MIDI device sessions."""

from __future__ import annotations

import contextlib
import ctypes
from dataclasses import dataclass
from typing import Literal

from ._engine_conversions import _c_string
from ._errors import _entry, _malformed_output, _refuse, _refuse_type, _require_open
from ._ffi_types_core import (
    SonareLiveAudioDevice,
    SonareLiveAudioDeviceList,
    SonareLiveMidiDevice,
    SonareLiveMidiDeviceList,
    SonareLiveSessionConfig,
)
from ._ffi_types_core import (
    SonareLiveSessionStatus as _SonareLiveSessionStatus,
)
from ._ffi_types_mastering_project import SonareMidiEventList
from ._project_model import ProjectMidiEvent, _midi_event_from_c
from ._runtime import (
    _C_INT_MAX,
    _check,
    _get_lib,
    _narrow_int,
    _optional_positive_int,
    _require_instance,
    _to_c_uint32,
    _utf8_arg,
)
from .engine import RealtimeEngine


@dataclass(frozen=True, slots=True)
class LiveAudioDevice:
    """One available audio device, selected by its stable ``uid``."""

    uid: str
    """Stable device UID used to select this device."""
    name: str
    """Device display name."""
    input_channels: int
    """Available input channels."""
    output_channels: int
    """Available output channels."""
    is_default_input: bool
    """Whether this is the current default input device."""
    is_default_output: bool
    """Whether this is the current default output device."""
    nominal_sample_rate: float
    """Nominal device sample rate in Hz."""


@dataclass(frozen=True, slots=True)
class LiveMidiDevice:
    """One available MIDI source, selected by its stable ``uid``."""

    uid: str
    """Stable CoreMIDI UID used to select this source."""
    name: str
    """MIDI source display name."""


@dataclass(frozen=True, slots=True)
class LiveSessionStatus:
    """Current connection state and counters for a live session."""

    running: bool
    """Whether a device is currently driving the engine."""
    device_lost: bool
    """Whether a selected audio device disappeared."""
    rate_changed: bool
    """Whether a device's nominal sample rate changed after opening."""
    sample_rate: int
    """Negotiated engine sample rate in Hz."""
    output_block_size: int
    """Negotiated output frames per block, or zero without output."""
    input_block_size: int
    """Negotiated input frames per block, or zero without input."""
    output_latency_ms: float
    """Device-reported output latency in milliseconds."""
    input_latency_ms: float
    """Device-reported input latency in milliseconds."""
    xrun_count: int
    """Number of audio device dropouts."""
    input_overrun_frames: int
    """Input frames discarded after priming, including high-water discards."""
    input_underrun_frames: int
    """Input frames repeated after priming because the ring ran low."""
    dropped_midi_events: int
    """MIDI events dropped because the capture queue was full."""


def _device_selection(
    value: object,
    name: str,
    record_type: type[LiveAudioDevice] | type[LiveMidiDevice],
    *,
    allow_default: bool,
) -> tuple[int, bytes | None]:
    allowed = (
        f"'default', a {record_type.__name__}, or None"
        if allow_default
        else f"a {record_type.__name__} or None"
    )
    if value is None:
        return 0, None
    if isinstance(value, str):
        if value == "default" and allow_default:
            return 1, None
        raise _refuse(f"{name} must be {allowed}")
    if not isinstance(value, record_type):
        raise _refuse_type(f"{name} must be {allowed}")
    uid = _utf8_arg(value.uid, f"{name}.uid")
    if not uid:
        raise _refuse(f"{name}.uid must not be empty")
    return 2, uid


def _audio_device_from_c(device: SonareLiveAudioDevice) -> LiveAudioDevice:
    return LiveAudioDevice(
        uid=_c_string(device.uid),
        name=_c_string(device.name),
        input_channels=int(device.input_channels),
        output_channels=int(device.output_channels),
        is_default_input=bool(device.is_default_input),
        is_default_output=bool(device.is_default_output),
        nominal_sample_rate=float(device.nominal_sample_rate),
    )


def _midi_device_from_c(device: SonareLiveMidiDevice) -> LiveMidiDevice:
    return LiveMidiDevice(uid=_c_string(device.uid), name=_c_string(device.name))


class LiveSession:
    """A running device connection that drives a :class:`RealtimeEngine`.

    The engine remains strongly referenced until the session object is released.
    Use :meth:`close` or a ``with`` statement to stop the devices.
    """

    _handle: ctypes.c_void_p | None = None

    @_entry("LiveSession")
    def __init__(
        self,
        engine: RealtimeEngine,
        *,
        audio_output: LiveAudioDevice | Literal["default"] | None = None,
        audio_input: LiveAudioDevice | Literal["default"] | None = None,
        midi_input: LiveMidiDevice | None = None,
        sample_rate: int | None = None,
        block_size: int | None = None,
        destination_id: int | None = None,
    ) -> None:
        self._handle: ctypes.c_void_p | None = None
        engine = _require_instance(engine, RealtimeEngine, "engine")
        self._engine = engine
        engine_handle = engine._require_handle()

        output_select, output_uid = _device_selection(
            audio_output, "audio_output", LiveAudioDevice, allow_default=True
        )
        input_select, input_uid = _device_selection(
            audio_input, "audio_input", LiveAudioDevice, allow_default=True
        )
        midi_select, midi_uid = _device_selection(
            midi_input, "midi_input", LiveMidiDevice, allow_default=False
        )

        config = SonareLiveSessionConfig()
        config.audio_output = output_select
        config.audio_output_uid = output_uid
        config.audio_input = input_select
        config.audio_input_uid = input_uid
        config.midi_input = midi_select
        config.midi_input_uid = midi_uid
        if sample_rate is not None:
            config.sample_rate = _optional_positive_int(sample_rate, "sample_rate", _C_INT_MAX)
        if block_size is not None:
            config.block_size = _narrow_int(block_size, "block_size", 1, _C_INT_MAX)
        if destination_id is not None:
            config.destination_id = _to_c_uint32(destination_id, "destination_id")

        handle = ctypes.c_void_p()
        _check(
            _get_lib().sonare_live_session_create(
                engine_handle, ctypes.byref(config), ctypes.byref(handle)
            ),
            flattened="config",
        )
        if not handle:
            raise _malformed_output("live session creation succeeded without a session")
        self._handle = handle

    @staticmethod
    @_entry("LiveSession.audio_devices")
    def audio_devices() -> list[LiveAudioDevice]:
        """Return a copied snapshot of the available audio devices."""
        lib = _get_lib()
        result = SonareLiveAudioDeviceList()
        try:
            _check(lib.sonare_live_audio_devices(ctypes.byref(result)))
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output(
                    "live audio device list has an inconsistent pointer and count"
                )
            return [_audio_device_from_c(result.items[index]) for index in range(count)]
        finally:
            lib.sonare_free_live_audio_device_list(ctypes.byref(result))

    @staticmethod
    @_entry("LiveSession.midi_sources")
    def midi_sources() -> list[LiveMidiDevice]:
        """Return a copied snapshot of the available MIDI sources."""
        lib = _get_lib()
        result = SonareLiveMidiDeviceList()
        try:
            _check(lib.sonare_live_midi_sources(ctypes.byref(result)))
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output(
                    "live MIDI source list has an inconsistent pointer and count"
                )
            return [_midi_device_from_c(result.items[index]) for index in range(count)]
        finally:
            lib.sonare_free_live_midi_device_list(ctypes.byref(result))

    @_entry("LiveSession.close")
    def close(self) -> None:
        """Stop the session on the control thread and release its handle.

        Repeated calls are safe. Finish other session and engine calls before closing.
        """
        handle = self._handle
        if handle is None:
            return
        self._handle = None
        _get_lib().sonare_live_session_destroy(handle)

    def __enter__(self) -> LiveSession:
        """Return this session for use in a context manager."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the session when the context ends."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "LiveSession")

    @_entry("LiveSession.status")
    def status(self) -> LiveSessionStatus:
        """Return the current connection state and counters."""
        result = _SonareLiveSessionStatus()
        _check(_get_lib().sonare_live_session_status(self._require_handle(), ctypes.byref(result)))
        return LiveSessionStatus(
            running=bool(result.running),
            device_lost=bool(result.device_lost),
            rate_changed=bool(result.rate_changed),
            sample_rate=int(result.sample_rate),
            output_block_size=int(result.output_block_size),
            input_block_size=int(result.input_block_size),
            output_latency_ms=float(result.output_latency_ms),
            input_latency_ms=float(result.input_latency_ms),
            xrun_count=int(result.xrun_count),
            input_overrun_frames=int(result.input_overrun_frames),
            input_underrun_frames=int(result.input_underrun_frames),
            dropped_midi_events=int(result.dropped_midi_events),
        )

    @_entry("LiveSession.captured_midi")
    def captured_midi(self) -> list[ProjectMidiEvent]:
        """Drain the captured channel-voice MIDI events in arrival order."""
        lib = _get_lib()
        result = SonareMidiEventList()
        try:
            _check(
                lib.sonare_live_session_captured_midi(self._require_handle(), ctypes.byref(result))
            )
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output("captured MIDI list has an inconsistent pointer and count")
            return [_midi_event_from_c(result.items[index]) for index in range(count)]
        finally:
            lib.sonare_free_midi_event_list(ctypes.byref(result))


__all__ = ["LiveAudioDevice", "LiveMidiDevice", "LiveSession", "LiveSessionStatus"]
