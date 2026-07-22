"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import Any

from ._engine_conversions import (
    _CAPTURE_SOURCE_VALUES as _CAPTURE_SOURCE_VALUES,
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
    _clips_to_c,
    _fixed_bytes,
    _marker_from_c,
    _marker_to_c,
    _metronome_from_c,
    _metronome_to_c,
    _parameter_from_c,
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
    _meter_telemetry_wide_from_c as _meter_telemetry_wide_from_c,
)
from ._engine_conversions import (
    _scope_telemetry_from_c as _scope_telemetry_from_c,
)
from ._engine_conversions import (
    _telemetry_from_c as _telemetry_from_c,
)
from ._engine_io import _EngineIoMixin
from ._engine_midi import _EngineMidiMixin
from ._engine_mixing import _EngineMixingMixin
from ._engine_pages import ClipPageProvider as ClipPageProvider
from ._engine_pages import FileClipPageProvider as FileClipPageProvider
from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._ffi_types_core import (
    SonareAutomationPoint,
    SonareEngineMarker,
    SonareEngineMetronomeConfig,
    SonareExternalMidiEvent,
    SonareParameterInfo,
    SonareTransportState,
)
from ._ffi_types_mastering_project import (
    SonareEngineMidiClipSchedule,
    SonareEngineMidiEvent,
    SonareProjectTempoSegment,
    SonareProjectTimeSignatureSegment,
)
from ._runtime import (
    _check,
    _get_lib,
)
from .types import (
    AutomationCurve,
    AutomationPoint,
    EngineClip,
    EngineMarker,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    ExternalMidiEvent,
    ParameterInfo,
    TimeSignature,
    TransportState,
)

# Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
# binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
# binary lays out engine structs differently than this wrapper expects.
EXPECTED_ENGINE_ABI_VERSION = 3


