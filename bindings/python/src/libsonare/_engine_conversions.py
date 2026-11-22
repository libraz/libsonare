"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
import dataclasses
import json
import operator
from collections.abc import Mapping, Sequence
from typing import cast

from ._errors import _refuse, _refuse_type
from ._ffi_types_core import SonareAutomationPoint
from ._narrowing import _INT64_MAX, _INT64_MIN, _narrow_double, _narrow_int
from ._runtime import (
    _AUTOMATION_CURVES,
    _UINT32_MAX,
    AutomationCurve,
    EngineClip,
    EngineGraphConnection,
    EngineGraphNode,
    EngineGraphParameterBinding,
    EngineMarker,
    EngineMarkerInput,
    EngineMetronomeConfig,
    EngineTelemetry,
    MeterTelemetryRecord,
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
    SonareParameterInfo,
    SonareScopeTelemetryRecord,
    _curve_value,
    _from_c_float_array,
    _optional_positive_int,
    _planar_channel_arrays,
    _require_instance,
    _token_ordinal,
    _utf8_arg,
    _warp_mode_value,
)
from ._types_engine import (
    _CAPTURE_SOURCES,
    _GRAPH_MIXES,
    _GRAPH_NODE_TYPES,
    _MARKER_KINDS,
    _TRACK_MONITOR_MODES,
    AutomationPoint,
    EngineCaptureSource,
    EngineTrackMonitorMode,
    MarkerKind,
)
from ._types_enums import (
    _ENGINE_TELEMETRY_ERRORS,
    _ENGINE_TELEMETRY_TYPES,
    EngineTelemetryError,
    EngineTelemetryType,
)

# SONARE_ENGINE_METRONOME_CLICK_FROM_SEC: the click length is derived from click_sec.
_METRONOME_CLICK_FROM_SEC = -1
# The C ABI's wording for a positive int field and a positive int64 count.
_POSITIVE_INT = "an integer in [1, 2147483647]"
_POSITIVE_COUNT = "an integer of at least 1"


def _given_positive(value: object, name: str, domain: str) -> int | None:
    """A given positive engine field whose -1 is the C "not given" constant; None stays None.

    A negative would alias that constant, so it is refused here in the C ABI's words; a 0 is
    left to the C refusal.
    """
    if value is None:
        return None
    number = _narrow_int(value, name, _INT64_MIN, _INT64_MAX)
    if number < 0:
        raise _refuse(f"{name} must be {domain}")
    return number


def _track_monitor_mode_value(mode: EngineTrackMonitorMode, key: str) -> int:
    """Resolve a track monitor mode token to its C enum ordinal."""
    return _token_ordinal(mode, _TRACK_MONITOR_MODES, key)


def _capture_source_value(source: EngineCaptureSource, key: str) -> int:
    """Resolve a capture source token to its C enum ordinal."""
    return _token_ordinal(source, _CAPTURE_SOURCES, key)


def _capture_source_name(source: int) -> EngineCaptureSource:
    return cast(EngineCaptureSource, _CAPTURE_SOURCES[int(source)])


def _marker_kind_value(kind: MarkerKind, key: str) -> int:
    """Resolve a marker kind token to its C enum ordinal."""
    return _token_ordinal(kind, _MARKER_KINDS, key)


def _marker_kind_name(kind: int) -> MarkerKind:
    return cast(MarkerKind, _MARKER_KINDS[int(kind)])


def _band_json_arg(band: Mapping[str, object]) -> bytes:
    if not isinstance(band, Mapping):
        raise _refuse_type("band must be a mapping")
    return _utf8_arg(json.dumps(dict(band)), "band")


def _scene_entry_json(entry: Mapping[str, object], key: str, name: str) -> bytes:
    """Wrap one strip or bus mapping as the one-entry scene document the engine setters read."""
    if not isinstance(entry, Mapping):
        raise _refuse_type(f"{name} must be a mapping (parse JSON text with json.loads first)")
    return _utf8_arg(json.dumps({key: [dict(entry)]}), name)


def _fixed_bytes(value: str, capacity: int, name: str) -> bytes:
    """Encode ``value`` for a fixed C char field; one that does not fit is refused, never cut."""
    encoded = _utf8_arg(value, name)
    if len(encoded) >= capacity:
        raise _refuse(f"{name} must be at most {capacity - 1} UTF-8 bytes")
    return encoded


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
        default_curve=cast(AutomationCurve, _AUTOMATION_CURVES[int(raw.default_curve)]),
    )


