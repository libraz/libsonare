"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
import json
import operator
from collections.abc import Mapping, Sequence

from ._runtime import (
    AutomationCurve,
    EngineClip,
    EngineGraphConnection,
    EngineGraphMix,
    EngineGraphNode,
    EngineGraphNodeType,
    EngineGraphParameterBinding,
    EngineMarker,
    EngineMetronomeConfig,
    EngineTelemetry,
    EngineTelemetryError,
    EngineTelemetryType,
    MeterTelemetryRecord,
    MeterTelemetryRecordWide,
    ParameterInfo,
    ScopeTelemetryRecord,
    SonareEngineClip,
    SonareEngineGraphConnection,
    SonareEngineGraphNode,
    SonareEngineGraphParameterBinding,
    SonareEngineMarker,
    SonareEngineMetronomeConfig,
    SonareEngineTelemetry,
    SonareEngineWarpAnchor,
    SonareMeterTelemetryRecord,
    SonareMeterTelemetryRecordWide,
    SonareParameterInfo,
    SonareScopeTelemetryRecord,
    SonareValueError,
    _warp_mode_value,
)
from ._types_engine import EngineTrackMonitorMode

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
EXPECTED_ENGINE_ABI_VERSION = 3
_CAPTURE_SOURCE_VALUES = {"output": 0, "input": 1}
_TRACK_MONITOR_MODE_VALUES = {
    "off": int(EngineTrackMonitorMode.OFF),
    "pfl": int(EngineTrackMonitorMode.PFL),
    "afl": int(EngineTrackMonitorMode.AFL),
}


def _track_monitor_mode_value(mode: EngineTrackMonitorMode | str | int) -> int:
    """Resolve a track monitor mode to its C enum ordinal.

    Only the three public ordinals and their case-insensitive ASCII names are
    accepted; booleans and fractional values are rejected before the native call.
    """
    if isinstance(mode, str):
        if mode.isascii():
            try:
                return _TRACK_MONITOR_MODE_VALUES[mode.lower()]
            except KeyError:
                pass
        raise SonareValueError("track monitor mode must be OFF/PFL/AFL or an integer 0, 1, or 2")
    if isinstance(mode, bool):
        raise SonareValueError("track monitor mode must be OFF/PFL/AFL or an integer 0, 1, or 2")
    try:
        value = operator.index(mode)
    except (TypeError, ValueError, OverflowError) as exc:
        raise SonareValueError(
            "track monitor mode must be OFF/PFL/AFL or an integer 0, 1, or 2"
        ) from exc
    if value not in _TRACK_MONITOR_MODE_VALUES.values():
        raise SonareValueError("track monitor mode must be OFF/PFL/AFL or an integer 0, 1, or 2")
    return int(value)


def _capture_source_value(source: str | int) -> int:
    if isinstance(source, str):
        try:
            return _CAPTURE_SOURCE_VALUES[source]
        except KeyError as exc:
            raise SonareValueError("capture source must be 'output' or 'input'") from exc
    value = int(source)
    if value in _CAPTURE_SOURCE_VALUES.values():
        return value
    raise SonareValueError("capture source must be 'output' or 'input'")


def _capture_source_name(source: int) -> str:
    return "input" if int(source) == _CAPTURE_SOURCE_VALUES["input"] else "output"


def _band_json_arg(band: Mapping[str, object] | str) -> bytes:
    return (band if isinstance(band, str) else json.dumps(dict(band))).encode("utf-8")


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
    try:
        marker_id = operator.index(marker.id)
    except TypeError as exc:
        raise SonareValueError("marker id must be a positive uint32 integer") from exc
    if marker_id <= 0 or marker_id > 0xFFFFFFFF:
        raise SonareValueError("marker id must be a positive uint32 integer")
    raw = SonareEngineMarker()
    raw.id = marker_id
    raw.kind = int(marker.kind) & 0xFF
    raw.key_fifths = int(marker.key_fifths)
    raw.key_minor = 1 if marker.key_minor else 0
    raw.ppq = float(marker.ppq)
    raw.name = _fixed_bytes(marker.name, 64)
    return raw


def _marker_from_c(raw: SonareEngineMarker) -> EngineMarker:
    return EngineMarker(
        id=int(raw.id),
        ppq=float(raw.ppq),
        name=_c_string(bytes(raw.name)),
        kind=int(raw.kind),
        key_fifths=int(raw.key_fifths),
        key_minor=bool(raw.key_minor),
    )


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
    list[ctypes.Array[ctypes._Pointer[ctypes.c_float]]],
    list[ctypes.Array[SonareEngineWarpAnchor]],
]:
    # Local import avoids a module cycle: page providers use the shared runtime,
    # while clip conversion validates their concrete host-side wrapper type.
    from ._engine_pages import ClipPageProvider

    channel_arrays: list[list[ctypes.Array[ctypes.c_float]]] = []
    channel_ptrs: list[ctypes.Array[ctypes._Pointer[ctypes.c_float]]] = []
    warp_arrays: list[ctypes.Array[SonareEngineWarpAnchor]] = []
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
            raise SonareValueError("clip channels must not be empty")
        num_samples = len(clip.channels[0])
        if num_samples <= 0:
            raise SonareValueError("clip channels must not be empty")
        arrays: list[ctypes.Array[ctypes.c_float]] = []
        ptr_values: list[ctypes._Pointer[ctypes.c_float]] = []
        for channel in clip.channels:
            if len(channel) != num_samples:
                raise SonareValueError("all clip channels must have the same length")
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


def _meter_telemetry_wide_from_c(
    raw: SonareMeterTelemetryRecordWide,
) -> MeterTelemetryRecordWide:
    planes = max(0, min(int(raw.channel_count), len(raw.peak_db)))
    return MeterTelemetryRecordWide(
        target_id=int(raw.target_id),
        render_frame=int(raw.render_frame),
        seq=int(raw.seq),
        channel_count=planes,
        peak_db=[float(raw.peak_db[i]) for i in range(planes)],
        rms_db=[float(raw.rms_db[i]) for i in range(planes)],
        true_peak_db=[float(raw.true_peak_db[i]) for i in range(planes)],
        max_true_peak_db=float(raw.max_true_peak_db),
        correlation=float(raw.correlation),
        mono_compat_width=float(raw.mono_compat_width),
        momentary_lufs=float(raw.momentary_lufs),
        short_term_lufs=float(raw.short_term_lufs),
        integrated_lufs=float(raw.integrated_lufs),
        gain_reduction_db=float(raw.gain_reduction_db),
        dropped_records=int(raw.dropped_records),
    )


def _scope_telemetry_from_c(raw: SonareScopeTelemetryRecord) -> ScopeTelemetryRecord:
    band_count = int(raw.band_count)
    point_count = int(raw.point_count)
    bands = [float(raw.bands[i]) for i in range(band_count)]
    points = [(float(raw.points[2 * i]), float(raw.points[2 * i + 1])) for i in range(point_count)]
    return ScopeTelemetryRecord(
        target_id=int(raw.target_id),
        render_frame=int(raw.render_frame),
        seq=int(raw.seq),
        dropped_records=int(raw.dropped_records),
        bands=bands,
        points=points,
    )
