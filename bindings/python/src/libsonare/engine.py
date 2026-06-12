"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
import json
import os
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import BinaryIO

import numpy as np

from ._project import (
    BuiltinSynthConfig,
    Sf2InstrumentConfig,
    SynthPatch,
    _synth_patch_arg,
)
from ._runtime import (
    AutomationCurve,
    AutomationPoint,
    ClipPageRequest,
    EngineBounceOptions,
    EngineBounceResult,
    EngineCaptureStatus,
    EngineClip,
    EngineFreezeOptions,
    EngineFreezeResult,
    EngineGraphConnection,
    EngineGraphMix,
    EngineGraphNode,
    EngineGraphNodeType,
    EngineGraphParameterBinding,
    EngineGraphSpec,
    EngineMarker,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    EngineTelemetry,
    EngineTelemetryError,
    EngineTelemetryType,
    MeterTelemetryRecord,
    ParameterInfo,
    SonareAutomationPoint,
    SonareClipPageRequest,
    SonareEngineBounceOptions,
    SonareEngineBounceResult,
    SonareEngineBus,
    SonareEngineCaptureBuffer,
    SonareEngineCaptureStatus,
    SonareEngineClip,
    SonareEngineFreezeOptions,
    SonareEngineFreezeResult,
    SonareEngineGraphConnection,
    SonareEngineGraphNode,
    SonareEngineGraphParameterBinding,
    SonareEngineGraphSpec,
    SonareEngineMarker,
    SonareEngineMetronomeConfig,
    SonareEngineMidiClipSchedule,
    SonareEngineMidiEvent,
    SonareEngineTelemetry,
    SonareEngineTrackLane,
    SonareEngineTrackSend,
    SonareEngineWarpAnchor,
    SonareMeterTelemetryRecord,
    SonareParameterInfo,
    SonareTransportState,
    TimeSignature,
    TransportState,
    _as_float32_buffer,
    _check,
    _from_c_float_array,
    _get_lib,
)

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
EXPECTED_ENGINE_ABI_VERSION = 3
_CAPTURE_SOURCE_VALUES = {"output": 0, "input": 1}


def _capture_source_value(source: str | int) -> int:
    if isinstance(source, str):
        try:
            return _CAPTURE_SOURCE_VALUES[source]
        except KeyError as exc:
            raise ValueError("capture source must be 'output' or 'input'") from exc
    value = int(source)
    if value in _CAPTURE_SOURCE_VALUES.values():
        return value
    raise ValueError("capture source must be 'output' or 'input'")


def _capture_source_name(source: int) -> str:
    return "input" if int(source) == _CAPTURE_SOURCE_VALUES["input"] else "output"


def _band_json_arg(band: Mapping[str, object] | str) -> bytes:
    return (band if isinstance(band, str) else json.dumps(dict(band))).encode("utf-8")


class ClipPageProvider:
    """Host-supplied paged audio source for realtime clip streaming."""

    def __init__(self, num_channels: int, num_samples: int, page_frames: int) -> None:
        self._handle: ctypes.c_void_p | None = None
        handle = ctypes.c_void_p()
        _check(
            _get_lib().sonare_clip_page_provider_create(
                int(num_channels), int(num_samples), int(page_frames), ctypes.byref(handle)
            )
        )
        self._handle = handle

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle is not None:
            _get_lib().sonare_clip_page_provider_destroy(handle)
            self._handle = None

    def destroy(self) -> None:
        self.close()

    def __enter__(self) -> ClipPageProvider:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("ClipPageProvider is closed")
        return self._handle

    def supply(self, page_index: int, channels: Sequence[Sequence[float]]) -> None:
        if not channels:
            raise ValueError("channels must not be empty")
        frames = len(channels[0])
        if frames <= 0:
            raise ValueError("channels must not be empty")
        arrays: list[ctypes.Array[ctypes.c_float]] = []
        ptr_values: list[ctypes.POINTER(ctypes.c_float)] = []
        for channel in channels:
            if len(channel) != frames:
                raise ValueError("all channels must have the same length")
            array = (ctypes.c_float * frames)(*channel)
            arrays.append(array)
            ptr_values.append(ctypes.cast(array, ctypes.POINTER(ctypes.c_float)))
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(ptr_values)
        ptrs = ptr_type(*ptr_values)
        _check(
            _get_lib().sonare_clip_page_provider_supply(
                self._require_handle(),
                int(page_index),
                ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
                len(ptr_values),
                frames,
            )
        )

    def clear(self, page_index: int) -> None:
        _check(_get_lib().sonare_clip_page_provider_clear(self._require_handle(), int(page_index)))