def _automation_points_to_c(
    points: Sequence[AutomationPoint],
) -> ctypes.Array[SonareAutomationPoint]:
    """Marshal :class:`AutomationPoint` records; any other item is a ``TypeError``."""
    rows = list(points)
    raw = (SonareAutomationPoint * len(rows))()
    for i, point in enumerate(rows):
        _require_instance(point, AutomationPoint, f"points[{i}]")
        raw[i].ppq = _narrow_double(point.ppq, f"points[{i}].ppq")
        raw[i].value = _narrow_double(point.value, f"points[{i}].value")
        raw[i].curve_to_next = _curve_value(point.curve_to_next, f"points[{i}].curve_to_next")
    return raw


def _assign_marker_ids(markers: Sequence[object], next_id: int) -> tuple[list[object], int]:
    """Give every id-less marker the lowest id at or above ``next_id`` no entry carries.

    Explicit ids are kept; validating them is the setter's, and an entry that is
    not a marker passes through for :func:`_marker_to_c` to refuse.
    Returns the entries in the order given and the counter for the next call
    (above every id the list now holds).
    """

    def explicit(marker: object) -> int | None:
        if not isinstance(marker, EngineMarker | EngineMarkerInput):
            return None
        value = marker.id
        return value if isinstance(value, int) and not isinstance(value, bool) else None

    taken = {value for value in map(explicit, markers) if value is not None}
    following = next_id
    highest = next_id - 1
    out: list[object] = []
    for marker in markers:
        if not isinstance(marker, EngineMarkerInput) or marker.id is not None:
            value = explicit(marker)
            if value is not None:
                highest = max(highest, value)
            out.append(marker)
            continue
        while following in taken:
            following += 1
        taken.add(following)
        highest = max(highest, following)
        out.append(dataclasses.replace(marker, id=following))
        following += 1
    return out, highest + 1


def _marker_to_c(value: object) -> SonareEngineMarker:
    if not isinstance(value, EngineMarker | EngineMarkerInput):
        raise _refuse_type("marker must be an EngineMarker or an EngineMarkerInput")
    marker = value
    try:
        # set_markers has allocated every id-less entry, so a None does not reach here.
        marker_id = operator.index(cast(int, marker.id))
    except TypeError as exc:
        raise _refuse(f"marker id must be an integer in [1, {_UINT32_MAX}]") from exc
    if marker_id <= 0 or marker_id > _UINT32_MAX:
        raise _refuse(f"marker id must be an integer in [1, {_UINT32_MAX}]")
    raw = SonareEngineMarker()
    raw.id = marker_id
    # Assigned unconverted so the struct's own narrowing sees each caller value;
    # int() would truncate a fraction past it.
    raw.kind = _marker_kind_value(marker.kind, "marker.kind")
    raw.key_fifths = marker.key_fifths
    raw.key_minor = 1 if marker.key_minor else 0
    raw.ppq = _narrow_double(marker.ppq, "marker.ppq")
    if marker.name is not None:
        if marker.name == "":
            raise _refuse("marker name must not be empty; omit it for an unnamed marker")
        raw.name = _fixed_bytes(marker.name, 64, "name")
    return raw


def _marker_from_c(raw: SonareEngineMarker) -> EngineMarker:
    return EngineMarker(
        float(raw.ppq),
        id=int(raw.id),
        name=_c_string(bytes(raw.name)) or None,
        kind=_marker_kind_name(raw.kind),
        key_fifths=int(raw.key_fifths),
        key_minor=bool(raw.key_minor),
    )


def _metronome_to_c(config: EngineMetronomeConfig) -> SonareEngineMetronomeConfig:
    _require_instance(config, EngineMetronomeConfig, "config")
    # An omitted click length keeps the init's CLICK_FROM_SEC / 2 ms; a given 0 is the C refusal's.
    return SonareEngineMetronomeConfig._given(
        enabled=int(config.enabled),
        beat_gain=_narrow_double(config.beat_gain, "config.beat_gain"),
        accent_gain=_narrow_double(config.accent_gain, "config.accent_gain"),
        click_samples=_given_positive(
            config.click_samples, "config.click_samples", "an integer in [1, 384000]"
        ),
        click_sec=None
        if config.click_sec is None
        else _narrow_double(config.click_sec, "config.click_sec"),
    )


