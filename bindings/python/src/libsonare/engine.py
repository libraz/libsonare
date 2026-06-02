"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._runtime import (
    AutomationCurve,
    AutomationPoint,
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
    EngineTelemetry,
    EngineTelemetryError,
    EngineTelemetryType,
    MeterTelemetryRecord,
    ParameterInfo,
    SonareAutomationPoint,
    SonareEngineBounceOptions,
    SonareEngineBounceResult,
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
    SonareEngineTelemetry,
    SonareMeterTelemetryRecord,
    SonareParameterInfo,
    SonareTransportState,
    TimeSignature,
    TransportState,
    _check,
    _from_c_float_array,
    _get_lib,
)

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
EXPECTED_ENGINE_ABI_VERSION = 2


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
        raw_clips, _channel_arrays, _channel_ptrs = _clips_to_c(clips)
        _check(_get_lib().sonare_engine_set_clips(self._require_handle(), raw_clips, len(clips)))

    def clip_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_clip_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

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
        monitor_arrays = [(ctypes.c_float * frame_count)(*([0.0] * frame_count)) for _ in arrays]
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
        raw_options = SonareEngineBounceOptions()
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
        lib = _get_lib()
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
            arrays.append((ctypes.c_float * frame_count)(*channel))
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
    return value.split(b"\0", 1)[0].decode("utf-8")


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
]:
    channel_arrays: list[list[ctypes.Array[ctypes.c_float]]] = []
    channel_ptrs: list[ctypes.Array] = []
    raw_items: list[SonareEngineClip] = []
    for clip in clips:
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
        raw_items.append(raw)
        channel_arrays.append(arrays)
        channel_ptrs.append(ptrs)
    return (SonareEngineClip * len(raw_items))(*raw_items), channel_arrays, channel_ptrs


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