class FileClipPageProvider(ClipPageProvider):
    """File-backed float32 PCM page supplier for realtime clip streaming.

    The file format is raw little-endian interleaved float32 PCM.
    """

    def __init__(
        self,
        path: str | os.PathLike[str],
        *,
        num_channels: int,
        num_samples: int,
        page_frames: int,
        data_offset_bytes: int = 0,
    ) -> None:
        if num_channels <= 0 or num_samples <= 0 or page_frames <= 0:
            raise ValueError("num_channels, num_samples, and page_frames must be positive")
        super().__init__(num_channels, num_samples, page_frames)
        self._file: BinaryIO | None = None
        try:
            self._file = Path(path).open("rb")  # noqa: SIM115
        except BaseException:
            super().close()
            raise
        self.num_channels = int(num_channels)
        self.num_samples = int(num_samples)
        self.page_frames = int(page_frames)
        self.data_offset_bytes = int(data_offset_bytes)

    def close(self) -> None:
        file = getattr(self, "_file", None)
        if file is not None:
            file.close()
            self._file = None
        super().close()

    def supply_page(self, page_index: int) -> bool:
        if self._file is None:
            raise RuntimeError("FileClipPageProvider is closed")
        page = int(page_index)
        if page < 0:
            return False
        start_frame = page * self.page_frames
        if start_frame >= self.num_samples:
            return False
        frames = min(self.page_frames, self.num_samples - start_frame)
        frame_bytes = self.num_channels * np.dtype("<f4").itemsize
        self._file.seek(self.data_offset_bytes + start_frame * frame_bytes)
        raw = self._file.read(frames * frame_bytes)
        frames_read = len(raw) // frame_bytes
        if frames_read < frames:
            return False
        interleaved = np.frombuffer(raw[: frames_read * frame_bytes], dtype="<f4")
        channels = [interleaved[ch :: self.num_channels] for ch in range(self.num_channels)]
        self.supply(page, channels)
        return True

    def supply_request(self, request: ClipPageRequest) -> bool:
        return self.supply_page(int(request.sample) // self.page_frames)


class RealtimeEngine:
    """Thin Python wrapper around the native realtime engine handle."""

    def __init__(
        self,
        sample_rate: float = 48000.0,
        max_block_size: int = 128,
        *,
        command_capacity: int = 1024,
        telemetry_capacity: int = 1024,
    ) -> None:
        lib = _get_lib()
        abi_version = int(lib.sonare_engine_abi_version())
        if abi_version != EXPECTED_ENGINE_ABI_VERSION:
            raise RuntimeError(
                f"libsonare engine ABI mismatch: native binary reports {abi_version}, "
                f"expected {EXPECTED_ENGINE_ABI_VERSION}. The installed shared library is "
                "incompatible with this Python binding."
            )
        handle = ctypes.c_void_p()
        _check(lib.sonare_engine_create(ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle
        self._capture_arrays: list[ctypes.Array[ctypes.c_float]] = []
        self._capture_ptrs: ctypes.Array | None = None
        self._clip_page_providers: list[ClipPageProvider] = []
        self.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity)

    def close(self) -> None:
        if self._handle is not None:
            _get_lib().sonare_engine_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> RealtimeEngine:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("RealtimeEngine is closed")
        return self._handle

    def prepare(
        self,
        sample_rate: float,
        max_block_size: int,
        command_capacity: int = 1024,
        telemetry_capacity: int = 1024,
    ) -> None:
        lib = _get_lib()
        _check(
            lib.sonare_engine_prepare(
                self._require_handle(),
                float(sample_rate),
                int(max_block_size),
                int(command_capacity),
                int(telemetry_capacity),
            )
        )

    def play(self, render_frame: int = -1) -> None:
        _check(_get_lib().sonare_engine_play(self._require_handle(), int(render_frame)))

    def stop(self, render_frame: int = -1) -> None:
        _check(_get_lib().sonare_engine_stop(self._require_handle(), int(render_frame)))

    def seek_sample(self, timeline_sample: int, render_frame: int = -1) -> None:
        _check(
            _get_lib().sonare_engine_seek_sample(
                self._require_handle(), int(timeline_sample), int(render_frame)
            )
        )

    def seek_ppq(self, ppq: float, render_frame: int = -1) -> None:
        _check(
            _get_lib().sonare_engine_seek_ppq(self._require_handle(), float(ppq), int(render_frame))
        )

    def set_tempo(self, bpm: float) -> None:
        _check(_get_lib().sonare_engine_set_tempo(self._require_handle(), float(bpm)))

    def set_time_signature(self, numerator: int, denominator: int) -> None:
        _check(
            _get_lib().sonare_engine_set_time_signature(
                self._require_handle(), int(numerator), int(denominator)
            )
        )

    def sample_at_ppq(self, ppq: float) -> int:
        """Convert PPQ to a timeline sample using the engine tempo-map snapshot."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_sample_at_ppq"):
            raise RuntimeError("libsonare was built without sampleAtPpq support")
        out = ctypes.c_int64()
        _check(
            lib.sonare_engine_sample_at_ppq(self._require_handle(), float(ppq), ctypes.byref(out))
        )
        return int(out.value)

    def set_loop(self, start_ppq: float, end_ppq: float, enabled: bool = True) -> None:
        _check(
            _get_lib().sonare_engine_set_loop(
                self._require_handle(), float(start_ppq), float(end_ppq), int(enabled)
            )
        )

    def add_parameter(self, info: ParameterInfo) -> None:
        raw = SonareParameterInfo()
        raw.id = int(info.id)
        raw.name = _fixed_bytes(info.name, 64)
        raw.unit = _fixed_bytes(info.unit, 16)
        raw.min_value = float(info.min_value)
        raw.max_value = float(info.max_value)
        raw.default_value = float(info.default_value)
        raw.rt_safe = int(info.rt_safe)
        raw.default_curve = int(AutomationCurve(info.default_curve))
        _check(_get_lib().sonare_engine_add_parameter(self._require_handle(), ctypes.byref(raw)))

    def clear_parameters(self) -> None:
        """Remove all registered parameters and release their backing strings.

        Use before re-registering a parameter id to change its metadata
        (:meth:`add_parameter` rejects duplicate ids). Control-thread only; not
        realtime-safe.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_parameters"):
            raise RuntimeError("libsonare was built without parameter-registry support")
        _check(lib.sonare_engine_clear_parameters(self._require_handle()))

    def parameter_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_parameter_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def parameter_info_by_index(self, index: int) -> ParameterInfo:
        raw = SonareParameterInfo()
        _check(
            _get_lib().sonare_engine_parameter_info_by_index(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        return _parameter_from_c(raw)

    def parameter_info(self, id: int) -> ParameterInfo:
        raw = SonareParameterInfo()
        _check(
            _get_lib().sonare_engine_parameter_info(
                self._require_handle(), int(id), ctypes.byref(raw)
            )
        )
        return _parameter_from_c(raw)

    def set_automation_lane(self, param_id: int, points: Sequence[AutomationPoint]) -> None:
        raw_points = (SonareAutomationPoint * len(points))(
            *[
                SonareAutomationPoint(
                    float(point.ppq),
                    float(point.value),
                    int(AutomationCurve(point.curve_to_next)),
                )
                for point in points
            ]
        )
        _check(
            _get_lib().sonare_engine_set_automation_lane(
                self._require_handle(), int(param_id), raw_points, len(points)
            )
        )

    def automation_lane_count(self) -> int:
        out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_automation_lane_count(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    def set_markers(self, markers: Sequence[EngineMarker]) -> None:
        raw_markers = (SonareEngineMarker * len(markers))(
            *[_marker_to_c(marker) for marker in markers]
        )
        _check(
            _get_lib().sonare_engine_set_markers(self._require_handle(), raw_markers, len(markers))
        )

    def marker_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_marker_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def marker_by_index(self, index: int) -> EngineMarker:
        raw = SonareEngineMarker()
        _check(
            _get_lib().sonare_engine_marker_by_index(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        return _marker_from_c(raw)

    def marker(self, id: int) -> EngineMarker:
        raw = SonareEngineMarker()
        _check(_get_lib().sonare_engine_marker(self._require_handle(), int(id), ctypes.byref(raw)))
        return _marker_from_c(raw)

    def seek_marker(self, marker_id: int, render_frame: int = -1) -> None:
        _check(
            _get_lib().sonare_engine_seek_marker(
                self._require_handle(), int(marker_id), int(render_frame)
            )
        )

    def set_loop_from_markers(self, start_marker_id: int, end_marker_id: int) -> None:
        _check(
            _get_lib().sonare_engine_set_loop_from_markers(
                self._require_handle(), int(start_marker_id), int(end_marker_id)
            )
        )

    def set_metronome(self, config: EngineMetronomeConfig) -> None:
        raw = _metronome_to_c(config)
        _check(_get_lib().sonare_engine_set_metronome(self._require_handle(), ctypes.byref(raw)))

    def metronome(self) -> EngineMetronomeConfig:
        raw = SonareEngineMetronomeConfig()
        _check(_get_lib().sonare_engine_metronome(self._require_handle(), ctypes.byref(raw)))
        return _metronome_from_c(raw)

    def count_in_end_sample(self, start_sample: int, bars: int) -> int:
        out = ctypes.c_int64()
        _check(
            _get_lib().sonare_engine_count_in_end_sample(
                self._require_handle(), int(start_sample), int(bars), ctypes.byref(out)
            )
        )
        return int(out.value)

    def set_clips(self, clips: Sequence[EngineClip]) -> None:
        page_providers = [
            provider
            for provider in (getattr(clip, "page_provider", None) for clip in clips)
            if isinstance(provider, ClipPageProvider)
        ]
        raw_clips, _channel_arrays, _channel_ptrs, _warp_arrays = _clips_to_c(clips)
        _check(_get_lib().sonare_engine_set_clips(self._require_handle(), raw_clips, len(clips)))
        self._clip_page_providers = page_providers

    def clip_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_clip_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def set_track_lanes(self, lanes: Sequence[int | Mapping[str, object]]) -> None:
        raw = (SonareEngineTrackLane * len(lanes))()
        send_arrays: list[object] = []
        for i, lane in enumerate(lanes):
            if isinstance(lane, Mapping):
                raw[i].track_id = int(lane["track_id"] if "track_id" in lane else lane["trackId"])
                sends = lane.get("sends", [])
                if sends:
                    send_array = (SonareEngineTrackSend * len(sends))()
                    for send_index, send in enumerate(sends):
                        if not isinstance(send, Mapping):
                            raise TypeError("track lane send must be a mapping")
                        send_array[send_index].bus_id = int(
                            send["bus_id"] if "bus_id" in send else send["busId"]
                        )
                        send_array[send_index].level_db = float(
                            send["level_db"] if "level_db" in send else send.get("levelDb", 0.0)
                        )
                        send_array[send_index].enabled = 1 if bool(send.get("enabled", True)) else 0
                    raw[i].sends = send_array
                    raw[i].send_count = len(sends)
                    send_arrays.append(send_array)
            else:
                raw[i].track_id = int(lane)
        _check(_get_lib().sonare_engine_set_track_lanes(self._require_handle(), raw, len(lanes)))

    def set_track_buses(self, buses: Sequence[Mapping[str, object]]) -> None:
        raw = (SonareEngineBus * len(buses))()
        for i, bus in enumerate(buses):
            raw[i].bus_id = int(bus["bus_id"] if "bus_id" in bus else bus["busId"])
            raw[i].gain_db = float(bus["gain_db"] if "gain_db" in bus else bus.get("gainDb", 0.0))
        _check(_get_lib().sonare_engine_set_track_buses(self._require_handle(), raw, len(buses)))

    def set_bus_strip_json(self, bus_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_bus_strip_json(
                self._require_handle(),
                int(bus_id),
                scene_json.encode("utf-8"),
            )
        )

    def set_track_strip_json(self, track_id: int, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_json(
                self._require_handle(),
                int(track_id),
                scene_json.encode("utf-8"),
            )
        )

    def set_track_strip_eq_band(
        self, track_id: int, band_index: int, band: Mapping[str, object] | str
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                int(track_id),
                int(band_index),
                _band_json_arg(band),
            )
        )

    def set_track_strip_eq_band_json(self, track_id: int, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_eq_band_json(
                self._require_handle(),
                int(track_id),
                int(band_index),
                band_json.encode("utf-8"),
            )
        )

    def set_track_strip_insert_bypassed(
        self, track_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_track_strip_insert_bypassed(
                self._require_handle(),
                int(track_id),
                int(insert_index),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        )

    def set_master_strip_json(self, scene_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_json(
                self._require_handle(),
                scene_json.encode("utf-8"),
            )
        )

    def set_master_strip_eq_band(self, band_index: int, band: Mapping[str, object] | str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(),
                int(band_index),
                _band_json_arg(band),
            )
        )

    def set_master_strip_eq_band_json(self, band_index: int, band_json: str) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_eq_band_json(
                self._require_handle(),
                int(band_index),
                band_json.encode("utf-8"),
            )
        )

    def set_master_strip_insert_bypassed(
        self, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        _check(
            _get_lib().sonare_engine_set_master_strip_insert_bypassed(
                self._require_handle(),
                int(insert_index),
                1 if bypassed else 0,
                1 if reset_on_bypass else 0,
            )
        )

    def set_capture_buffer(self, num_channels: int, capacity_frames: int) -> None:
        if num_channels <= 0:
            raise ValueError("num_channels must be positive")
        if capacity_frames <= 0:
            raise ValueError("capacity_frames must be positive")
        self._capture_arrays = [
            (ctypes.c_float * int(capacity_frames))() for _ in range(int(num_channels))
        ]
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(self._capture_arrays)
        self._capture_ptrs = ptr_type(
            *[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in self._capture_arrays]
        )
        raw = SonareEngineCaptureBuffer()
        raw.channels = ctypes.cast(
            self._capture_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))
        )
        raw.num_channels = int(num_channels)
        raw.capacity_frames = int(capacity_frames)
        _check(
            _get_lib().sonare_engine_set_capture_buffer(self._require_handle(), ctypes.byref(raw))
        )

    def arm_capture(self, armed: bool = True) -> None:
        _check(_get_lib().sonare_engine_arm_capture(self._require_handle(), int(armed)))

    def set_capture_punch(self, start_sample: int, end_sample: int, enabled: bool = True) -> None:
        _check(
            _get_lib().sonare_engine_set_capture_punch(
                self._require_handle(), int(start_sample), int(end_sample), int(enabled)
            )
        )

    def set_capture_source(self, source: str | int) -> None:
        _check(
            _get_lib().sonare_engine_set_capture_source(
                self._require_handle(), _capture_source_value(source)
            )
        )

    def set_record_offset_samples(self, offset_samples: int) -> None:
        _check(
            _get_lib().sonare_engine_set_record_offset_samples(
                self._require_handle(), int(offset_samples)
            )
        )

    def set_input_monitor(self, enabled: bool, gain: float = 1.0) -> None:
        _check(
            _get_lib().sonare_engine_set_input_monitor(
                self._require_handle(), int(enabled), ctypes.c_float(float(gain))
            )
        )

    def reset_capture(self) -> None:
        _check(_get_lib().sonare_engine_reset_capture(self._require_handle()))

    def capture_status(self) -> EngineCaptureStatus:
        raw = SonareEngineCaptureStatus()
        _check(_get_lib().sonare_engine_capture_status(self._require_handle(), ctypes.byref(raw)))
        return EngineCaptureStatus(
            captured_frames=int(raw.captured_frames),
            overflow_count=int(raw.overflow_count),
            armed=bool(raw.armed),
            punch_enabled=bool(raw.punch_enabled),
            source=_capture_source_name(raw.source),
            record_offset_samples=int(raw.record_offset_samples),
        )

    def captured_audio(self) -> list[list[float]]:
        status = self.capture_status()
        capacity = len(self._capture_arrays[0]) if self._capture_arrays else 0
        frames = max(0, min(status.captured_frames, capacity))
        return [[float(array[i]) for i in range(frames)] for array in self._capture_arrays]

    def set_graph(self, spec: EngineGraphSpec) -> None:
        nodes = (SonareEngineGraphNode * len(spec.nodes))(
            *[_graph_node_to_c(node) for node in spec.nodes]
        )
        connections = (SonareEngineGraphConnection * len(spec.connections))(
            *[_graph_connection_to_c(connection) for connection in spec.connections]
        )
        bindings = list(spec.parameter_bindings or [])
        parameter_bindings = (SonareEngineGraphParameterBinding * len(bindings))(
            *[_graph_parameter_binding_to_c(binding) for binding in bindings]
        )
        raw = SonareEngineGraphSpec()
        raw.nodes = nodes
        raw.node_count = len(spec.nodes)
        raw.connections = connections
        raw.connection_count = len(spec.connections)
        raw.parameter_bindings = parameter_bindings
        raw.parameter_binding_count = len(bindings)
        raw.input_node = _fixed_bytes(spec.input_node, 64)
        raw.output_node = _fixed_bytes(spec.output_node, 64)
        raw.num_channels = int(spec.num_channels)
        _check(_get_lib().sonare_engine_set_graph(self._require_handle(), ctypes.byref(raw)))

    def graph_node_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_graph_node_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def graph_connection_count(self) -> int:
        out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_graph_connection_count(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    def process(self, channels: Sequence[Sequence[float]]) -> list[list[float]]:
        arrays, ptrs, frame_count = self._channel_arrays(channels)
        _check(
            _get_lib().sonare_engine_process(
                self._require_handle(), ptrs, len(arrays), int(frame_count)
            )
        )
        return [
            np.frombuffer(array, dtype=np.float32, count=frame_count).tolist() for array in arrays
        ]

    def process_with_monitor(
        self, channels: Sequence[Sequence[float]]
    ) -> tuple[list[list[float]], list[list[float]]]:
        arrays, ptrs, frame_count = self._channel_arrays(channels)
        # `(c_float * N)()` already zero-initialises; the previous
        # `*([0.0] * N)` built a throwaway Python list and varargs-marshalled it.
        monitor_arrays = [(ctypes.c_float * frame_count)() for _ in arrays]
        monitor_ptrs = (ctypes.POINTER(ctypes.c_float) * len(monitor_arrays))(
            *[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in monitor_arrays]
        )
        _check(
            _get_lib().sonare_engine_process_with_monitor(
                self._require_handle(), ptrs, monitor_ptrs, len(arrays), int(frame_count)
            )
        )
        output = [
            np.frombuffer(array, dtype=np.float32, count=frame_count).tolist() for array in arrays
        ]
        monitor = [
            np.frombuffer(array, dtype=np.float32, count=frame_count).tolist()
            for array in monitor_arrays
        ]
        return output, monitor

    def render_offline(
        self, channels: Sequence[Sequence[float]], *, block_size: int = 128
    ) -> list[list[float]]:
        arrays, ptrs, frame_count = self._channel_arrays(channels)
        _check(
            _get_lib().sonare_engine_render_offline(
                self._require_handle(), ptrs, len(arrays), int(frame_count), int(block_size)
            )
        )
        return [
            np.frombuffer(array, dtype=np.float32, count=frame_count).tolist() for array in arrays
        ]

    def bounce_offline(self, options: EngineBounceOptions) -> EngineBounceResult:
        lib = _get_lib()
        raw_options = SonareEngineBounceOptions()
        # Seed native defaults first (mirrors StreamAnalyzer.__init__) so any
        # field the caller leaves at the dataclass sentinel still tracks the C
        # layer's defaults instead of a hardcoded Python copy that can drift.
        _check(lib.sonare_engine_bounce_options_default(ctypes.byref(raw_options)))
        raw_options.total_frames = int(options.total_frames)
        raw_options.block_size = int(options.block_size)
        raw_options.num_channels = int(options.num_channels)
        raw_options.target_sample_rate = int(options.target_sample_rate)
        raw_options.source_sample_rate = int(options.source_sample_rate)
        raw_options.normalize_lufs = int(options.normalize_lufs)
        raw_options.target_lufs = float(options.target_lufs)
        raw_options.dither = int(options.dither)
        raw_options.dither_bits = int(options.dither_bits)
        raw_options.dither_seed = int(options.dither_seed)
        raw_result = SonareEngineBounceResult()
        _check(
            lib.sonare_engine_bounce_offline(
                self._require_handle(), ctypes.byref(raw_options), ctypes.byref(raw_result)
            )
        )
        try:
            interleaved = _from_c_float_array(
                raw_result.interleaved, int(raw_result.sample_count)
            ).tolist()
        finally:
            if raw_result.interleaved:
                lib.sonare_free_bounce_result(ctypes.byref(raw_result))
        return EngineBounceResult(
            interleaved=interleaved,
            frames=int(raw_result.frames),
            num_channels=int(raw_result.num_channels),
            sample_rate=int(raw_result.sample_rate),
            integrated_lufs=float(raw_result.integrated_lufs),
        )

    def freeze_offline(self, options: EngineFreezeOptions) -> EngineFreezeResult:
        raw_options = SonareEngineFreezeOptions()
        raw_options.total_frames = int(options.total_frames)
        raw_options.block_size = int(options.block_size)
        raw_options.num_channels = int(options.num_channels)
        raw_options.clip_id = int(options.clip_id)
        raw_options.start_ppq = float(options.start_ppq)
        raw_options.gain = float(options.gain)
        raw_result = SonareEngineFreezeResult()
        _check(
            _get_lib().sonare_engine_freeze_offline(
                self._require_handle(), ctypes.byref(raw_options), ctypes.byref(raw_result)
            )
        )
        return EngineFreezeResult(
            clip_id=int(raw_result.clip_id),
            frames=int(raw_result.frames),
            num_channels=int(raw_result.num_channels),
        )

    def drain_telemetry(self, max_records: int = 1024) -> list[EngineTelemetry]:
        if max_records <= 0:
            return []
        raw = (SonareEngineTelemetry * int(max_records))()
        written = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_drain_telemetry(
                self._require_handle(), raw, int(max_records), ctypes.byref(written)
            )
        )
        return [_telemetry_from_c(raw[i]) for i in range(written.value)]

    def pop_clip_page_request(self) -> ClipPageRequest | None:
        raw = SonareClipPageRequest()
        has_request = ctypes.c_int()
        _check(
            _get_lib().sonare_engine_pop_clip_page_request(
                self._require_handle(), ctypes.byref(raw), ctypes.byref(has_request)
            )
        )
        if not has_request.value:
            return None
        return ClipPageRequest(
            clip_id=int(raw.clip_id), channel=int(raw.channel), sample=int(raw.sample)
        )

    def drain_meter_telemetry(self, max_records: int = 1024) -> list[MeterTelemetryRecord]:
        """Drain pending meter telemetry records published by the engine."""
        if max_records <= 0:
            return []
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_drain_meter_telemetry"):
            raise RuntimeError("libsonare was built without meter-telemetry support")
        raw = (SonareMeterTelemetryRecord * int(max_records))()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_engine_drain_meter_telemetry(
                self._require_handle(), raw, int(max_records), ctypes.byref(written)
            )
        )
        return [_meter_telemetry_from_c(raw[i]) for i in range(written.value)]

    def set_parameter(self, param_id: int, value: float, render_frame: int = -1) -> None:
        """Push a live parameter value to the engine (immediate jump).

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_parameter"):
            raise RuntimeError("libsonare was built without live-parameter support")
        _check(
            lib.sonare_engine_set_parameter(
                self._require_handle(), int(param_id), float(value), int(render_frame)
            )
        )

    def set_parameter_smoothed(self, param_id: int, value: float, render_frame: int = -1) -> None:
        """Push a live parameter value to the engine using a smoothed ramp."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_parameter_smoothed"):
            raise RuntimeError("libsonare was built without live-parameter support")
        _check(
            lib.sonare_engine_set_parameter_smoothed(
                self._require_handle(), int(param_id), float(value), int(render_frame)
            )
        )

    def set_solo_mute(
        self, lane_index: int, solo: bool, mute: bool, render_frame: int = -1
    ) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_solo_mute"):
            raise RuntimeError("libsonare was built without realtime mixer support")
        _check(
            lib.sonare_engine_set_solo_mute(
                self._require_handle(),
                int(lane_index),
                1 if solo else 0,
                1 if mute else 0,
                int(render_frame),
            )
        )

    def set_midi_clips(self, clips: Sequence[EngineMidiClipSchedule]) -> None:
        """Replace the realtime MIDI clip snapshot with compiled schedules."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_clips"):
            raise RuntimeError("libsonare was built without realtime MIDI clip support")
        event_arrays: list[ctypes.Array[SonareEngineMidiEvent]] = []
        raw_clips = (SonareEngineMidiClipSchedule * len(clips))()
        for index, clip in enumerate(clips):
            raw_events = (SonareEngineMidiEvent * len(clip.events))()
            for event_index, event in enumerate(clip.events):
                raw_events[event_index] = SonareEngineMidiEvent(
                    int(event.render_frame),
                    int(event.word0),
                    int(event.word1),
                    int(event.word2),
                    int(event.word3),
                    int(event.word_count),
                    int(event.group),
                    0,
                    int(event.sysex_handle),
                )
            event_arrays.append(raw_events)
            raw_clips[index] = SonareEngineMidiClipSchedule(
                int(clip.id),
                int(clip.track_id),
                int(clip.start_sample),
                float(clip.start_ppq),
                int(clip.length_samples),
                1 if clip.loop else 0,
                int(clip.loop_length_samples),
                int(clip.destination_id if clip.destination_id != 0 else clip.track_id),
                raw_events,
                len(clip.events),
            )
        _check(
            lib.sonare_engine_set_midi_clips(
                self._require_handle(), raw_clips, ctypes.c_size_t(len(clips))
            )
        )
        _ = event_arrays

    def push_midi_cc(
        self,
        destination_id: int,
        group: int,
        channel: int,
        controller: int,
        value: int,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate (live) MIDI control change to a MIDI destination.

        Values are 7-bit (``controller`` / ``value`` in 0..127); ``channel`` and
        ``group`` in 0..15. ``render_frame`` is the render-frame time to apply,
        or ``-1`` for immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_cc(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(controller),
                int(value),
                int(render_frame),
            )
        )

    def push_midi_panic(self, render_frame: int = -1) -> None:
        """Queue a MIDI panic (all-notes-off) releasing every sounding note.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_panic"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_push_midi_panic(self._require_handle(), int(render_frame)))

    # -- live MIDI instruments / CC bindings / input source -----------------

    def set_builtin_instrument(
        self, config: BuiltinSynthConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind the built-in polyphonic synth to ``destination_id`` (default 0).

        ``config`` is a :class:`BuiltinSynthConfig` patch; ``None`` installs the
        default sine patch. After binding, MIDI events routed to that
        destination render through the built-in synth.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_builtin_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        cfg = (config if config is not None else BuiltinSynthConfig())._to_c()
        _check(
            lib.sonare_engine_set_builtin_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(cfg)
            )
        )

    def set_synth_instrument(
        self, patch: SynthPatch | str | None = None, destination_id: int = 0
    ) -> None:
        """Bind the patch-driven NativeSynth to ``destination_id`` (default 0).

        ``patch`` is a :class:`SynthPatch`, a preset name string
        (``"saw-lead"`` or ``"va:saw-lead"``; see :func:`synth_preset_names`),
        or ``None`` for the default subtractive patch. The patch resolves
        exactly like :meth:`Project.bounce_with_synth_instrument`. After
        binding, live MIDI input and scheduled MIDI clips routed to that
        destination render through the synth. Raises :class:`SonareError` for
        an unknown preset name.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_synth_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        c_patch = _synth_patch_arg(patch)._to_c()
        _check(
            lib.sonare_engine_set_synth_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(c_patch)
            )
        )

    def load_soundfont(self, data: bytes | bytearray | memoryview) -> None:
        """Load (parse) SoundFont 2 bytes into the engine.

        Replaces any previously loaded SoundFont (already-bound SF2 instruments
        keep the SoundFont they were created with); the input buffer is not
        referenced after the call. Raises :class:`SonareError` on malformed
        input.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_load_soundfont"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        buf = bytes(data)
        if not buf:
            raise ValueError("SoundFont data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_engine_load_soundfont(
                self._require_handle(), c_data, ctypes.c_size_t(len(buf))
            )
        )

    def set_sf2_instrument(
        self, config: Sf2InstrumentConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind a GS-compatible SoundFont player to ``destination_id`` (default 0).

        ``config`` is an :class:`Sf2InstrumentConfig` patch; ``None`` installs
        the defaults. After binding, live MIDI input and scheduled MIDI clips
        routed to that destination render through the player (16 MIDI
        channels, channel 10 drums, GS NRPN part edits, GS/GM SysEx resets).
        Without a loaded SoundFont (:meth:`load_soundfont`) — or for programs
        the SoundFont does not cover — notes play through the built-in
        synthesizer GM fallback bank (the data-free floor).
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_sf2_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        cfg = (config if config is not None else Sf2InstrumentConfig())._to_c()
        _check(
            lib.sonare_engine_set_sf2_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(cfg)
            )
        )

    def clear_midi_instrument(self, destination_id: int = 0) -> None:
        """Clear any realtime instrument bound to ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_instrument(self._require_handle(), int(destination_id)))

    def midi_instrument_count(self) -> int:
        """Return the number of bound realtime MIDI instruments."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_instrument_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(lib.sonare_engine_midi_instrument_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def bind_midi_cc(
        self,
        channel: int,
        controller: int,
        param_id: int,
        min_value: float = 0.0,
        max_value: float = 1.0,
    ) -> None:
        """Bind a live MIDI CC to an engine automation parameter.

        Incoming CC values on ``channel`` / ``controller`` are mapped onto
        ``[min_value, max_value]`` and applied to ``param_id``.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_bind_midi_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_bind_midi_cc(
                self._require_handle(),
                int(channel),
                int(controller),
                int(param_id),
                float(min_value),
                float(max_value),
            )
        )

    def clear_midi_cc_bindings(self) -> None:
        """Clear all live MIDI CC to parameter bindings."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_cc_bindings"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_cc_bindings(self._require_handle()))

    def midi_cc_binding_count(self) -> int:
        """Return the number of live MIDI CC bindings."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_cc_binding_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(lib.sonare_engine_midi_cc_binding_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def clear_midi_fx(self, destination_id: int = 0) -> None:
        """Clear the live MIDI-FX insert for ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_fx"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_fx(self._require_handle(), int(destination_id)))

    def set_midi_input_source(self, destination_id: int = 0) -> None:
        """Enable the engine-owned live MIDI input source for ``destination_id``.

        Hosts can then push timestamped events with
        :meth:`push_midi_input_note_on` / ``_note_off`` / ``_cc``; the engine
        drains them at block boundaries.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_input_source"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_set_midi_input_source(self._require_handle(), int(destination_id)))

    def clear_midi_input_source(self) -> None:
        """Clear the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_input_source"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_input_source(self._require_handle()))

    def midi_input_pending_count(self) -> int:
        """Number of queued events in the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_input_pending_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(
            lib.sonare_engine_midi_input_pending_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def push_midi_input_note_on(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a note-on into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_note_on"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_note_on(
                self._require_handle(),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(port_time_samples),
            )
        )

    def push_midi_input_note_off(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a note-off into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_note_off"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_note_off(
                self._require_handle(),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(port_time_samples),
            )
        )

    def push_midi_input_cc(
        self,
        group: int,
        channel: int,
        controller: int,
        value: int,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a control change into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_cc(
                self._require_handle(),
                int(group),
                int(channel),
                int(controller),
                int(value),
                int(port_time_samples),
            )
        )

    def push_midi_note_on(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate live MIDI note-on to a MIDI destination.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_note_on"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_note_on(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(render_frame),
            )
        )

    def push_midi_note_off(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate live MIDI note-off to a MIDI destination.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_note_off"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_note_off(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(render_frame),
            )
        )

    def transport_state(self) -> TransportState:
        """Read the current engine transport state (playing/position/ppq/tempo)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_get_transport_state"):
            raise RuntimeError("libsonare was built without transport-state support")
        raw = SonareTransportState()
        _check(lib.sonare_engine_get_transport_state(self._require_handle(), ctypes.byref(raw)))
        return TransportState(
            playing=bool(raw.playing),
            looping=bool(raw.looping),
            render_frame=int(raw.render_frame),
            sample_position=int(raw.sample_position),
            ppq_position=float(raw.ppq_position),
            bpm=float(raw.bpm),
            loop_start_ppq=float(raw.loop_start_ppq),
            loop_end_ppq=float(raw.loop_end_ppq),
            sample_rate=float(raw.sample_rate),
            bar_start_ppq=float(raw.bar_start_ppq),
            bar_count=int(raw.bar_count),
            time_signature=TimeSignature(
                numerator=int(raw.time_signature.numerator),
                denominator=int(raw.time_signature.denominator),
                confidence=float(raw.time_signature.confidence),
            ),
        )

    @staticmethod
    def _channel_arrays(
        channels: Sequence[Sequence[float]],
    ) -> tuple[list[ctypes.Array[ctypes.c_float]], ctypes.Array, int]:
        if not channels:
            raise ValueError("channels must not be empty")
        frame_count = len(channels[0])
        if frame_count == 0:
            raise ValueError("channels must not be empty")
        arrays: list[ctypes.Array[ctypes.c_float]] = []
        for channel in channels:
            if len(channel) != frame_count:
                raise ValueError("all channels must have the same length")
            # Zero-copy marshal each channel via NumPy's vectorised C path
            # instead of `(c_float*N)(*channel)`, which unpacks every sample
            # through Python varargs on this realtime path. The engine writes
            # its output back into these buffers in-place, so always own a
            # fresh writable copy (np.array(copy=True)) rather than aliasing the
            # caller's array; pin the numpy backing to the ctypes object so it
            # outlives the C call.
            buf = np.array(_as_float32_buffer(channel), dtype=np.float32, copy=True, order="C")
            c_array = (ctypes.c_float * frame_count).from_buffer(buf)
            c_array._np_backing = buf  # type: ignore[attr-defined]
            arrays.append(c_array)
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(arrays)
        ptrs = ptr_type(*[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in arrays])
        return arrays, ptrs, frame_count


