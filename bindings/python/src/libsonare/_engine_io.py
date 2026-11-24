"""Realtime/offline DAW engine wrapper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any, Literal, overload

import numpy as np

from ._engine_conversions import (
    _POSITIVE_INT,
    _capture_source_name,
    _capture_source_value,
    _fixed_bytes,
    _given_positive,
    _graph_connection_to_c,
    _graph_node_to_c,
    _graph_parameter_binding_to_c,
    _meter_telemetry_from_c,
    _scope_telemetry_from_c,
    _telemetry_from_c,
)
from ._errors import _entry, _invalid_state, _refuse, _refuse_type
from ._ffi_types_core import (
    SONARE_METER_MAX_INSERTS,
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
    SonareEngineRenderOfflineOptions,
    SonareEngineTelemetry,
    SonareMeterTelemetryRecord,
    SonareScopeTelemetryRecord,
)
from ._field_spelling import surface_spelling
from ._narrowing import _MAX_PUBLIC_PPQ, _narrow_double, _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _INT64_MAX,
    _INT64_MIN,
    ClipPageRequest,
    EngineBounceOptions,
    EngineBounceResult,
    EngineCaptureStatus,
    EngineFreezeOptions,
    EngineFreezeResult,
    EngineGraphSpec,
    EngineTelemetry,
    MeterTelemetryRecord,
    ScopeTelemetryRecord,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _narrow_int,
    _planar_channel_arrays,
    _present_count,
    _require_instance,
    _resolve_render_frame,
    _resolve_sample_bound,
    _Respelling,
    _to_c_float,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint,
    _token_ordinal,
)
from ._types_engine import (
    _ENGINE_DITHERS,
    CapturePunch,
    EngineCaptureSource,
    EngineInputMonitor,
    EngineInputMonitorInput,
    EngineProcessWithMonitorResult,
)


def _plane_arrays(arrays: Sequence[Any], frame_count: int) -> list[np.ndarray]:
    """View each ctypes plane as a float32 ndarray that keeps the plane alive."""
    return [np.frombuffer(array, dtype=np.float32, count=frame_count) for array in arrays]


class _EngineIoMixin:
    _capture_arrays: list[ctypes.Array[ctypes.c_float]]
    _capture_ptrs: ctypes.Array[Any] | None
    _sample_rate: int

    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

        def sample_at_ppq(self, ppq: float) -> int: ...

    def _capture_shape(self) -> tuple[int, int] | None:
        if not self._capture_arrays:
            return None
        return len(self._capture_arrays), len(self._capture_arrays[0])

    def _install_capture_buffer(self, channel_count: int, frame_capacity: int) -> None:
        arrays = [(ctypes.c_float * frame_capacity)() for _ in range(channel_count)]
        ptr_type = ctypes.POINTER(ctypes.c_float) * len(arrays)
        ptrs = ptr_type(*[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in arrays])
        raw = SonareEngineCaptureBuffer()
        raw.channels = ctypes.cast(ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float)))
        raw.num_channels = channel_count
        raw.capacity_frames = frame_capacity
        _check(
            _get_lib().sonare_engine_set_capture_buffer(self._require_handle(), ctypes.byref(raw))
        )
        self._capture_arrays = arrays
        self._capture_ptrs = ptrs

    def _punch_bound(self, punch: CapturePunch, edge: str) -> tuple[int, _Respelling]:
        """One punch edge in samples, with the spelling the caller gave it."""
        ppq = getattr(punch, f"{edge}_ppq")
        sample = getattr(punch, f"{edge}_sample")
        sec = getattr(punch, f"{edge}_sec")
        names = (f"{edge}_ppq", f"{edge}_sample", f"{edge}_sec")
        if sum(value is not None for value in (ppq, sample, sec)) != 1:
            raise _refuse(f"punch needs exactly one of {', '.join(names)}")
        if ppq is not None:
            beats = _narrow_double(ppq, f"punch.{edge}_ppq")
            if not 0.0 <= beats <= _MAX_PUBLIC_PPQ:
                raise _refuse(f"punch.{edge}_ppq must be finite and in [0, 1e12]")
            # Beats map to samples through the tempo map, so no single scale applies.
            return self.sample_at_ppq(beats), _Respelling(f"punch.{edge}_ppq")
        bound = _resolve_sample_bound(
            sample, sec, self._sample_rate, f"punch.{edge}_sample", f"punch.{edge}_sec"
        )
        assert bound.value is not None
        given = f"punch.{edge}_sample"
        return bound.value, bound.respelled.get(given, _Respelling(given))

    @_entry("RealtimeEngine.configure_capture")
    def configure_capture(
        self,
        *,
        capacity_frames: int,
        num_channels: int = 2,
        source: EngineCaptureSource = "output",
        record_offset_samples: int = 0,
        punch: CapturePunch | None = None,
    ) -> None:
        """Configure capture as a whole; an omitted setting takes its default, not its last value.

        ``source`` is ``"output"`` or ``"input"``. A positive ``record_offset_samples`` delays
        capture relative to the punch window; input arriving within the offset after a loop wrap
        belongs to the end of the pass played before it, so every pass captures the whole loop
        range. The offset has effect only with a ``punch``; while an armed punch capture loops it
        may span at most one loop pass, and a setup that would break that is refused whole. An
        omitted ``punch`` leaves capture ungated. The dry input monitor is not part of capture;
        it is :meth:`set_input_monitor`'s. The buffer is reallocated only when ``num_channels`` or
        ``capacity_frames`` changes, and reallocating while armed raises ``INVALID_STATE``.
        """
        # Narrowed, not int()-coerced: int(1024.5) is a buffer one frame short.
        channel_count = _narrow_int(num_channels, "num_channels", 1, _C_INT_MAX)
        frame_capacity = _narrow_int(capacity_frames, "capacity_frames", 1, _INT64_MAX)
        source_value = _capture_source_value(source, "source")
        offset = _narrow_int(record_offset_samples, "record_offset_samples", _INT64_MIN, _INT64_MAX)
        bounds = None
        if punch is not None:
            punch = _require_instance(punch, CapturePunch, "punch")
            bounds = (self._punch_bound(punch, "start"), self._punch_bound(punch, "end"))

        if self._capture_shape() != (channel_count, frame_capacity):
            if self.capture_status().armed:
                raise _invalid_state("the capture buffer cannot be resized while armed")
            self._install_capture_buffer(channel_count, frame_capacity)
        handle = self._require_handle()
        lib = _get_lib()
        _check(lib.sonare_engine_set_capture_source(handle, _to_c_int(source_value, "source")))
        (start, start_given), (end, end_given) = (
            bounds
            if bounds is not None
            else ((0, _Respelling("start_sample")), (0, _Respelling("end_sample")))
        )
        # Offset and punch go together, so a looped setup is checked as the state it produces.
        _check(
            lib.sonare_engine_set_capture_timing(
                handle,
                _to_c_int64(offset, "record_offset_samples"),
                _to_c_int64(start, start_given.name),
                _to_c_int64(end, end_given.name),
                0 if bounds is None else 1,
            ),
            respelled={"punch_start_sample": start_given, "punch_end_sample": end_given},
        )

    @_entry("RealtimeEngine.set_input_monitor")
    def set_input_monitor(self, monitor: EngineInputMonitor | EngineInputMonitorInput) -> None:
        """Turn the dry input monitor on or off at ``monitor.gain``; it switches without a ramp.

        Capture source ``"input"`` records the input ahead of the monitor, so what is recorded
        does not depend on it. A non-finite gain raises ``SonareValueError``.
        """
        if not isinstance(monitor, EngineInputMonitor | EngineInputMonitorInput):
            raise _refuse_type(
                "monitor must be an EngineInputMonitor or an EngineInputMonitorInput"
            )
        given = monitor
        _check(
            _get_lib().sonare_engine_set_input_monitor(
                self._require_handle(),
                1 if given.enabled else 0,
                _to_c_float(given.gain, "monitor.gain"),
            ),
            respelled={"gain": _Respelling("monitor.gain")},
        )

    @_entry("RealtimeEngine.input_monitor")
    def input_monitor(self) -> EngineInputMonitor:
        """Return the dry input monitor, both fields from one write."""
        enabled = ctypes.c_int(0)
        gain = ctypes.c_float(0.0)
        _check(
            _get_lib().sonare_engine_input_monitor(
                self._require_handle(), ctypes.byref(enabled), ctypes.byref(gain)
            )
        )
        return EngineInputMonitor(enabled=enabled.value != 0, gain=float(gain.value))

    @_entry("RealtimeEngine.arm_capture")
    def arm_capture(self, armed: bool = True) -> None:
        _check(_get_lib().sonare_engine_arm_capture(self._require_handle(), 1 if armed else 0))

    @_entry("RealtimeEngine.reset_capture")
    def reset_capture(self) -> None:
        _check(_get_lib().sonare_engine_reset_capture(self._require_handle()))

    @_entry("RealtimeEngine.capture_status")
    def capture_status(self) -> EngineCaptureStatus:
        raw = SonareEngineCaptureStatus()
        _check(_get_lib().sonare_engine_capture_status(self._require_handle(), ctypes.byref(raw)))
        return EngineCaptureStatus(
            captured_frames=int(raw.captured_frames),
            dropped_frames=int(raw.dropped_frames),
            armed=bool(raw.armed),
            punch_enabled=bool(raw.punch_enabled),
            source=_capture_source_name(raw.source),
            record_offset_samples=int(raw.record_offset_samples),
        )

    @_entry("RealtimeEngine.captured_audio")
    def captured_audio(self) -> list[np.ndarray]:
        status = self.capture_status()
        capacity = len(self._capture_arrays[0]) if self._capture_arrays else 0
        frames = max(0, min(status.captured_frames, capacity))
        return [_from_c_float_array(array, frames) for array in self._capture_arrays]

    @_entry("RealtimeEngine.set_graph")
    def set_graph(self, spec: EngineGraphSpec) -> None:
        _require_instance(spec, EngineGraphSpec, "spec")
        nodes = (SonareEngineGraphNode * len(spec.nodes))(
            *[_graph_node_to_c(node, index) for index, node in enumerate(spec.nodes)]
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
        for field in ("input_node", "output_node"):
            if getattr(spec, field) == "":
                raise _refuse(f"{field} must not be empty")
        raw.input_node = _fixed_bytes(spec.input_node, 64, "input_node")
        raw.output_node = _fixed_bytes(spec.output_node, 64, "output_node")
        # Assigned unconverted so the struct's own narrowing sees the caller's
        # value; int() would truncate a fraction past it.
        raw.num_channels = spec.num_channels
        _check(_get_lib().sonare_engine_set_graph(self._require_handle(), ctypes.byref(raw)))

    @property
    def graph_node_count(self) -> int:
        out = ctypes.c_size_t()
        _check(_get_lib().sonare_engine_graph_node_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @property
    def graph_connection_count(self) -> int:
        out = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_graph_connection_count(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    @overload
    def process(
        self, channels: Sequence[Sequence[float]], *, with_monitor: Literal[False] = False
    ) -> list[np.ndarray]: ...

    @overload
    def process(
        self, channels: Sequence[Sequence[float]], *, with_monitor: Literal[True]
    ) -> EngineProcessWithMonitorResult: ...

    @_entry("RealtimeEngine.process")
    def process(
        self, channels: Sequence[Sequence[float]], *, with_monitor: bool = False
    ) -> list[np.ndarray] | EngineProcessWithMonitorResult:
        """Render one block and return the processed channel copies.

        Input channel buffers are never modified: the engine renders over
        private copies, adding its output to each corresponding input plane.
        Pass zero-filled planes when the engine is the only audio source.

        With ``with_monitor=True`` the cue (monitor) bus is kept out of the program
        planes and returned beside them as an
        :class:`EngineProcessWithMonitorResult`; otherwise it is folded into
        the returned planes.
        """
        arrays, ptrs, frame_count = self._channel_arrays(channels)
        monitor_arrays: list[ctypes.Array[ctypes.c_float]] = []
        monitor_ptrs = None
        if with_monitor:
            # `(c_float * N)()` zero-initialises each plane.
            monitor_arrays = [(ctypes.c_float * frame_count)() for _ in arrays]
            monitor_ptrs = (ctypes.POINTER(ctypes.c_float) * len(monitor_arrays))(
                *[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in monitor_arrays]
            )
        _check_realtime(
            _get_lib().sonare_engine_process(
                self._require_handle(),
                ptrs,
                monitor_ptrs,
                _to_c_int(len(arrays), surface_spelling("engine_process", "num_channels")),
                _to_c_int(frame_count, "channels frame count"),
            ),
            reason="engineDeviceDriven",
        )
        if not with_monitor:
            return _plane_arrays(arrays, frame_count)
        return EngineProcessWithMonitorResult(
            output=tuple(_plane_arrays(arrays, frame_count)),
            monitor=tuple(_plane_arrays(monitor_arrays, frame_count)),
        )

    @_entry("RealtimeEngine.render_offline")
    def render_offline(
        self,
        channels: Sequence[Sequence[float]],
        *,
        block_size: int | None = None,
        finalize: bool = True,
    ) -> list[np.ndarray]:
        """Render one offline span from the current transport position.

        An omitted ``block_size`` is the default offline block, capped at the
        prepared maximum; an explicit 0 is refused. ``finalize`` says whether
        this call ends the timeline. True (the
        default, and what a one-shot bounce wants) releases every sounding note
        and flushes the PDC / alignment delay lines before returning. False
        renders one CHUNK of a longer timeline: a note held across the chunk
        boundary keeps sounding into the next call and the delay lines carry
        their history over, so consecutive chunks concatenate to exactly what
        one continuous render of the same span produces. Call
        ``finish_offline_render`` once after the last chunk.

        Sample-exact concatenation requires every chunk to use the same
        ``block_size`` and a frame count that is a whole number of blocks: each
        call restarts the block grid at its own frame 0 and renders a short
        final block for the remainder, and the clip / automation / MIDI-clip
        snapshots are frozen once per block, so a chunk that ends mid-block
        shifts every later block boundary. Audio stays continuous either way;
        only bit-identity is lost.
        """
        arrays, ptrs, frame_count = self._channel_arrays(channels)
        options = SonareEngineRenderOfflineOptions._given(finalize=1 if finalize else 0)
        _check(
            _get_lib().sonare_engine_render_offline(
                self._require_handle(),
                ptrs,
                _to_c_int(len(arrays), surface_spelling("engine_render_offline", "num_channels")),
                _to_c_int64(frame_count, "frame_count"),
                _to_c_int(_resolve_zero_default(block_size, "block_size"), "block_size"),
                ctypes.byref(options),
            ),
            flattened="options",
        )
        return _plane_arrays(arrays, frame_count)

    @_entry("RealtimeEngine.finish_offline_render")
    def finish_offline_render(self) -> None:
        """End a chunked offline render.

        Releases every note the sequencer still holds and flushes the PDC /
        alignment delay lines. Required after ``render_offline(...,
        finalize=False)``; the finalizing form does it itself.

        Skipping it leaves every note still sounding at the last chunk held. On
        an engine-internal instrument the tail simply never releases; on a
        destination marked external (``set_midi_destination_external``) the
        note-ons already left through the external MIDI queue, so the note-offs
        emitted here are the only ones the receiving device will get and the
        notes otherwise hang outside the engine.
        """
        _check(_get_lib().sonare_engine_finish_offline_render(self._require_handle()))

    @_entry("RealtimeEngine.bounce_offline")
    def bounce_offline(self, options: EngineBounceOptions) -> EngineBounceResult:
        """Render the whole span in one call and return the interleaved mix.

        The whole result is held in memory, so the span is capped by a 1 GiB
        peak budget over the full-size float buffers a bounce holds at once, and
        exceeding it raises :class:`SonareValueError`.

        The cap is a *frame* count: inversely proportional to ``num_channels``,
        and independent of the sample rate. Stereo refuses past 44,739,242
        frames, or past 33,554,432 frames when ``dither`` is given, which
        adds one more copy; 5.1 refuses past a third of each. Read as a
        *duration* those two stereo counts are 15 min 32 s and 11 min 39 s at
        48 kHz, and half that at 96 kHz. When ``source_sample_rate`` and
        ``target_sample_rate`` differ both the source and the resampled length
        are checked, so the longer of the two is what binds.

        ``source_sample_rate`` defaults to ``None``, the engine's prepared rate,
        and a given value must equal it or the call raises :class:`SonareValueError`.
        ``target_sample_rate`` defaults to ``None``, the source rate (no
        resampling); the result's ``sample_rate`` is the rate of the output.
        An explicit 0 is refused for either.

        Render longer material in chunks through
        ``render_offline(..., finalize=False)``.
        """
        _require_instance(options, EngineBounceOptions, "options")
        lib = _get_lib()
        # Seeded by sonare_engine_bounce_options_init; a field left None keeps the init's value
        # (BLOCK_SIZE_AUTO, SAMPLE_RATE_PREPARED, NaN for -14 LUFS, the built-in dither seed),
        # and a given value is written unconverted so the struct's own narrowing sees it.
        raw_options = SonareEngineBounceOptions._given(
            total_frames=options.total_frames,
            block_size=_given_positive(options.block_size, "options.block_size", _POSITIVE_INT),
            num_channels=options.num_channels,
            target_sample_rate=_given_positive(
                options.target_sample_rate, "options.target_sample_rate", _POSITIVE_INT
            ),
            source_sample_rate=_given_positive(
                options.source_sample_rate, "options.source_sample_rate", _POSITIVE_INT
            ),
            normalize_lufs=1 if options.normalize_lufs else 0,
            target_lufs=None
            if options.target_lufs is None
            else _narrow_double(options.target_lufs, "options.target_lufs"),
            # The C ordinals put "none" at 0, ahead of the three shapes.
            dither=0
            if options.dither is None
            else _token_ordinal(options.dither, _ENGINE_DITHERS, "options.dither") + 1,
            dither_bits=options.dither_bits,
            dither_seed=options.dither_seed,
        )
        raw_result = SonareEngineBounceResult()
        _check(
            lib.sonare_engine_bounce_offline(
                self._require_handle(), ctypes.byref(raw_options), ctypes.byref(raw_result)
            )
        )
        # Converted whole before the free, which resets every field of the struct.
        try:
            return EngineBounceResult(
                interleaved=_from_c_float_array(
                    raw_result.interleaved, int(raw_result.sample_count)
                ),
                frames=int(raw_result.frames),
                num_channels=int(raw_result.num_channels),
                sample_rate=int(raw_result.sample_rate),
                integrated_lufs=float(raw_result.integrated_lufs),
                quantize_clipped_samples=_present_count(
                    raw_result.quantize_clipped_samples, raw_result.has_quantize_clipped_samples
                ),
            )
        finally:
            if raw_result.interleaved:
                lib.sonare_free_engine_bounce_result(ctypes.byref(raw_result))

    @_entry("RealtimeEngine.freeze_offline")
    def freeze_offline(self, options: EngineFreezeOptions) -> EngineFreezeResult:
        """Render the source layer over the span and install it as the only clip.

        Bakes every audio clip and hosted MIDI instrument through the lane strips,
        lane inserts and group buses. The master strip, monitor bus, engine graph,
        capture and metronome stay live and process the frozen clip once on
        playback. The frozen clip replaces the whole clip set and every MIDI clip
        is withdrawn in the same call, so nothing plays twice; live MIDI input
        still reaches the instruments.
        """
        _require_instance(options, EngineFreezeOptions, "options")
        # Seeded by sonare_engine_freeze_options_init, so an omitted block_size keeps
        # BLOCK_SIZE_AUTO; the rest is written unconverted so the struct's own narrowing sees it.
        raw_options = SonareEngineFreezeOptions._given(
            total_frames=options.total_frames,
            block_size=_given_positive(options.block_size, "options.block_size", _POSITIVE_INT),
            num_channels=options.num_channels,
            clip_id=options.clip_id,
            start_ppq=_narrow_double(options.start_ppq, "options.start_ppq"),
            gain=_narrow_double(options.gain, "options.gain"),
        )
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

    @_entry("RealtimeEngine.drain_telemetry")
    def drain_telemetry(self, max_records: int = 1024) -> list[EngineTelemetry]:
        if max_records <= 0:
            return []
        capacity = _to_c_size_t(max_records, "max_records")
        raw = (SonareEngineTelemetry * capacity.value)()
        written = ctypes.c_size_t()
        _check(
            _get_lib().sonare_engine_drain_telemetry(
                self._require_handle(), raw, capacity, ctypes.byref(written)
            )
        )
        return [_telemetry_from_c(raw[i]) for i in range(written.value)]

    @_entry("RealtimeEngine.reset_master_loudness_meter")
    def reset_master_loudness_meter(self, render_frame: int | None = None) -> None:
        """Queue a reset of the master integrated-loudness meter."""
        lib = _get_lib()
        _check(
            lib.sonare_engine_reset_master_loudness_meter(
                self._require_handle(),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.reset_processor_state")
    def reset_processor_state(self, render_frame: int | None = None) -> None:
        """Queue a reset of every lane, bus, master and graph processor state.

        At ``render_frame`` (``None``: the next block head) tails, delay lines
        and envelopes are dropped, automation is applied at the transport
        position and every smoother is snapped to its target. Instruments keep
        their state. Raises when the command queue is full.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_reset_processor_state(
                self._require_handle(),
                _to_c_int64(_resolve_render_frame(render_frame), "render_frame"),
            )
        )

    @_entry("RealtimeEngine.pop_clip_page_request")
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

    @_entry("RealtimeEngine.set_clip_page_prefetch_frames")
    def set_clip_page_prefetch_frames(self, frames: int) -> None:
        """Set the clip-page look-ahead window in timeline frames.

        The player reports the pages it is *about to* read that are not
        resident yet, so a streaming host can service them before the audio
        thread reaches them. Without look-ahead a page miss is only reported
        after the read already produced silence, which costs one block of
        silence at every page boundary the host has not primed.

        Look-ahead requests drain through the same
        :meth:`pop_clip_page_request` queue and are queued *after* the block's
        genuine misses, so a host that keeps only the newest request per clip
        tracks the look-ahead frontier.

        :meth:`prepare` defaults this to half a second at the engine's sample
        rate; ``0`` disables it. A clip whose pages are all resident produces
        no requests either way. Safe to call during playback.
        """
        lib = _get_lib()
        _check(
            lib.sonare_engine_set_clip_page_prefetch_frames(
                self._require_handle(), _to_c_int64(frames, "frames")
            )
        )

    @property
    def clip_page_prefetch_frames(self) -> int:
        """Return the clip-page look-ahead window in timeline frames."""
        lib = _get_lib()
        out = ctypes.c_int64()
        _check(
            lib.sonare_engine_clip_page_prefetch_frames(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    @_entry("RealtimeEngine.drain_meter_telemetry")
    def drain_meter_telemetry(self, max_records: int = 1024) -> list[MeterTelemetryRecord]:
        """Drain pending per-plane meter telemetry for every target.

        Each array has one element per target channel. A stereo target's left
        and right are planes 0 and 1. A mono target has no right measurement.
        """
        if max_records <= 0:
            return []
        lib = _get_lib()
        capacity = _to_c_size_t(max_records, "max_records")
        raw = (SonareMeterTelemetryRecord * capacity.value)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_engine_drain_meter_telemetry(
                self._require_handle(), raw, capacity, ctypes.byref(written)
            )
        )
        return [_meter_telemetry_from_c(raw[i]) for i in range(written.value)]

    @_entry("RealtimeEngine.meter_target_insert_gain_reduction")
    def meter_target_insert_gain_reduction(self, target_id: int) -> np.ndarray:
        """Return per-insert gain reduction in dB (<= 0) for a meter target's strip.

        Entries follow the combined insert order (pre-fader, then post-fader)
        of the last rendered block. ``target_id`` uses the meter-record
        encoding: 0 master, 1..32 lanes, 33..40 buses; the input monitor
        (0xFFFF) and unused slots give an empty array.
        """
        lib = _get_lib()
        raw = (ctypes.c_float * SONARE_METER_MAX_INSERTS)()
        count = ctypes.c_size_t()
        _check(
            lib.sonare_engine_meter_target_insert_gain_reduction(
                self._require_handle(),
                _to_c_uint(target_id, "target_id"),
                raw,
                _to_c_size_t(SONARE_METER_MAX_INSERTS, "capacity"),
                ctypes.byref(count),
            )
        )
        return _from_c_float_array(raw, min(count.value, SONARE_METER_MAX_INSERTS))

    @_entry("RealtimeEngine.insert_parameter_constructed_value")
    def insert_parameter_constructed_value(self, param_id: int) -> float:
        """Return an insert parameter's immutable construction value."""
        lib = _get_lib()
        out_value = ctypes.c_float()
        _check(
            lib.sonare_engine_insert_parameter_constructed_value(
                self._require_handle(), _to_c_uint(param_id, "param_id"), ctypes.byref(out_value)
            )
        )
        return float(out_value.value)

    @_entry("RealtimeEngine.configure_scope_telemetry")
    def configure_scope_telemetry(self, interval_frames: int, band_count: int) -> int:
        """Enable scope telemetry publishing and return the applied band count.

        ``interval_frames`` is the minimum render-frame spacing between snapshots;
        ``band_count`` is the requested number of spectrum bands (clamped by the
        engine). Returns the band count the engine actually applied.
        """
        lib = _get_lib()
        applied = ctypes.c_size_t()
        _check(
            lib.sonare_engine_configure_scope_telemetry(
                self._require_handle(),
                _to_c_int(interval_frames, "interval_frames"),
                _to_c_size_t(band_count, "band_count"),
                ctypes.byref(applied),
            )
        )
        return int(applied.value)

    @_entry("RealtimeEngine.drain_scope_telemetry")
    def drain_scope_telemetry(self, max_records: int = 1024) -> list[ScopeTelemetryRecord]:
        """Drain pending scope telemetry records published by the engine."""
        if max_records <= 0:
            return []
        lib = _get_lib()
        capacity = _to_c_size_t(max_records, "max_records")
        raw = (SonareScopeTelemetryRecord * capacity.value)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_engine_drain_scope_telemetry(
                self._require_handle(), raw, capacity, ctypes.byref(written)
            )
        )
        return [_scope_telemetry_from_c(raw[i]) for i in range(written.value)]

    @staticmethod
    def _channel_arrays(
        channels: Sequence[Sequence[float]],
    ) -> tuple[list[ctypes.Array[ctypes.c_float]], ctypes.Array[Any], int]:
        return _planar_channel_arrays(channels)