def _metronome_from_c(raw: SonareEngineMetronomeConfig) -> EngineMetronomeConfig:
    return EngineMetronomeConfig(
        enabled=bool(raw.enabled),
        beat_gain=float(raw.beat_gain),
        accent_gain=float(raw.accent_gain),
        click_samples=None
        if raw.click_samples == _METRONOME_CLICK_FROM_SEC
        else int(raw.click_samples),
        click_sec=float(raw.click_sec),
    )


def _clips_to_c(
    clips: Sequence[EngineClip],
    indexed: bool = True,
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
    for index, clip in enumerate(clips):
        _require_instance(clip, EngineClip, "clip")
        # The C ABI's subject: clips[i]. for a set, clip. for a single upsert.
        length_name = f"clips[{index}].length_samples" if indexed else "clip.length_samples"
        page_provider = getattr(clip, "page_provider", None)
        if page_provider is not None and not isinstance(page_provider, ClipPageProvider):
            raise _refuse_type("clip page_provider must be a ClipPageProvider")
        if page_provider is not None:
            raw = SonareEngineClip()
            # Assigned unconverted so the struct's own narrowing sees each
            # caller value; int() would truncate a fraction past it. loop is the
            # one exception -- it is a documented bool, spelled 0/1 here.
            raw.id = clip.id
            raw.track_id = _optional_positive_int(clip.track_id, "clip.track_id", _UINT32_MAX)
            raw.channels = None
            raw.num_channels = 0
            raw.num_samples = 0
            raw.start_ppq = _narrow_double(clip.start_ppq, "clip.start_ppq")
            raw.clip_offset_samples = clip.clip_offset_samples
            if clip.length_samples is not None:
                raw.length_samples = _given_positive(
                    clip.length_samples, length_name, _POSITIVE_COUNT
                )
            raw.loop = 1 if clip.loop else 0
            raw.gain = _narrow_double(clip.gain, "clip.gain")
            raw.fade_in_samples = clip.fade_in_samples
            raw.fade_out_samples = clip.fade_out_samples
            raw.warp_mode = _warp_mode_value(clip.warp_mode, "clip.warp_mode")
            raw.page_provider = page_provider._require_handle()
            if clip.warp_anchors:
                anchor_array = (SonareEngineWarpAnchor * len(clip.warp_anchors))()
                for i, (warp_sample, source_sample) in enumerate(clip.warp_anchors):
                    anchor_array[i].warp_sample = _narrow_double(warp_sample, "warp_sample")
                    anchor_array[i].source_sample = _narrow_double(source_sample, "source_sample")
                raw.warp_anchors = ctypes.cast(anchor_array, ctypes.c_void_p)
                raw.warp_anchor_count = len(clip.warp_anchors)
                warp_arrays.append(anchor_array)
            raw_items.append(raw)
            channel_arrays.append([])
            continue
        arrays, ptrs, num_samples = _planar_channel_arrays(clip.channels, subject="clip channels")
        raw = SonareEngineClip()
        # As above: unconverted so the struct narrows, loop spelled 0/1.
        raw.id = clip.id
        raw.track_id = _optional_positive_int(clip.track_id, "clip.track_id", _UINT32_MAX)
        raw.channels = ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float)))
        raw.num_channels = len(arrays)
        raw.num_samples = num_samples
        raw.start_ppq = _narrow_double(clip.start_ppq, "clip.start_ppq")
        raw.clip_offset_samples = clip.clip_offset_samples
        # An omitted length keeps the init's LENGTH_TO_END: the source remaining after the offset.
        if clip.length_samples is not None:
            raw.length_samples = _given_positive(clip.length_samples, length_name, _POSITIVE_COUNT)
        raw.loop = 1 if clip.loop else 0
        raw.gain = _narrow_double(clip.gain, "clip.gain")
        raw.fade_in_samples = clip.fade_in_samples
        raw.fade_out_samples = clip.fade_out_samples
        raw.warp_mode = _warp_mode_value(clip.warp_mode, "clip.warp_mode")
        if clip.warp_anchors:
            anchor_array = (SonareEngineWarpAnchor * len(clip.warp_anchors))()
            for i, (warp_sample, source_sample) in enumerate(clip.warp_anchors):
                anchor_array[i].warp_sample = _narrow_double(warp_sample, "warp_sample")
                anchor_array[i].source_sample = _narrow_double(source_sample, "source_sample")
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