def _fixed_bytes(value: str, capacity: int) -> bytes:
    # Truncate on a UTF-8 character boundary: slicing the encoded bytes at an
    # arbitrary offset can split a multi-byte codepoint and leave invalid UTF-8.
    # Decoding with errors="ignore" drops any partial trailing codepoint.
    if capacity <= 1:
        return b""
    return value.encode("utf-8")[: capacity - 1].decode("utf-8", "ignore").encode("utf-8")


def _c_string(value: bytes) -> str:
    # Fixed-size C name buffers can hold a truncated multi-byte codepoint, so
    # use a replacement fallback rather than letting a decode error abort a
    # whole batch of results.
    return value.split(b"\0", 1)[0].decode("utf-8", "replace")


def _parameter_from_c(raw: SonareParameterInfo) -> ParameterInfo:
    return ParameterInfo(
        id=int(raw.id),
        name=_c_string(bytes(raw.name)),
        unit=_c_string(bytes(raw.unit)),
        min_value=float(raw.min_value),
        max_value=float(raw.max_value),
        default_value=float(raw.default_value),
        rt_safe=bool(raw.rt_safe),
        default_curve=AutomationCurve(int(raw.default_curve)),
    )


def _marker_to_c(marker: EngineMarker) -> SonareEngineMarker:
    raw = SonareEngineMarker()
    raw.id = int(marker.id)
    raw.ppq = float(marker.ppq)
    raw.name = _fixed_bytes(marker.name, 64)
    return raw


