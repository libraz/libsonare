"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Any, Literal

from . import _ffi_types_core as _ffi_core
from . import _ffi_types_mastering_project as _ffi_project
from ._engine_conversions import (
    _LATENCY_Q8_SCALE,
    _POSITIVE_COUNT,
    _assign_marker_ids,
    _automation_points_to_c,
    _clips_to_c,
    _fixed_bytes,
    _given_positive,
    _marker_from_c,
    _marker_to_c,
    _metronome_from_c,
    _metronome_to_c,
    _parameter_from_c,
)
from ._engine_conversions import (
    _band_json_arg as _band_json_arg,
)
from ._engine_conversions import (
    _c_string as _c_string,
)
from ._engine_conversions import (
    _capture_source_name as _capture_source_name,
)
from ._engine_conversions import (
    _capture_source_value as _capture_source_value,
)
from ._engine_conversions import (
    _graph_connection_to_c as _graph_connection_to_c,
)
from ._engine_conversions import (
    _graph_node_to_c as _graph_node_to_c,
)
from ._engine_conversions import (
    _graph_parameter_binding_to_c as _graph_parameter_binding_to_c,
)
from ._engine_conversions import (
    _meter_telemetry_from_c as _meter_telemetry_from_c,
)
from ._engine_conversions import (
    _scope_telemetry_from_c as _scope_telemetry_from_c,
)
from ._engine_conversions import (
    _telemetry_from_c as _telemetry_from_c,
)
from ._engine_conversions import (
    _track_monitor_mode_value as _track_monitor_mode_value,
)
from ._engine_io import _EngineIoMixin
from ._engine_midi import _EngineMidiMixin
from ._engine_mixing import _EngineMixingMixin
from ._engine_pages import ClipPageProvider as ClipPageProvider
from ._engine_pages import FileClipPageProvider as FileClipPageProvider
from ._errors import (
    ErrorCode,
    SonareError,
    _entry,
    _invalid_state,
    _refuse,
    _require_open,
)
from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._mixing import StripRef
from ._narrowing import _narrow_double, _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _METER_SEGMENT_KEYS,
    _TEMPO_SEGMENT_KEYS,
    _UINT8_MAX,
    _UINT32_MAX,
    SonareValueError,
    _check,
    _curve_value,
    _get_lib,
    _int_refusal,
    _mapping_keys,
    _max_block_size,
    _narrow_int,
    _optional_positive_int,
    _require_instance,
    _resolve_render_frame,
    _to_c_float,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint32,
)
from .types import (
    AutomationPoint,
    EngineClip,
    EngineMarker,
    EngineMarkerInput,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    EngineTrackMonitorMode,
    ExternalMidiEvent,
    ParameterInfo,
    TimeSignature,
    TransportState,
)

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's _EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
_EXPECTED_ENGINE_ABI_VERSION = 4
# The C record's destination_id for a byte meant for every external port.
_ALL_EXTERNAL_PORTS = 0xFFFFFFFF

# SONARE_ENGINE_MAX_TRACK_LANES: most lanes RealtimeEngine.set_track_lanes accepts.
MAX_TRACK_LANES = 32
"""Most track lanes :meth:`RealtimeEngine.set_track_lanes` accepts."""

# The most MIDI 1.0 messages one queued external-MIDI record can lower to, and
# therefore the smallest drain budget that can consume a record at all. Mirrors
# sonare::host::ExternalMidi1Lowered::messages, which the C ABI reads for the
# same guard (src/c_api/sonare_c_engine_midi.cpp).
_MAX_LOWERED_MIDI1_MESSAGES = 4


if TYPE_CHECKING:
    from ._project import ProjectTimeline


