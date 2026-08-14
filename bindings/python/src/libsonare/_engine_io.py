"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

import numpy as np

from ._engine_conversions import (
    _capture_source_name,
    _capture_source_value,
    _fixed_bytes,
    _graph_connection_to_c,
    _graph_node_to_c,
    _graph_parameter_binding_to_c,
    _meter_telemetry_from_c,
    _meter_telemetry_wide_from_c,
    _scope_telemetry_from_c,
    _telemetry_from_c,
)
from ._ffi_types_core import (
    SonareClipPageRequest,
    SonareEngineBounceOptions,
    SonareEngineBounceResult,
    SonareEngineCaptureBuffer,
    SonareEngineCaptureStatus,
    SonareEngineFreezeOptions,
    SonareEngineFreezeResult,
    SonareEngineGraphConnection,
    SonareEngineGraphNode,
    SonareEngineGraphParameterBinding,
    SonareEngineGraphSpec,
    SonareEngineTelemetry,
    SonareMeterTelemetryRecord,
    SonareMeterTelemetryRecordWide,
    SonareScopeTelemetryRecord,
)
from ._runtime import (
    ClipPageRequest,
    EngineBounceOptions,
    EngineBounceResult,
    EngineCaptureStatus,
    EngineFreezeOptions,
    EngineFreezeResult,
    EngineGraphSpec,
    EngineTelemetry,
    MeterTelemetryRecord,
    MeterTelemetryRecordWide,
    ScopeTelemetryRecord,
    _as_float32_buffer,
    _check,
    _from_c_float_array,
    _get_lib,
)


class _EngineIoMixin:
    _capture_arrays: list[ctypes.Array[ctypes.c_float]]
    _capture_ptrs: ctypes.Array[Any] | None

    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

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
        """Shift capture on the timeline; positive values delay the punch window."""
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
        """Render one block and return the processed channel copies.

        Input channel buffers are never modified: the engine renders over
        private copies, adding its output to each corresponding input plane.
        Pass zero-filled planes when the engine is the only audio source.
        """
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

    def drain_meter_telemetry_wide(self, max_records: int = 1024) -> list[MeterTelemetryRecordWide]:
        """Drain pending per-plane meter telemetry for a surround target.

        Use this drain for a surround mix target; :meth:`drain_meter_telemetry`
        stays the stereo fast path. The two share one queue, so call only one
        per target.
        """
        if max_records <= 0:
            return []
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_drain_meter_telemetry_wide"):
            raise RuntimeError("libsonare was built without meter-telemetry support")
        raw = (SonareMeterTelemetryRecordWide * int(max_records))()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_engine_drain_meter_telemetry_wide(
                self._require_handle(), raw, int(max_records), ctypes.byref(written)
            )
        )
        return [_meter_telemetry_wide_from_c(raw[i]) for i in range(written.value)]

    def configure_scope_telemetry(self, interval_frames: int, band_count: int) -> int:
        """Enable scope telemetry publishing and return the applied band count.

        ``interval_frames`` is the minimum render-frame spacing between snapshots;
        ``band_count`` is the requested number of spectrum bands (clamped by the
        engine). Returns the band count the engine actually applied.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_configure_scope_telemetry"):
            raise RuntimeError("libsonare was built without scope-telemetry support")
        applied = ctypes.c_uint()
        _check(
            lib.sonare_engine_configure_scope_telemetry(
                self._require_handle(),
                int(interval_frames),
                int(band_count),
                ctypes.byref(applied),
            )
        )
        return int(applied.value)

    def drain_scope_telemetry(self, max_records: int = 1024) -> list[ScopeTelemetryRecord]:
        """Drain pending scope telemetry records published by the engine."""
        if max_records <= 0:
            return []
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_drain_scope_telemetry"):
            raise RuntimeError("libsonare was built without scope-telemetry support")
        raw = (SonareScopeTelemetryRecord * int(max_records))()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_engine_drain_scope_telemetry(
                self._require_handle(), raw, int(max_records), ctypes.byref(written)
            )
        )
        return [_scope_telemetry_from_c(raw[i]) for i in range(written.value)]

    @staticmethod
    def _channel_arrays(
        channels: Sequence[Sequence[float]],
    ) -> tuple[list[ctypes.Array[ctypes.c_float]], ctypes.Array[Any], int]:
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