def _marker_from_c(raw: SonareEngineMarker) -> EngineMarker:
    return EngineMarker(id=int(raw.id), ppq=float(raw.ppq), name=_c_string(bytes(raw.name)))


def _metronome_to_c(config: EngineMetronomeConfig) -> SonareEngineMetronomeConfig:
    raw = SonareEngineMetronomeConfig()
    raw.enabled = int(config.enabled)
    raw.beat_gain = float(config.beat_gain)
    raw.accent_gain = float(config.accent_gain)
    raw.click_samples = int(config.click_samples)
    raw.click_seconds = float(config.click_seconds)
    return raw


def _metronome_from_c(raw: SonareEngineMetronomeConfig) -> EngineMetronomeConfig:
    return EngineMetronomeConfig(
        enabled=bool(raw.enabled),
        beat_gain=float(raw.beat_gain),
        accent_gain=float(raw.accent_gain),
        click_samples=int(raw.click_samples),
        click_seconds=float(raw.click_seconds),
    )


def _clips_to_c(
    clips: Sequence[EngineClip],
) -> tuple[
    ctypes.Array[SonareEngineClip],
    list[list[ctypes.Array[ctypes.c_float]]],
    list[ctypes.Array],
    list[ctypes.Array],
]:
    channel_arrays: list[list[ctypes.Array[ctypes.c_float]]] = []
    channel_ptrs: list[ctypes.Array] = []
    warp_arrays: list[ctypes.Array] = []
    raw_items: list[SonareEngineClip] = []
    for clip in clips:
        page_provider = getattr(clip, "page_provider", None)
        if page_provider is not None and not isinstance(page_provider, ClipPageProvider):
            raise TypeError("clip page_provider must be a ClipPageProvider")
        if page_provider is not None:
            raw = SonareEngineClip()
            raw.id = int(clip.id)
            raw.track_id = int(clip.track_id)
            raw.channels = None
            raw.num_channels = 0
            raw.num_samples = 0
            raw.start_ppq = float(clip.start_ppq)
            raw.clip_offset_samples = int(clip.clip_offset_samples)
            raw.length_samples = int(clip.length_samples) if clip.length_samples is not None else 0
            raw.loop = int(clip.loop)
            raw.gain = float(clip.gain)
            raw.fade_in_samples = int(clip.fade_in_samples)
            raw.fade_out_samples = int(clip.fade_out_samples)
            raw.warp_mode = _warp_mode_value(clip.warp_mode)
            raw.page_provider = page_provider._require_handle()
            if clip.warp_anchors:
                anchor_array = (SonareEngineWarpAnchor * len(clip.warp_anchors))()
                for i, (warp_sample, source_sample) in enumerate(clip.warp_anchors):
                    anchor_array[i].warp_sample = float(warp_sample)
                    anchor_array[i].source_sample = float(source_sample)
                raw.warp_anchors = ctypes.cast(anchor_array, ctypes.c_void_p)
                raw.warp_anchor_count = len(clip.warp_anchors)
                warp_arrays.append(anchor_array)
            raw_items.append(raw)
            channel_arrays.append([])
            continue
        if not clip.channels:
            raise ValueError("clip channels must not be empty")
        num_samples = len(clip.channels[0])
        if num_samples <= 0:
            raise ValueError("clip channels must not be empty")
        arrays: list[ctypes.Array[ctypes.c_float]] = []
        ptr_values: list[ctypes.POINTER(ctypes.c_float)] = []
        for channel in clip.channels:
            if len(channel) != num_samples:
                raise ValueError("all clip channels must have the same length")
            array = (ctypes.c_float * num_samples)(*channel)
            arrays.append(array)
            ptr_values.append(ctypes.cast(array, ctypes.POINTER(ctypes.c_float)))
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(ptr_values)
        ptrs = ptr_type(*ptr_values)
        raw = SonareEngineClip()
        raw.id = int(clip.id)
        raw.track_id = int(clip.track_id)
        raw.channels = ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float)))
        raw.num_channels = len(arrays)
        raw.num_samples = num_samples
        raw.start_ppq = float(clip.start_ppq)
        raw.clip_offset_samples = int(clip.clip_offset_samples)
        raw.length_samples = (
            int(clip.length_samples) if clip.length_samples is not None else num_samples
        )
        raw.loop = int(clip.loop)
        raw.gain = float(clip.gain)
        raw.fade_in_samples = int(clip.fade_in_samples)
        raw.fade_out_samples = int(clip.fade_out_samples)
        raw.warp_mode = _warp_mode_value(clip.warp_mode)
        if clip.warp_anchors:
            anchor_array = (SonareEngineWarpAnchor * len(clip.warp_anchors))()
            for i, (warp_sample, source_sample) in enumerate(clip.warp_anchors):
                anchor_array[i].warp_sample = float(warp_sample)
                anchor_array[i].source_sample = float(source_sample)
            raw.warp_anchors = ctypes.cast(anchor_array, ctypes.c_void_p)
            raw.warp_anchor_count = len(clip.warp_anchors)
            warp_arrays.append(anchor_array)
        raw.page_provider = None
        raw_items.append(raw)
        channel_arrays.append(arrays)
        channel_ptrs.append(ptrs)
    return (
        (SonareEngineClip * len(raw_items))(*raw_items),
        channel_arrays,
        channel_ptrs,
        warp_arrays,
    )