class RealtimeEngine(_EngineMidiMixin, _EngineMixingMixin, _EngineIoMixin):
    """Thin Python wrapper around the native realtime engine handle."""

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None
    # Lowest id set_markers may allocate; never moves back, so an id is not reused.
    _next_marker_id: int = 1

    @_entry("RealtimeEngine")
    def __init__(
        self,
        sample_rate: int,
        max_block_size: int | None = None,
        *,
        command_capacity: int | None = None,
        telemetry_capacity: int | None = None,
        max_channels: int | None = None,
    ) -> None:
        """Create and prepare an engine; see :meth:`prepare` for the arguments."""
        # Set first so a failed create -- an ABI mismatch is the reachable one --
        # leaves a valid attribute for __del__/close() instead of raising
        # AttributeError over the real error.
        self._handle: ctypes.c_void_p | None = None
        lib = _get_lib()
        abi_version = int(lib.sonare_engine_abi_version())
        if abi_version != _EXPECTED_ENGINE_ABI_VERSION:
            raise SonareError(
                int(ErrorCode.ABI_MISMATCH),
                f"libsonare engine ABI mismatch: native binary reports {abi_version}, "
                f"expected {_EXPECTED_ENGINE_ABI_VERSION}. The installed shared library is "
                "incompatible with this Python binding.",
            )
        handle = ctypes.c_void_p()
        _check(lib.sonare_engine_create(ctypes.byref(handle)))
        self._handle = handle
        self._capture_arrays: list[ctypes.Array[ctypes.c_float]] = []
        self._capture_ptrs: ctypes.Array[Any] | None = None
        # Track ids in lane order as set_track_lanes declared them; "timeline" once a
        # project timeline installed lanes this facade cannot see.
        self._lane_order: list[int] | Literal["timeline"] = []
        # Page providers the published clips read from, kept alive by clip id.
        self._clip_page_providers: dict[int, list[ClipPageProvider]] = {}
        self.prepare(
            sample_rate, max_block_size, command_capacity, telemetry_capacity, max_channels
        )

    @_entry("RealtimeEngine.close")
    def close(self) -> None:
        if self._handle is not None:
            _get_lib().sonare_engine_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> RealtimeEngine:
        """Return the engine for use as a context manager."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the engine on leaving the ``with`` block."""
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "RealtimeEngine")

    @_entry("RealtimeEngine.apply_project_timeline")
    def apply_project_timeline(self, timeline: ProjectTimeline) -> None:
        """Install a compiled project timeline into this stopped engine.

        All or nothing: a playing transport raises (``INVALID_STATE``) and a
        timeline the engine cannot validate raises, both leaving the engine
        unchanged. The timeline replaces tempo and time-signature segments,
        markers, track lanes and automation, and audio and MIDI clips; the
        instrument, bus, master and metronome setup are not touched. The engine
        keeps its own share of the timeline, so ``timeline.close()`` may follow
        immediately.

        Raises on an unprepared engine (``INVALID_STATE``) and on a timeline
        whose sample rate differs from the engine's. Once a timeline is applied,
        a later ``prepare`` at another rate is refused until the timeline's clips
        are cleared.
        """
        from ._project import ProjectTimeline

        _require_instance(timeline, ProjectTimeline, "timeline")
        _check(
            _get_lib().sonare_engine_apply_project_timeline(
                self._require_handle(), timeline._require_handle()
            )
        )
        self._lane_order = "timeline"

    @_entry("RealtimeEngine.prepare")
    def prepare(
        self,
        sample_rate: int,
        max_block_size: int | None = None,
        command_capacity: int | None = None,
        telemetry_capacity: int | None = None,
        max_channels: int | None = None,
    ) -> None:
        """Size the engine's queues and scratch for a sample rate and block size.

        Each size left out takes the library default; an explicit 0 or a
        negative value is refused. ``command_capacity`` must not exceed 65536
        and ``telemetry_capacity`` must not exceed 16384; a larger value raises
        and leaves the engine untouched. The telemetry number is not a queue
        depth paid for one-for-one: the engine reserves that many meter records
        per metered lane, so its memory cost is far larger than the number given
        here.
        """
        _check(
            _get_lib().sonare_engine_prepare(
                self._require_handle(),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(
                    _resolve_zero_default(max_block_size, "max_block_size"), "max_block_size"
                ),
                _to_c_size_t(
                    _resolve_zero_default(command_capacity, "command_capacity"), "command_capacity"
                ),
                _to_c_size_t(
                    _resolve_zero_default(telemetry_capacity, "telemetry_capacity"),
                    "telemetry_capacity",
                ),
                _to_c_int(_resolve_zero_default(max_channels, "max_channels"), "max_channels"),
            )
        )
        self._sample_rate = _to_c_int(sample_rate, "sample_rate").value

    @_entry("RealtimeEngine.play")
    def play(self, render_frame: int | None = None) -> None:
        _check(
            _get_lib().sonare_engine_play(
                self._require_handle(),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.stop")
    def stop(self, render_frame: int | None = None) -> None:
        """Stop the transport.

        A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
        centred pitch bend on every channel played since the last reset. When
        playback next rolls, the controller, program, channel pressure and pitch
        bend values the clips set before the new position are sent again, so each
        loop pass and each seek starts from the state continuous playback reaches.
        """
        _check(
            _get_lib().sonare_engine_stop(
                self._require_handle(),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.seek_sample")
    def seek_sample(self, timeline_sample: int, render_frame: int | None = None) -> None:
        """Move the playhead to a timeline sample.

        A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
        centred pitch bend on every channel played since the last reset. When
        playback next rolls, the controller, program, channel pressure and pitch
        bend values the clips set before the new position are sent again, so each
        loop pass and each seek starts from the state continuous playback reaches.
        """
        _check(
            _get_lib().sonare_engine_seek_sample(
                self._require_handle(),
                _to_c_int64(timeline_sample, "timeline_sample"),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.settle_parameters")
    def settle_parameters(self) -> None:
        """Snap every in-flight parameter ramp to its target value.

        Covers engine-level smoothed parameters and the mixer's lane
        fader/pan/gate and bus gain smoothers. It is one step of
        :meth:`prime_offline_parameters`, which offline renders call before the
        first audible block so it renders at the settled values instead of
        ramping in from defaults.
        """
        _check(_get_lib().sonare_engine_settle_parameters(self._require_handle()))

    @_entry("RealtimeEngine.prime_offline_parameters")
    def prime_offline_parameters(self, num_channels: int, block_size: int | None = None) -> None:
        """Bring an offline engine to its start-of-render state without rendering.

        Applies queued commands, returns the mixer and effect processors to
        their prepared state, adopts the published snapshots, resolves
        automation and lane gates at the transport position and snaps every
        smoother. Call once before the first :meth:`render_offline` chunk. An
        omitted ``block_size`` is the default offline block, capped at the
        prepared maximum; an explicit 0 is refused. Raises for a channel or block
        count outside the prepared shape or an engine that was never prepared.
        Not concurrent with processing.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_prime_offline_parameters(
                self._require_handle(),
                _to_c_int(num_channels, "num_channels"),
                _to_c_int(_resolve_zero_default(block_size, "block_size"), "block_size"),
            )
        )

    @property
    def max_block_size(self) -> int:
        """The largest block :meth:`process` accepts; 0 before the engine is prepared."""
        return _max_block_size(_get_lib().sonare_engine_max_block_size, self._require_handle())

    @property
    def tail_samples(self) -> int:
        """Longest audible tail after the last input, in samples (an upper bound).

        The bound is conservative before the first note (a drum-kit or General
        MIDI program lane has no patch yet) and narrows once a patch has sounded.
        2147483647 means the tail is unbounded. Control thread only; not
        concurrent with processing.
        """
        lib = _get_lib()
        out = ctypes.c_int()
        _check(lib.sonare_engine_tail_samples(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @property
    def graph_latency_samples(self) -> float:
        """Processing latency of the engine in samples (fractional).

        The value telemetry records carry as ``graph_latency_samples``.
        """
        lib = _get_lib()
        out = ctypes.c_int()
        _check(
            lib.sonare_engine_graph_latency_samples_q8(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value) / _LATENCY_Q8_SCALE

    @_entry("RealtimeEngine.flush_control_commands")
    def flush_control_commands(self) -> None:
        """Apply commands queued on an offline/control-only engine immediately.

        A host that never calls process() still queues onto a bounded realtime
        command ring, so those commands have to be drained explicitly rather
        than by the next process() call. Not safe to call concurrently with a
        running process().
        """
        _check(_get_lib().sonare_engine_flush_control_commands(self._require_handle()))

    @_entry("RealtimeEngine.settle_insert_parameters")
    def settle_insert_parameters(self) -> None:
        """Snap every in-flight strip insert parameter smoother to its target.

        Insert-parameter counterpart of :meth:`settle_parameters`.
        """
        _check(_get_lib().sonare_engine_settle_insert_parameters(self._require_handle()))

    @_entry("RealtimeEngine.apply_commands_due_now_preserving_future")
    def apply_commands_due_now_preserving_future(self) -> None:
        """Apply commands already due at the current render frame.

        Control-only mirror of :meth:`flush_control_commands` that keeps
        future-dated commands queued in their original order instead of
        draining them too. Not safe to call concurrently with a running
        :meth:`process`.
        """
        _check(
            _get_lib().sonare_engine_apply_commands_due_now_preserving_future(
                self._require_handle()
            )
        )

    @_entry("RealtimeEngine.seek_ppq")
    def seek_ppq(self, ppq: float, render_frame: int | None = None) -> None:
        """Move the playhead to a PPQ position.

        A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
        centred pitch bend on every channel played since the last reset. When
        playback next rolls, the controller, program, channel pressure and pitch
        bend values the clips set before the new position are sent again, so each
        loop pass and each seek starts from the state continuous playback reaches.
        """
        _check(
            _get_lib().sonare_engine_seek_ppq(
                self._require_handle(),
                _narrow_double(ppq, "ppq"),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.set_tempo")
    def set_tempo(self, bpm: float) -> None:
        """Set a finite tempo in the range ``(0, 100000]`` BPM."""
        _check(
            _get_lib().sonare_engine_set_tempo(self._require_handle(), _narrow_double(bpm, "bpm"))
        )

    @_entry("RealtimeEngine.set_time_signature")
    def set_time_signature(self, numerator: int, denominator: int) -> None:
        _check(
            _get_lib().sonare_engine_set_time_signature(
                self._require_handle(),
                _to_c_int(numerator, "numerator"),
                _to_c_int(denominator, "denominator"),
            )
        )

    @_entry("RealtimeEngine.set_tempo_segments")
    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float]],
    ) -> None:
        """Install a tempo map from ramp segments.

        Each segment is a mapping with ``start_ppq``, ``bpm`` and an optional
        ``end_bpm``.
        ``bpm`` and a non-zero ``end_bpm`` must be at most 100000. ``end_bpm``
        0 means a constant-tempo segment. Pass an empty sequence to clear the map.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (_ffi_project.SonareProjectTempoSegment * count)() if count else None
        for i, seg in enumerate(rows):
            fields = _mapping_keys(seg, _TEMPO_SEGMENT_KEYS, f"segments[{i}]")
            start_ppq = _narrow_double(fields["start_ppq"], f"segments[{i}].start_ppq")
            bpm = _narrow_double(fields["bpm"], f"segments[{i}].bpm")
            end_bpm = _narrow_double(fields.get("end_bpm", 0.0), f"segments[{i}].end_bpm")
            c_segments[i].start_ppq = start_ppq
            c_segments[i].bpm = bpm
            c_segments[i].end_bpm = end_bpm
        _check(
            _get_lib().sonare_engine_set_tempo_segments(
                self._require_handle(), c_segments, _to_c_size_t(count, "count")
            )
        )

    @_entry("RealtimeEngine.set_time_signature_segments")
    def set_time_signature_segments(
        self,
        segments: Sequence[Mapping[str, float]],
    ) -> None:
        """Install a time-signature map.

        Each segment is a mapping with ``start_ppq``, ``numerator`` and
        ``denominator``. Pass an empty sequence to clear the map.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (_ffi_project.SonareProjectTimeSignatureSegment * count)() if count else None
        # The signature fields are carried unconverted so the struct's own
        # narrowing sees each caller value; int() truncated 4.5 onto the legal 4,
        # which the core's positive-signature check then accepts as written.
        for i, seg in enumerate(rows):
            fields = _mapping_keys(seg, _METER_SEGMENT_KEYS, f"segments[{i}]")
            c_segments[i].start_ppq = _narrow_double(
                fields["start_ppq"], f"segments[{i}].start_ppq"
            )
            c_segments[i].numerator = fields["numerator"]
            c_segments[i].denominator = fields["denominator"]
        _check(
            _get_lib().sonare_engine_set_time_signature_segments(
                self._require_handle(), c_segments, _to_c_size_t(count, "count")
            )
        )

    @_entry("RealtimeEngine.sample_at_ppq")
    def sample_at_ppq(self, ppq: float) -> int:
        """Convert PPQ to a timeline sample using the engine tempo-map snapshot."""
        lib = _get_lib()
        out = ctypes.c_int64()
        _check(
            lib.sonare_engine_sample_at_ppq(
                self._require_handle(), _narrow_double(ppq, "ppq"), ctypes.byref(out)
            )
        )
        return int(out.value)

    @_entry("RealtimeEngine.set_loop")
    def set_loop(self, start_ppq: float, end_ppq: float, enabled: bool = True) -> None:
        """Set the loop region and whether it is enabled.

        A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
        centred pitch bend on every channel played since the last reset. When
        playback next rolls, the controller, program, channel pressure and pitch
        bend values the clips set before the new position are sent again, so each
        loop pass and each seek starts from the state continuous playback reaches.
        """
        _check(
            _get_lib().sonare_engine_set_loop(
                self._require_handle(),
                _narrow_double(start_ppq, "start_ppq"),
                _narrow_double(end_ppq, "end_ppq"),
                1 if enabled else 0,
            )
        )

    @_entry("RealtimeEngine.add_parameter")
    def add_parameter(self, info: ParameterInfo) -> None:
        _require_instance(info, ParameterInfo, "info")
        raw = _ffi_core.SonareParameterInfo()
        # Assigned unconverted so the struct's own narrowing sees the caller's
        # value; int() would truncate a fraction onto a neighbouring parameter
        # id. rt_safe is the exception -- it is a documented bool, spelled 0/1.
        raw.id = info.id
        raw.name = _fixed_bytes(info.name, 64, "name")
        raw.unit = _fixed_bytes(info.unit, 16, "unit")
        raw.min_value = float(info.min_value)
        raw.max_value = float(info.max_value)
        raw.default_value = float(info.default_value)
        raw.rt_safe = 1 if info.rt_safe else 0
        raw.default_curve = _curve_value(info.default_curve, "info.default_curve")
        _check(_get_lib().sonare_engine_add_parameter(self._require_handle(), ctypes.byref(raw)))

    @_entry("RealtimeEngine.clear_parameters")
    def clear_parameters(self) -> None:
        """Remove all registered parameters and release their backing strings.

        Use before re-registering a parameter id to change its metadata
        (:meth:`add_parameter` rejects duplicate ids). Control-thread only; not
        realtime-safe.
        """
        lib = _get_lib()
        _check(lib.sonare_engine_clear_parameters(self._require_handle()))

    @_entry("RealtimeEngine.parameters")
    def parameters(self) -> list[ParameterInfo]:
        """Every registered parameter, in stored order."""
        lib = _get_lib()
        handle = self._require_handle()
        count = ctypes.c_size_t()
        _check(lib.sonare_engine_parameter_count(handle, ctypes.byref(count)))
        out: list[ParameterInfo] = []
        for index in range(int(count.value)):
            raw = _ffi_core.SonareParameterInfo()
            _check(
                lib.sonare_engine_parameter_info_by_index(
                    handle, _to_c_size_t(index, "index"), ctypes.byref(raw)
                )
            )
            out.append(_parameter_from_c(raw))
        return out

    @_entry("RealtimeEngine.parameter_info")
    def parameter_info(self, id: int) -> ParameterInfo:
        raw = _ffi_core.SonareParameterInfo()
        _check(
            _get_lib().sonare_engine_parameter_info(
                self._require_handle(), _to_c_uint32(id, "id"), ctypes.byref(raw)
            )
        )
        return _parameter_from_c(raw)

    @_entry("RealtimeEngine.set_automation_lane")
    def set_automation_lane(self, param_id: int, points: Sequence[AutomationPoint]) -> None:
        """Replace the automation lane driving ``param_id``.

        An empty lane (``points`` empty) leaves the target undriven rather
        than snapping it to 0 or a default: once the change is adopted, the
        target reverts to the last value explicitly sent through
        :meth:`set_parameter`, or is left
        unchanged if no such value was ever sent for ``param_id``. This holds
        regardless of whether the manual value or the lane clear is adopted
        first.
        """
        raw_points = _automation_points_to_c(points)
        _check(
            _get_lib().sonare_engine_set_automation_lane(
                self._require_handle(),
                _to_c_uint32(param_id, "param_id"),
                raw_points,
                len(points),
            )
        )

    @property
    def automation_lane_count(self) -> int:
        out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_automation_lane_count(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    @_entry("RealtimeEngine.set_markers")
    def set_markers(
        self, markers: Sequence[EngineMarker | EngineMarkerInput]
    ) -> list[EngineMarker]:
        """Replace the whole marker set and return the stored markers in the order given.

        An :class:`EngineMarkerInput` whose ``id`` is ``None`` is given the lowest id
        above every id this engine has stored or assigned that the list does not
        carry, so an id is not
        reused after its marker is removed. An explicit id is kept; it must be
        positive and unique within the list. A refused list changes neither the
        stored markers nor the ids later allocations start from.
        """
        floor = self._next_marker_id
        for stored in self.markers():
            floor = max(floor, stored.id + 1)
        assigned, next_id = _assign_marker_ids(list(markers), floor)
        raw = [_marker_to_c(marker, f"markers[{index}]") for index, marker in enumerate(assigned)]
        raw_markers = (_ffi_core.SonareEngineMarker * len(raw))(*raw)
        _check(_get_lib().sonare_engine_set_markers(self._require_handle(), raw_markers, len(raw)))
        self._next_marker_id = next_id
        return [self.marker(int(entry.id)) for entry in raw]

    @_entry("RealtimeEngine.markers")
    def markers(self) -> list[EngineMarker]:
        """Every marker, in stored order."""
        lib = _get_lib()
        handle = self._require_handle()
        count = ctypes.c_size_t()
        _check(lib.sonare_engine_marker_count(handle, ctypes.byref(count)))
        out: list[EngineMarker] = []
        for index in range(int(count.value)):
            raw = _ffi_core.SonareEngineMarker()
            _check(
                lib.sonare_engine_marker_by_index(
                    handle, _to_c_size_t(index, "index"), ctypes.byref(raw)
                )
            )
            out.append(_marker_from_c(raw))
        return out

    @_entry("RealtimeEngine.marker")
    def marker(self, id: int) -> EngineMarker:
        raw = _ffi_core.SonareEngineMarker()
        _check(
            _get_lib().sonare_engine_marker(
                self._require_handle(), _to_c_uint32(id, "id"), ctypes.byref(raw)
            )
        )
        return _marker_from_c(raw)

    @_entry("RealtimeEngine.seek_marker")
    def seek_marker(self, marker_id: int, render_frame: int | None = None) -> None:
        _check(
            _get_lib().sonare_engine_seek_marker(
                self._require_handle(),
                _to_c_uint32(marker_id, "marker_id"),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.set_loop_from_markers")
    def set_loop_from_markers(self, start_marker_id: int, end_marker_id: int) -> None:
        _check(
            _get_lib().sonare_engine_set_loop_from_markers(
                self._require_handle(),
                _to_c_uint32(start_marker_id, "start_marker_id"),
                _to_c_uint32(end_marker_id, "end_marker_id"),
            )
        )

    @_entry("RealtimeEngine.set_metronome")
    def set_metronome(self, config: EngineMetronomeConfig) -> None:
        raw = _metronome_to_c(config)
        _check(_get_lib().sonare_engine_set_metronome(self._require_handle(), ctypes.byref(raw)))

    @_entry("RealtimeEngine.metronome")
    def metronome(self) -> EngineMetronomeConfig:
        raw = _ffi_core.SonareEngineMetronomeConfig()
        _check(_get_lib().sonare_engine_metronome(self._require_handle(), ctypes.byref(raw)))
        return _metronome_from_c(raw)

    @_entry("RealtimeEngine.count_in_end_sample")
    def count_in_end_sample(self, start_sample: int, bars: int) -> int:
        """Return the sample at which a count-in of ``bars`` bars from ``start_sample`` ends.

        Counting starts at the beginning of the bar containing ``start_sample`` (a mid-bar start
        gets a short first bar), and every counted bar has the length of the time signature in
        effect at ``start_sample``. Returns ``start_sample`` when ``bars <= 0``.
        """
        out = ctypes.c_int64()
        _check(
            _get_lib().sonare_engine_count_in_end_sample(
                self._require_handle(),
                _to_c_int64(start_sample, "start_sample"),
                _to_c_int(bars, "bars"),
                ctypes.byref(out),
            )
        )
        return int(out.value)

    @_entry("RealtimeEngine.set_clips")
    def set_clips(self, clips: Sequence[EngineClip]) -> None:
        page_providers: dict[int, list[ClipPageProvider]] = {}
        for clip in clips:
            _require_instance(clip, EngineClip, "clip")
            provider = getattr(clip, "page_provider", None)
            if isinstance(provider, ClipPageProvider):
                page_providers.setdefault(clip.id, []).append(provider)
        raw_clips, _channel_arrays, _channel_ptrs, _warp_arrays = _clips_to_c(clips)
        _check(_get_lib().sonare_engine_set_clips(self._require_handle(), raw_clips, len(clips)))
        self._clip_page_providers = page_providers

    @_entry("RealtimeEngine.upsert_clip")
    def upsert_clip(self, clip: EngineClip) -> None:
        """Replace the clip with ``clip.id``, or add it, keeping every other clip.

        The clip is validated like a :meth:`set_clips` entry; a refused clip
        raises and leaves the published set unchanged.
        """
        raw_clips, _channel_arrays, _channel_ptrs, _warp_arrays = _clips_to_c([clip], indexed=False)
        _check(_get_lib().sonare_engine_upsert_clip(self._require_handle(), raw_clips))
        provider = getattr(clip, "page_provider", None)
        if isinstance(provider, ClipPageProvider):
            self._clip_page_providers[clip.id] = [provider]
        else:
            self._clip_page_providers.pop(clip.id, None)

    @_entry("RealtimeEngine.remove_clip")
    def remove_clip(self, clip_id: int) -> None:
        """Remove the clip ``clip_id``, keeping every other clip.

        Raises when no clip has that id.
        """
        _check(
            _get_lib().sonare_engine_remove_clip(
                self._require_handle(), _to_c_uint32(clip_id, "clip_id")
            )
        )
        self._clip_page_providers.pop(clip_id, None)

    @property
    def clip_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_clip_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @_entry("RealtimeEngine.set_strip_fader_db")
    def set_strip_fader_db(
        self, ref: StripRef, fader_db: float, render_frame: int | None = None
    ) -> None:
        """Set a strip's fader in decibels in realtime, through its reserved automation id.

        ``ref`` is a track id or ``"master"``; ``render_frame`` is the render-frame time
        to apply, None for immediate.
        """
        param_id = self.resolve_strip_automation_id(ref, "faderDb")
        self.set_parameter(param_id, fader_db, render_frame)

    @_entry("RealtimeEngine.set_bus_gain_db")
    def set_bus_gain_db(self, bus_id: int, gain_db: float, render_frame: int | None = None) -> None:
        """Set a bus's gain in decibels in realtime, through its reserved automation id."""
        param_id = self.resolve_bus_automation_id(bus_id, "faderDb")
        self.set_parameter(param_id, gain_db, render_frame)

    @_entry("RealtimeEngine.set_parameter")
    def set_parameter(
        self,
        param_id: int,
        value: float,
        render_frame: int | None = None,
        *,
        smoothed: bool = False,
    ) -> None:
        """Push a live parameter value to the engine (immediate jump by default).

        ``render_frame`` is the render-frame time to apply (``None`` for
        immediate). With ``smoothed=True`` the value is reached through a
        smoothed ramp instead of a jump; the ramp's target (not its in-flight
        position) becomes the base value. This value also becomes ``param_id``'s base value: if an
        automation lane later starts (and stops) driving ``param_id``, the
        target reverts to this value once that lane empties -- see
        :meth:`set_automation_lane`. A reserved mixer id (``0x4D58xxxx``) other
        than a master id names no strip and raises :class:`SonareValueError`;
        resolve ids with the ``resolve_*_automation_id`` methods
        (``resolve_strip_automation_id("master", ...)`` for the master fader, pan and width).
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_parameter(
                self._require_handle(),
                _to_c_uint32(param_id, "param_id"),
                _to_c_float(value, "value"),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
                1 if smoothed else 0,
            )
        )

    @_entry("RealtimeEngine.set_param_smoothing_ms")
    def set_param_smoothing_ms(self, smoothing_ms: float) -> None:
        """Set the ramp time (ms) of the engine-level smoother.

        The time is one time constant: about 63% of the way at the set time.
        Governs every ``set_parameter(..., smoothed=True)``, reserved fader, pan,
        insert and instrument targets included: a smoothed set on a reserved target
        ramps over this time and then through the target's fixed 5 ms click guard,
        which a stepped set passes through too. Changing it retimes ramps already
        running, and MIDI-CC mappings apply their values directly. The default is
        20 ms; ``0`` makes a smoothed set a step.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_param_smoothing_ms(
                self._require_handle(), _to_c_float(smoothing_ms, "smoothing_ms")
            )
        )

    def _lane_index_of(self, track_id: int) -> int:
        """Lane index of ``track_id`` in set_track_lanes order; an unknown id is refused."""
        track = _narrow_int(track_id, "track_id", 0, _UINT32_MAX)
        if self._lane_order == "timeline":
            raise _invalid_state(
                "the lanes were installed by a project timeline; declare them with set_track_lanes"
            )
        try:
            return self._lane_order.index(track)
        except ValueError:
            raise _refuse("track_id must be the id of a track in this engine") from None

    @_entry("RealtimeEngine.set_track_solo_mute")
    def set_track_solo_mute(
        self, track_id: int, *, solo: bool, mute: bool, render_frame: int | None = None
    ) -> None:
        """Queue a track's solo/mute state.

        ``track_id`` is a track declared through :meth:`set_track_lanes`; an unknown id
        raises ``SonareValueError`` and lanes installed by a project timeline raise
        ``INVALID_STATE``. An armed PFL/AFL cue (:meth:`set_track_monitor_mode`) is folded
        into the program output by ``process`` and kept separate by the ``*_with_monitor``
        variants; offline renders, bounces and freezes never include it.
        """
        lane_index = self._lane_index_of(track_id)
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_solo_mute(
                self._require_handle(),
                _to_c_uint32(lane_index, "lane_index"),
                1 if solo else 0,
                1 if mute else 0,
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.set_track_monitor_mode")
    def set_track_monitor_mode(
        self,
        track_id: int,
        mode: EngineTrackMonitorMode,
        render_frame: int | None = None,
    ) -> None:
        """Schedule a PFL/AFL monitor tap for a track at a render-frame boundary.

        ``mode`` is ``"off"``, ``"pfl"`` or ``"afl"``. ``track_id`` is a track declared
        through :meth:`set_track_lanes`; an unknown id raises ``SonareValueError``.

        ``process`` fold the armed cue into the program
        output, so the tapped lane is counted twice there; the
        ``*_with_monitor`` variants keep the cue separate. Offline renders,
        bounces and freezes never include the cue.
        """
        lane_index = self._lane_index_of(track_id)
        mode_value = _track_monitor_mode_value(mode, "mode")
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_track_monitor_mode(
                self._require_handle(),
                _to_c_uint32(lane_index, "lane_index"),
                _to_c_int(mode_value, "mode"),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.set_midi_clips")
    def set_midi_clips(self, clips: Sequence[EngineMidiClipSchedule]) -> None:
        """Replace the realtime MIDI clip snapshot with compiled schedules."""
        lib = _get_lib()
        event_arrays: list[ctypes.Array[_ffi_project.SonareEngineMidiEvent]] = []
        raw_clips = (_ffi_project.SonareEngineMidiClipSchedule * len(clips))()
        for index, clip in enumerate(clips):
            _require_instance(clip, EngineMidiClipSchedule, "clip")
            raw_events = (_ffi_project.SonareEngineMidiEvent * len(clip.events))()
            for event_index, event in enumerate(clip.events):
                # ctypes narrows to the c_uint8 field silently, so a group of 256
                # would arrive as 0 and pass the C ABI's range check instead of
                # being rejected, and int() folded 5.5 onto the legal group 5.
                # Narrow both away here; the C ABI still owns the finer [0, 15]
                # MIDI range.
                group_name = f"clips[{index}].events[{event_index}].group"
                try:
                    group = _narrow_int(event.group, group_name, 0, _UINT8_MAX)
                except (TypeError, SonareValueError) as exc:
                    raise _int_refusal(
                        event.group,
                        group_name,
                        f"must be an integer in [0, {_UINT8_MAX}]",
                    ) from exc
                # Same tolerance the C bridge documents: a word_count outside
                # [1, 4] means "infer the word form", so this narrows integrality
                # only -- a range refusal would reject what the bridge defines.
                # int() truncated 2.5 onto a word form the caller never asked for.
                word_count = _narrow_int(
                    event.word_count,
                    f"clips[{index}].events[{event_index}].word_count",
                    _C_INT_MIN,
                    _C_INT_MAX,
                )
                # The remaining fields go in unconverted so the struct's own
                # narrowing sees each caller value.
                raw_events[event_index] = _ffi_project.SonareEngineMidiEvent(
                    event.render_frame,
                    event.word0,
                    event.word1,
                    event.word2,
                    event.word3,
                    word_count if 1 <= word_count <= 4 else 0,
                    group,
                    0,
                    event.sysex_handle,
                )
            event_arrays.append(raw_events)
            track_id = _optional_positive_int(clip.track_id, "clip.track_id", _UINT32_MAX)
            # An omitted length keeps the init's OPEN_ENDED and an omitted loop period its 0;
            # a given 0 length is the C refusal's.
            raw_clips[index] = _ffi_project.SonareEngineMidiClipSchedule._given(
                id=_optional_positive_int(clip.id, "clip.id", _UINT32_MAX),
                track_id=track_id,
                start_sample=clip.start_sample,
                start_ppq=_narrow_double(clip.start_ppq, f"clips[{index}].start_ppq"),
                length_samples=_given_positive(
                    clip.length_samples, f"clips[{index}].length_samples", _POSITIVE_COUNT
                ),
                loop=1 if clip.loop else 0,
                loop_length_samples=clip.loop_length_samples,
                destination_id=track_id if clip.destination_id is None else clip.destination_id,
                events=raw_events,
                event_count=len(clip.events),
                gain=_narrow_double(clip.gain, f"clips[{index}].gain"),
                fade_in_samples=clip.fade_in_samples,
                fade_out_samples=clip.fade_out_samples,
            )
        _check(
            lib.sonare_engine_set_midi_clips(
                self._require_handle(), raw_clips, ctypes.c_size_t(len(clips))
            )
        )
        _ = event_arrays

    @_entry("RealtimeEngine.set_midi_destination_external")
    def set_midi_destination_external(self, destination_id: int, external: bool) -> None:
        """Mark a MIDI destination for external routing (or clear it).

        A destination marked external bypasses the internal instrument rack; its
        sequenced events are buffered in the engine's external-MIDI output queue
        for the host to drain with :meth:`drain_external_midi`. ``external`` False
        clears the mark. Control-thread only. The change takes effect at the next
        processed block; switching a destination's route first releases its notes
        and resets its controllers through the old route, and drops its pending
        MIDI-FX events. Single writer.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_midi_destination_external(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                1 if external else 0,
            )
        )

    @_entry("RealtimeEngine.set_external_midi_clock_enabled")
    def set_external_midi_clock_enabled(self, enabled: bool) -> None:
        """Enable/disable forwarding MIDI clock/transport bytes to the external queue.

        When enabled, MIDI clock (0xF8) and transport (start/continue/stop) bytes
        are enqueued tagged with destination id ``0xFFFFFFFF`` so external gear can
        be tempo-synced. Off by default.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_external_midi_clock_enabled(
                self._require_handle(), 1 if enabled else 0
            )
        )

    @property
    def external_midi_dropped_count(self) -> int:
        """Number of external-MIDI events dropped because the queue was full.

        Advisory telemetry; monotonic within a prepared session.
        """
        lib = _get_lib()
        out = ctypes.c_uint64()
        _check(
            lib.sonare_engine_external_midi_dropped_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    @property
    def clip_page_request_overflow_count(self) -> int:
        """Number of clip-page requests dropped because the bounded queue was full.

        Advisory telemetry; monotonic within a prepared session and reset by
        :meth:`prepare`. A non-zero count means the host is draining
        :meth:`pop_clip_page_request` too slowly and some pages were never asked
        for, so their reads produced silence.
        """
        lib = _get_lib()
        out = ctypes.c_uint64()
        _check(
            lib.sonare_engine_clip_page_request_overflow_count(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    @property
    def warp_stretch_overflow_count(self) -> int:
        """Number of blocks in which a time-stretched clip fell back to resampling.

        Advisory telemetry; monotonic within a prepared session and reset by
        :meth:`prepare`. Only a fixed number of stretcher voices exist, so a
        project with more overlapping time-stretch clips than voices plays some
        of them pitch-shifted instead; a non-zero count is the only way to
        detect that degradation.
        """
        lib = _get_lib()
        out = ctypes.c_uint64()
        _check(
            lib.sonare_engine_warp_stretch_overflow_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    @_entry("RealtimeEngine.set_warp_voice_capacity")
    def set_warp_voice_capacity(self, voices: int) -> None:
        """Set the number of concurrent time-stretch voices.

        Voices must be in [0, 64]. Default is 8. Capacity 0 disables
        time-stretch: every warped clip plays resampled instead, and none of
        that counts toward :attr:`warp_stretch_overflow_count`. A change
        applied while the engine is running rebuilds the voice pool
        immediately; any clip stretching through a voice at that moment
        restarts its WSOLA state rather than carrying it over.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_warp_voice_capacity(
                self._require_handle(),
                ctypes.c_uint32(_narrow_int(voices, "voices", 0, 64)),
            )
        )

    @_entry("RealtimeEngine.warp_voice_capacity")
    def warp_voice_capacity(self) -> int:
        """Read the current time-stretch voice capacity.

        Returns the value most recently accepted by
        :meth:`set_warp_voice_capacity`, or the default (8) if it was never
        called.
        """
        lib = _get_lib()
        out = ctypes.c_uint32()
        _check(lib.sonare_engine_warp_voice_capacity(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @_entry("RealtimeEngine.drain_external_midi")
    def drain_external_midi(self, max_records: int = 1024) -> list[ExternalMidiEvent]:
        """Drain queued external-MIDI events, lowered to MIDI 1.0 byte messages.

        Each returned :class:`ExternalMidiEvent` is one MIDI 1.0 message (1..3
        bytes). ``max_records`` caps the number of output events returned — the
        shared unit across every surface. Events past the cap stay queued for the
        next call (lossless); call again to drain the rest. A remaining budget
        below the per-record lowering bound stops the drain rather than
        over-fetch.

        ``max_records`` of 0 or less drains nothing and returns an empty list. A
        positive budget below the lowering bound could never consume a record, so
        it is rejected with :class:`SonareValueError` — the same refusal the C
        ABI and the other bindings give it — instead of reporting nothing while
        the queue keeps growing.
        """
        if max_records <= 0:
            return []
        if max_records < _MAX_LOWERED_MIDI1_MESSAGES:
            raise _refuse(
                "max_records must be 0 or at least "
                f"{_MAX_LOWERED_MIDI1_MESSAGES} to guarantee forward progress"
            )
        lib = _get_lib()
        capacity = _to_c_size_t(max_records, "max_records").value
        raw = (_ffi_core.SonareExternalMidiEvent * capacity)()
        written = ctypes.c_size_t()
        results: list[ExternalMidiEvent] = []
        while len(results) < max_records:
            remaining = max_records - len(results)
            if remaining < _MAX_LOWERED_MIDI1_MESSAGES:
                break  # too small to lower one record without loss
            cap = min(capacity, remaining)
            _check(
                lib.sonare_engine_drain_external_midi(
                    self._require_handle(),
                    raw,
                    _to_c_size_t(cap, "max_records"),
                    ctypes.byref(written),
                )
            )
            count = int(written.value)
            if count == 0:
                break
            # count <= cap <= remaining, so every drained event fits the budget.
            for i in range(count):
                event = raw[i]
                byte_count = max(0, min(int(event.byte_count), len(event.bytes)))
                results.append(
                    ExternalMidiEvent(
                        destination_id=(
                            None
                            if int(event.destination_id) == _ALL_EXTERNAL_PORTS
                            else int(event.destination_id)
                        ),
                        render_frame=int(event.render_frame),
                        bytes=bytes(event.bytes[:byte_count]),
                    )
                )
        return results

    @_entry("RealtimeEngine.transport_state")
    def transport_state(self) -> TransportState:
        """Read the current engine transport state (playing/position/ppq/tempo)."""
        lib = _get_lib()
        raw = _ffi_core.SonareTransportState()
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
            sample_rate=int(raw.sample_rate),
            bar_start_ppq=float(raw.bar_start_ppq),
            bar_index=int(raw.bar_index),
            time_signature=TimeSignature(
                numerator=int(raw.time_signature.numerator),
                denominator=int(raw.time_signature.denominator),
                confidence=float(raw.time_signature.confidence),
            ),
            beat=int(raw.beat),
            beat_fraction=float(raw.beat_fraction),
        )


_rebind_facade_exports(globals(), "libsonare._engine_")
del _rebind_facade_exports