class RealtimeEngine(_EngineMidiMixin, _EngineMixingMixin, _EngineIoMixin):
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
        self._capture_ptrs: ctypes.Array[Any] | None = None
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

    def settle_parameters(self) -> None:
        """Snap every in-flight parameter ramp to its target value.

        Covers engine-level smoothed parameters and the mixer's lane
        fader/pan/gate and bus gain smoothers. Offline renders call this after
        a priming process() block so the first audible block renders at the
        settled values instead of ramping in from defaults.
        """
        _check(_get_lib().sonare_engine_settle_parameters(self._require_handle()))

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

    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Install a tempo map from ramp segments.

        Each segment is a mapping (``start_ppq`` / ``bpm`` / optional ``end_bpm``)
        or a tuple ``(start_ppq, bpm, start_sample=ignored, end_bpm=0.0)``.
        ``end_bpm`` 0 means a constant-tempo segment; a positive value ramps to
        that tempo. Pass an empty sequence to clear the map.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTempoSegment * count)() if count else None
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                bpm = float(seg["bpm"])
                end_bpm = float(seg.get("end_bpm", 0.0))
            else:
                tup = tuple(seg)
                if len(tup) < 2:
                    raise ValueError(f"segments[{i}] must contain (start_ppq, bpm)")
                start_ppq = float(tup[0])
                bpm = float(tup[1])
                end_bpm = float(tup[3]) if len(tup) >= 4 else 0.0
            c_segments[i].start_ppq = start_ppq
            c_segments[i].bpm = bpm
            c_segments[i].end_bpm = end_bpm
        _check(
            _get_lib().sonare_engine_set_tempo_segments(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
            )
        )

    def set_time_signature_segments(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Install a time-signature map.

        Each segment is a mapping (``start_ppq`` / ``numerator`` / ``denominator``)
        or a tuple ``(start_ppq, numerator, denominator)``. Pass an empty sequence
        to clear the map.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTimeSignatureSegment * count)() if count else None
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                numerator = int(seg["numerator"])
                denominator = int(seg["denominator"])
            else:
                tup = tuple(seg)
                if len(tup) < 3:
                    raise ValueError(
                        f"segments[{i}] must contain (start_ppq, numerator, denominator)"
                    )
                start_ppq = float(tup[0])
                numerator = int(tup[1])
                denominator = int(tup[2])
            c_segments[i].start_ppq = start_ppq
            c_segments[i].numerator = numerator
            c_segments[i].denominator = denominator
        _check(
            _get_lib().sonare_engine_set_time_signature_segments(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
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

    def set_param_smoothing_ms(self, smoothing_ms: float) -> None:
        """Set the default ramp time (ms) for engine-level smoothed parameters.

        Applies to every smoothed parameter change -- fader/pan glides,
        insert-parameter automation, and MIDI-CC mappings. The default is 20 ms;
        pass ``0`` for instant (un-ramped) changes.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_param_smoothing_ms"):
            raise RuntimeError("libsonare was built without live-parameter support")
        _check(
            lib.sonare_engine_set_param_smoothing_ms(self._require_handle(), float(smoothing_ms))
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

    def set_midi_destination_external(self, destination_id: int, external: bool) -> None:
        """Mark a MIDI destination for external routing (or clear it).

        A destination marked external bypasses the internal instrument rack; its
        sequenced events are buffered in the engine's external-MIDI output queue
        for the host to drain with :meth:`drain_external_midi`. ``external`` False
        clears the mark.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_destination_external"):
            raise RuntimeError("libsonare was built without external-MIDI output support")
        _check(
            lib.sonare_engine_set_midi_destination_external(
                self._require_handle(), int(destination_id), 1 if external else 0
            )
        )

    def set_external_midi_clock_enabled(self, enabled: bool) -> None:
        """Enable/disable forwarding MIDI clock/transport bytes to the external queue.

        When enabled, MIDI clock (0xF8) and transport (start/continue/stop) bytes
        are enqueued tagged with destination id ``0xFFFFFFFF`` so external gear can
        be tempo-synced. Off by default.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_external_midi_clock_enabled"):
            raise RuntimeError("libsonare was built without external-MIDI output support")
        _check(
            lib.sonare_engine_set_external_midi_clock_enabled(
                self._require_handle(), 1 if enabled else 0
            )
        )

    def external_midi_dropped_count(self) -> int:
        """Number of external-MIDI events dropped because the queue was full.

        Advisory telemetry; monotonic within a prepared session.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_external_midi_dropped_count"):
            raise RuntimeError("libsonare was built without external-MIDI output support")
        out = ctypes.c_uint32()
        _check(
            lib.sonare_engine_external_midi_dropped_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def drain_external_midi(self, max_records: int = 1024) -> list[ExternalMidiEvent]:
        """Drain queued external-MIDI events, lowered to MIDI 1.0 byte messages.

        Each returned :class:`ExternalMidiEvent` is one MIDI 1.0 message (1..3
        bytes). ``max_records`` caps the number of output events returned — the
        shared unit across every surface. Events past the cap stay queued for the
        next call (lossless); call again to drain the rest. The native drain
        requires a capacity of at least 3 (the most one record can lower to), so
        a remaining budget below 3 stops the drain rather than over-fetch.
        """
        if max_records <= 0:
            return []
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_drain_external_midi"):
            raise RuntimeError("libsonare was built without external-MIDI output support")
        capacity = max(int(max_records), 3)
        raw = (SonareExternalMidiEvent * capacity)()
        written = ctypes.c_size_t()
        results: list[ExternalMidiEvent] = []
        while len(results) < max_records:
            remaining = max_records - len(results)
            if remaining < 3:
                break  # too small to lower one record without loss
            cap = min(capacity, remaining)
            _check(
                lib.sonare_engine_drain_external_midi(
                    self._require_handle(), raw, cap, ctypes.byref(written)
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
                        destination_id=int(event.destination_id),
                        render_frame=int(event.render_frame),
                        bytes=bytes(event.bytes[:byte_count]),
                    )
                )
        return results

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
            beat=int(raw.beat),
            beat_fraction=float(raw.beat_fraction),
        )


_rebind_facade_exports(globals(), "libsonare._engine_")
del _rebind_facade_exports