def _warp_mode_value(mode: str | int) -> int:
    if isinstance(mode, bool):
        raise ValueError(f"unknown warp mode: {mode}")
    if isinstance(mode, int):
        return mode
    if not isinstance(mode, str):
        raise ValueError(f"unknown warp mode: {mode}")
    key = mode.lower()
    if key == "off":
        return 0
    if key == "repitch":
        return 1
    if key == "tempo-sync":
        return 2
    raise ValueError(f"unknown warp mode: {mode}")


def _graph_node_to_c(node: EngineGraphNode) -> SonareEngineGraphNode:
    raw = SonareEngineGraphNode()
    raw.id = _fixed_bytes(node.id, 64)
    raw.type = int(EngineGraphNodeType(node.type))
    raw.gain_db = float(node.gain_db)
    raw.num_ports = int(node.num_ports)
    return raw


def _graph_connection_to_c(connection: EngineGraphConnection) -> SonareEngineGraphConnection:
    raw = SonareEngineGraphConnection()
    raw.source_node = _fixed_bytes(connection.source_node, 64)
    raw.source_port = int(connection.source_port)
    raw.dest_node = _fixed_bytes(connection.dest_node, 64)
    raw.dest_port = int(connection.dest_port)
    raw.mix = int(EngineGraphMix(connection.mix))
    return raw