def _graph_node_to_c(node: EngineGraphNode, index: int) -> SonareEngineGraphNode:
    # An omitted port count keeps the init's PORTS_FROM_SPEC.
    return SonareEngineGraphNode._given(
        id=_fixed_bytes(node.id, 64, "id"),
        type=_token_ordinal(node.type, _GRAPH_NODE_TYPES, "node.type"),
        gain_db=_narrow_double(node.gain_db, "node.gain_db"),
        num_ports=_given_positive(node.num_ports, f"spec.nodes[{index}].num_ports", _POSITIVE_INT),
    )


def _graph_connection_to_c(connection: EngineGraphConnection) -> SonareEngineGraphConnection:
    raw = SonareEngineGraphConnection()
    raw.source_node = _fixed_bytes(connection.source_node, 64, "source_node")
    # Assigned unconverted so the struct's own narrowing sees each caller value;
    # int() would truncate a fraction past it.
    raw.source_port = connection.source_port
    raw.dest_node = _fixed_bytes(connection.dest_node, 64, "dest_node")
    raw.dest_port = connection.dest_port
    raw.mix = _token_ordinal(connection.mix, _GRAPH_MIXES, "connection.mix")
    return raw


def _graph_parameter_binding_to_c(
    binding: EngineGraphParameterBinding,
) -> SonareEngineGraphParameterBinding:
    raw = SonareEngineGraphParameterBinding()
    # Assigned unconverted so the struct's own narrowing sees the caller value.
    raw.param_id = binding.param_id
    raw.node_id = _fixed_bytes(binding.node_id, 64, "node_id")
    return raw


# Fixed-point scale of the latency the native engine reports (1/256 sample).
_LATENCY_Q8_SCALE = 256.0


def _telemetry_from_c(raw: SonareEngineTelemetry) -> EngineTelemetry:
    return EngineTelemetry(
        type=cast(EngineTelemetryType, _ENGINE_TELEMETRY_TYPES[int(raw.type)]),
        error=cast(EngineTelemetryError, _ENGINE_TELEMETRY_ERRORS[int(raw.error)]),
        render_frame=int(raw.render_frame),
        timeline_sample=int(raw.timeline_sample),
        audible_timeline_sample=int(raw.audible_timeline_sample),
        graph_latency_samples=int(raw.graph_latency_samples_q8) / _LATENCY_Q8_SCALE,
        value=int(raw.value),
    )


def _meter_telemetry_from_c(
    raw: SonareMeterTelemetryRecord,
) -> MeterTelemetryRecord:
    planes = max(0, min(int(raw.num_channels), len(raw.peak_db)))
    return MeterTelemetryRecord(
        target_id=int(raw.target_id),
        render_frame=int(raw.render_frame),
        seq=int(raw.seq),
        num_channels=planes,
        peak_db=_from_c_float_array(raw.peak_db, planes),
        rms_db=_from_c_float_array(raw.rms_db, planes),
        true_peak_dbtp=_from_c_float_array(raw.true_peak_dbtp, planes),
        max_true_peak_dbtp=float(raw.max_true_peak_dbtp),
        correlation=float(raw.correlation),
        mono_compat_width=float(raw.mono_compat_width),
        momentary_lufs=float(raw.momentary_lufs),
        short_term_lufs=float(raw.short_term_lufs),
        integrated_lufs=float(raw.integrated_lufs),
        gain_reduction_db=float(raw.gain_reduction_db),
        dropped_records=int(raw.dropped_records),
        input_peak_db=_from_c_float_array(raw.input_peak_db, planes),
    )


def _scope_telemetry_from_c(raw: SonareScopeTelemetryRecord) -> ScopeTelemetryRecord:
    # Clamped to the mirror arrays the way the meter reader above and the WASM
    # reader both are: a count past the fixed capacity truncates rather than
    # raising IndexError on one surface while another returns a short record.
    # The point mirror is flattened to interleaved floats, so it holds half as
    # many points as it does entries.
    band_count = max(0, min(int(raw.band_count), len(raw.bands)))
    point_count = max(0, min(int(raw.point_count), len(raw.points) // 2))
    bands = _from_c_float_array(raw.bands, band_count)
    points = [(float(raw.points[2 * i]), float(raw.points[2 * i + 1])) for i in range(point_count)]
    return ScopeTelemetryRecord(
        target_id=int(raw.target_id),
        render_frame=int(raw.render_frame),
        seq=int(raw.seq),
        dropped_records=int(raw.dropped_records),
        bands=bands,
        points=points,
    )