def _graph_parameter_binding_to_c(
    binding: EngineGraphParameterBinding,
) -> SonareEngineGraphParameterBinding:
    raw = SonareEngineGraphParameterBinding()
    raw.param_id = int(binding.param_id)
    raw.node_id = _fixed_bytes(binding.node_id, 64)
    return raw


def _telemetry_from_c(raw: SonareEngineTelemetry) -> EngineTelemetry:
    return EngineTelemetry(
        type=EngineTelemetryType(int(raw.type)),
        error=EngineTelemetryError(int(raw.error)),
        render_frame=int(raw.render_frame),
        timeline_sample=int(raw.timeline_sample),
        audible_timeline_sample=int(raw.audible_timeline_sample),
        graph_latency_samples_q8=int(raw.graph_latency_samples_q8),
        value=int(raw.value),
    )


def _meter_telemetry_from_c(raw: SonareMeterTelemetryRecord) -> MeterTelemetryRecord:
    return MeterTelemetryRecord(
        target_id=int(raw.target_id),
        render_frame=int(raw.render_frame),
        seq=int(raw.seq),
        peak_db_l=float(raw.peak_db_l),
        peak_db_r=float(raw.peak_db_r),
        rms_db_l=float(raw.rms_db_l),
        rms_db_r=float(raw.rms_db_r),
        true_peak_db_l=float(raw.true_peak_db_l),
        true_peak_db_r=float(raw.true_peak_db_r),
        max_true_peak_db=float(raw.max_true_peak_db),
        correlation=float(raw.correlation),
        mono_compat_width=float(raw.mono_compat_width),
        momentary_lufs=float(raw.momentary_lufs),
        short_term_lufs=float(raw.short_term_lufs),
        integrated_lufs=float(raw.integrated_lufs),
        gain_reduction_db=float(raw.gain_reduction_db),
        dropped_records=int(raw.dropped_records),
    )
