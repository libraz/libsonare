from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from typing import Literal, overload

import numpy as np

from ._engine_pages import ClipPageProvider as ClipPageProvider
from ._engine_pages import FileClipPageProvider as FileClipPageProvider
from ._project import (
    Articulation,
    BuiltinSynthConfig,
    ControllerAxis,
    ControllerInput,
    MidiCcBinding,
    MpeDimension,
    NoteTracking,
    PartRigMode,
    ProjectTimeline,
    SampleBank,
    Sf2InstrumentConfig,
    SynthPatch,
)
from ._types_engine import PlanarChannels as PlanarChannels
from .analyzer import StripRef
from .types import (
    AutomationPoint,
    CaptureInputMonitor,
    CapturePunch,
    ClipPageRequest,
    EngineBounceOptions,
    EngineBounceResult,
    EngineCaptureSource,
    EngineCaptureStatus,
    EngineClip,
    EngineFreezeOptions,
    EngineFreezeResult,
    EngineGraphSpec,
    EngineMarker,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    EngineProcessWithMonitorResult,
    EngineTelemetry,
    EngineTrackMonitorMode,
    EqBandInput,
    ExternalMidiEvent,
    MeterTelemetryRecord,
    MidiFxConfig,
    MixSceneBusInput,
    MixSceneStripInput,
    PanLaw,
    PanMode,
    ParameterInfo,
    ScopeTelemetryRecord,
    SidechainCheck,
    SidechainSource,
    TransportState,
)

MAX_TRACK_LANES: int

class RealtimeEngine:
    def __init__(
        self,
        sample_rate: float,
        max_block_size: int | None = None,
        *,
        command_capacity: int | None = None,
        telemetry_capacity: int | None = None,
        max_channels: int | None = None,
    ) -> None: ...
    def close(self) -> None:
        """Release the native engine; idempotent. Any later
        call raises SonareError (InvalidState)."""
        ...
    def __enter__(self) -> RealtimeEngine:
        """Return the engine for use as a context manager."""
        ...
    def __exit__(self, *_exc: object) -> None:
        """Close the engine on leaving the ``with`` block."""
        ...
    def prepare(
        self,
        sample_rate: float,
        max_block_size: int | None = None,
        command_capacity: int | None = None,
        telemetry_capacity: int | None = None,
        max_channels: int | None = None,
    ) -> None: ...
    def play(self, render_frame: int | None = None) -> None:
        """Start the transport; ``render_frame`` is the render-frame time to apply (None: next
        block head)."""
        ...
    def stop(self, render_frame: int | None = None) -> None: ...
    def seek_sample(self, timeline_sample: int, render_frame: int | None = None) -> None: ...
    def settle_parameters(self) -> None: ...
    def prime_offline_parameters(
        self, num_channels: int, block_size: int | None = None
    ) -> None: ...
    @property
    def max_block_size(self) -> int: ...
    @property
    def tail_samples(self) -> int: ...
    @property
    def graph_latency_samples(self) -> float: ...
    def flush_control_commands(self) -> None: ...
    def settle_insert_parameters(self) -> None: ...
    def apply_commands_due_now_preserving_future(self) -> None: ...
    def apply_project_timeline(self, timeline: ProjectTimeline) -> None: ...
    def set_strip_sidechain(
        self, ref: StripRef, insert_index: int, source: SidechainSource | None
    ) -> None:
        """Key a strip insert from a track lane or (master only) a bus; ``None`` removes the
        binding."""
        ...
    def can_set_strip_sidechain(
        self, ref: StripRef, insert_index: int, source: SidechainSource
    ) -> SidechainCheck:
        """Report whether :meth:`set_strip_sidechain` would accept the binding, without changing
        anything."""
        ...
    def set_bus_sidechain(
        self, bus_id: int, insert_index: int, source: SidechainSource | None
    ) -> None:
        """Key a bus-strip insert from a track lane or another bus; ``None`` removes the
        binding."""
        ...
    def can_set_bus_sidechain(
        self, bus_id: int, insert_index: int, source: SidechainSource
    ) -> SidechainCheck:
        """Report whether :meth:`set_bus_sidechain` would accept the binding, without changing
        anything."""
        ...
    def seek_ppq(self, ppq: float, render_frame: int | None = None) -> None: ...
    def reset_master_loudness_meter(self, render_frame: int | None = None) -> None:
        """Queue a reset of the master integrated-loudness meter at ``render_frame`` (None: next
        block head)."""
        ...
    def reset_processor_state(self, render_frame: int | None = None) -> None:
        """Queue a reset of every lane, bus, master and graph processor state at ``render_frame``
        (None: next block head); raises SonareError when the command queue is full."""
        ...
    def set_tempo(self, bpm: float) -> None: ...
    def set_time_signature(self, numerator: int, denominator: int) -> None:
        """Set the time signature, e.g. 3/4 as ``numerator`` 3, ``denominator`` 4."""
        ...
    def set_tempo_segments(self, segments: Sequence[Mapping[str, float]]) -> None: ...
    def set_time_signature_segments(self, segments: Sequence[Mapping[str, float]]) -> None: ...
    def sample_at_ppq(self, ppq: float) -> int: ...
    def set_loop(self, start_ppq: float, end_ppq: float, enabled: bool = True) -> None: ...
    def add_parameter(self, info: ParameterInfo) -> None:
        """Register an automatable parameter; a duplicate id is refused."""
        ...
    def clear_parameters(self) -> None: ...
    def parameters(self) -> list[ParameterInfo]: ...
    def parameter_info(self, id: int) -> ParameterInfo:
        """Return the registered parameter with the given ``id``."""
        ...
    def set_automation_lane(self, param_id: int, points: Sequence[AutomationPoint]) -> None: ...
    @property
    def automation_lane_count(self) -> int:
        """Number of automation lanes currently installed."""
        ...
    def set_markers(self, markers: Sequence[EngineMarker]) -> list[EngineMarker]:
        """Replace the whole marker set and return the stored markers in the order given.

        A marker whose ``id`` is ``None`` is allocated the lowest id above every id
        this engine has stored or assigned that the list does not carry.
        """
        ...
    def markers(self) -> list[EngineMarker]: ...
    def marker(self, id: int) -> EngineMarker:
        """Return the marker with the given ``id``."""
        ...
    def seek_marker(self, marker_id: int, render_frame: int | None = None) -> None:
        """Move the playhead to a marker at ``render_frame`` (None: next block head)."""
        ...
    def set_loop_from_markers(self, start_marker_id: int, end_marker_id: int) -> None:
        """Set the loop region from a start and an end marker id."""
        ...
    def set_metronome(self, config: EngineMetronomeConfig) -> None:
        """Configure the metronome."""
        ...
    def metronome(self) -> EngineMetronomeConfig:
        """Return the current metronome configuration."""
        ...
    def count_in_end_sample(self, start_sample: int, bars: int) -> int: ...
    def set_clips(self, clips: Sequence[EngineClip]) -> None:
        """Replace the whole audio clip set; a refused clip raises and leaves the published set
        unchanged."""
        ...
    def upsert_clip(self, clip: EngineClip) -> None: ...
    def remove_clip(self, clip_id: int) -> None: ...
    def set_track_lanes(self, lanes: Sequence[Mapping[str, object]]) -> None:
        """Declare the mixer track lanes in order; more than :data:`MAX_TRACK_LANES` lanes raises
        SonareValueError."""
        ...
    def set_track_sends(self, track_id: int, sends: Sequence[Mapping[str, object]]) -> None:
        """Replace one track lane's sends (``bus_id``, ``level_db``, ``enabled``, ``send_timing``);
        an empty sequence clears them."""
        ...
    def set_track_output_bus(self, track_id: int, bus_id: int | None) -> None:
        """Route a track lane to a bus; ``None`` returns it to the master mix and 0 is refused.
        Unknown track or undeclared bus raises SonareValueError."""
        ...
    def set_track_buses(self, buses: Sequence[Mapping[str, object]]) -> None:
        """Declare the group buses from their mappings."""
        ...
    def set_bus_strip(self, bus_id: int, settings: MixSceneBusInput) -> None:
        """Replace a bus's strip with ``settings``; the bus comes from :meth:`set_track_buses`."""
        ...
    def set_strip(self, ref: StripRef, settings: MixSceneStripInput) -> None:
        """Replace a strip with ``settings``: a track id or ``"master"``."""
        ...
    def set_strip_eq_band(self, ref: StripRef, band_index: int, band: EqBandInput) -> None:
        """Replace one equalizer band of a strip in realtime; a track id or ``"master"``."""
        ...
    def set_bus_eq_band(self, bus_id: int, band_index: int, band: EqBandInput) -> None:
        """Replace one equalizer band of a bus strip in realtime."""
        ...
    def set_strip_insert_bypassed(
        self, ref: StripRef, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        """Bypass or engage one strip insert; ``reset_on_bypass`` clears its state when
        bypassed."""
        ...
    def set_bus_insert_bypassed(
        self, bus_id: int, insert_index: int, bypassed: bool, reset_on_bypass: bool = False
    ) -> None:
        """Bus counterpart of :meth:`set_strip_insert_bypassed`."""
        ...
    def set_strip_insert_param(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> None:
        """Realtime change of one strip insert parameter, addressed by name."""
        ...
    def set_bus_insert_param(self, bus_id: int, insert_index: int, name: str, value: float) -> None:
        """Bus counterpart of :meth:`set_strip_insert_param`."""
        ...
    def apply_strip_insert_param_now(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> bool:
        """Apply a strip insert parameter immediately on the calling thread; returns False for an
        unknown strip, insert or name."""
        ...
    def apply_bus_insert_param_now(
        self, bus_id: int, insert_index: int, name: str, value: float
    ) -> bool:
        """Bus counterpart of :meth:`apply_strip_insert_param_now`."""
        ...
    def restore_strip_insert_param(
        self, ref: StripRef, insert_index: int, name: str, value: float
    ) -> None:
        """Restore a retained strip insert value exactly, without a ramp; raises if the strip,
        insert or name is unknown."""
        ...
    def restore_bus_insert_param(
        self, bus_id: int, insert_index: int, name: str, value: float
    ) -> None:
        """Bus counterpart of :meth:`restore_strip_insert_param`."""
        ...
    def clear_strip_insert_parameter_bases(self, ref: StripRef) -> None:
        """Forget the remembered manual insert-parameter values of a strip and discard its queued
        insert edits."""
        ...
    def clear_bus_insert_parameter_bases(self, bus_id: int) -> None:
        """Forget the remembered manual insert-parameter values of a bus strip."""
        ...
    def set_strip_pan(self, ref: StripRef, pan: float) -> None:
        """Set a track strip's pan position (-1.0 hard left .. 1.0 hard right); the master strip
        is refused."""
        ...
    def set_strip_pan_law(self, ref: StripRef, pan_law: PanLaw) -> None:
        """Set a track strip's pan law (a :data:`PanLaw` token); the master strip is refused."""
        ...
    def set_strip_pan_mode(self, ref: StripRef, pan_mode: PanMode) -> None:
        """Set a track strip's pan mode (a :data:`PanMode` token); the master strip is refused."""
        ...
    def set_strip_dual_pan(self, ref: StripRef, left_pan: float, right_pan: float) -> None:
        """Set a track strip's dual-pan positions; the master strip is refused."""
        ...
    def set_strip_surround_pan(
        self,
        ref: StripRef,
        *,
        azimuth: float = ...,
        divergence: float = ...,
        lfe: float = ...,
    ) -> None:
        """Set a track strip's surround pan position; a non-finite or out-of-range value raises
        SonareValueError."""
        ...
    def set_strip_channel_delay_samples(self, ref: StripRef, delay_samples: int) -> None:
        """Set a track strip's alignment delay in samples; the master strip is refused."""
        ...
    def set_bus_pan(self, bus_id: int, pan: float) -> None:
        """Set a bus strip's pan position (-1.0 hard left .. 1.0 hard right)."""
        ...
    def set_bus_pan_law(self, bus_id: int, pan_law: PanLaw) -> None:
        """Set a bus strip's pan law (a :data:`PanLaw` token)."""
        ...
    def set_bus_pan_mode(self, bus_id: int, pan_mode: PanMode) -> None:
        """Set a bus strip's pan mode (a :data:`PanMode` token)."""
        ...
    def set_bus_dual_pan(self, bus_id: int, left_pan: float, right_pan: float) -> None:
        """Set a bus strip's dual-pan positions for the left and right channels."""
        ...
    def set_strip_fader_db(
        self, ref: StripRef, fader_db: float, render_frame: int | None = None
    ) -> None:
        """Set a strip's fader in dB in realtime through its reserved automation id."""
        ...
    def set_bus_gain_db(self, bus_id: int, gain_db: float, render_frame: int | None = None) -> None:
        """Set a bus's gain in dB in realtime through its reserved automation id."""
        ...
    def resolve_strip_insert_automation_id(
        self, ref: StripRef, insert_index: int, param_name: str
    ) -> int:
        """Resolve a strip insert parameter to its automation id; an unknown strip, insert or
        name raises SonareValueError."""
        ...
    def resolve_bus_insert_automation_id(
        self, bus_id: int, insert_index: int, param_name: str
    ) -> int:
        """Resolve a bus-strip insert parameter to its automation id; an unknown bus, insert or
        name raises SonareValueError."""
        ...
    def resolve_strip_automation_id(
        self, ref: StripRef, kind: Literal["faderDb", "pan", "width"]
    ) -> int:
        """Resolve a strip's ``"faderDb"``, ``"pan"`` or, for the master only, ``"width"`` to its
        automation id."""
        ...
    def resolve_bus_automation_id(self, bus_id: int, param_name: str) -> int:
        """Resolve a bus's ``"faderDb"`` to its automation id; an unknown bus or name raises
        SonareValueError."""
        ...
    def insert_parameter_constructed_value(self, param_id: int) -> float:
        """Return an insert parameter's immutable construction value."""
        ...
    @property
    def clip_count(self) -> int:
        """Number of audio clips currently installed."""
        ...
    def configure_capture(
        self,
        *,
        capacity_frames: int,
        num_channels: int = 2,
        source: EngineCaptureSource = "output",
        record_offset_samples: int = 0,
        input_monitor: CaptureInputMonitor | None = None,
        punch: CapturePunch | None = None,
    ) -> None:
        """Configure capture as a whole; an omitted setting takes its default."""
        ...
    def arm_capture(self, armed: bool = True) -> None:
        """Arm (or disarm) recording into the capture buffer."""
        ...
    def reset_capture(self) -> None:
        """Reset the capture state."""
        ...
    def capture_status(self) -> EngineCaptureStatus:
        """Return the current capture state."""
        ...
    def captured_audio(self) -> list[np.ndarray]:
        """Return the captured audio, one array per channel."""
        ...
    def set_graph(self, spec: EngineGraphSpec) -> None:
        """Install the engine processing graph from ``spec``."""
        ...
    @property
    def graph_node_count(self) -> int:
        """Number of nodes in the installed graph."""
        ...
    @property
    def graph_connection_count(self) -> int:
        """Number of connections in the installed graph."""
        ...
    @overload
    def process(
        self, channels: PlanarChannels, *, monitor: Literal[False] = False
    ) -> list[np.ndarray]:
        """Render one block and return the processed channel copies; inputs are not modified."""
        ...
    @overload
    def process(
        self, channels: PlanarChannels, *, monitor: Literal[True]
    ) -> EngineProcessWithMonitorResult:
        """Render one block and return the program planes and the monitor planes."""
        ...
    def render_offline(
        self,
        channels: PlanarChannels,
        *,
        block_size: int | None = None,
        finalize: bool = True,
    ) -> list[np.ndarray]:
        """Render one offline span from the current transport position; ``block_size`` None is the
        default offline block, 0 is refused; ``finalize`` False leaves the render open for
        :meth:`finish_offline_render`."""
        ...
    def finish_offline_render(self) -> None:
        """End a chunked offline render, releasing held notes and flushing delay lines; required
        after ``render_offline(..., finalize=False)``."""
        ...
    def bounce_offline(self, options: EngineBounceOptions) -> EngineBounceResult:
        """Render the whole span in one call and return the interleaved mix; a span over the 1 GiB
        memory budget raises SonareValueError."""
        ...
    def freeze_offline(self, options: EngineFreezeOptions) -> EngineFreezeResult:
        """Render the source layer over the span and install it as the only clip."""
        ...
    def drain_telemetry(self, max_records: int = 1024) -> list[EngineTelemetry]:
        """Drain up to ``max_records`` engine telemetry records; the rest stay queued."""
        ...
    def pop_clip_page_request(self) -> ClipPageRequest | None:
        """Pop the next clip-page request the host should service, or None when none is pending."""
        ...
    def set_clip_page_prefetch_frames(self, frames: int) -> None:
        """Set the clip-page look-ahead window in timeline frames."""
        ...
    @property
    def clip_page_prefetch_frames(self) -> int:
        """Clip-page look-ahead window in timeline frames."""
        ...
    def drain_meter_telemetry(self, max_records: int = 1024) -> list[MeterTelemetryRecord]:
        """Drain per-plane meter records; a stereo target's L/R are planes 0 and 1."""
        ...
    def meter_target_insert_gain_reduction(self, target_id: int) -> np.ndarray:
        """Return per-insert gain reduction in dB (<= 0) for a meter target's strip.

        Entries follow the combined insert order (pre-fader, then post-fader)
        of the last rendered block. ``target_id`` uses the meter-record
        encoding: 0 master, 1..32 lanes, 33..40 buses; the input monitor
        (0xFFFF) and unused slots give an empty list.
        """
        ...
    def configure_scope_telemetry(self, interval_frames: int, band_count: int) -> int:
        """Enable scope telemetry every ``interval_frames`` render frames with ``band_count``
        spectrum bands; returns the band count the engine applied."""
        ...
    def drain_scope_telemetry(self, max_records: int = 1024) -> list[ScopeTelemetryRecord]:
        """Drain up to ``max_records`` scope telemetry records."""
        ...
    def set_parameter(
        self,
        param_id: int,
        value: float,
        render_frame: int | None = None,
        *,
        smoothed: bool = False,
    ) -> None: ...
    def set_param_smoothing_ms(self, smoothing_ms: float) -> None: ...
    def set_track_solo_mute(
        self, track_id: int, *, solo: bool, mute: bool, render_frame: int | None = None
    ) -> None: ...
    def set_track_monitor_mode(
        self,
        track_id: int,
        mode: EngineTrackMonitorMode,
        render_frame: int | None = None,
    ) -> None: ...
    def set_midi_clips(self, clips: Sequence[EngineMidiClipSchedule]) -> None: ...
    def set_midi_destination_external(self, destination_id: int, external: bool) -> None: ...
    def set_external_midi_clock_enabled(self, enabled: bool) -> None: ...
    @property
    def clip_page_request_overflow_count(self) -> int: ...
    @property
    def warp_stretch_overflow_count(self) -> int: ...
    def set_warp_voice_capacity(self, voices: int) -> None: ...
    def warp_voice_capacity(self) -> int: ...
    @property
    def external_midi_dropped_count(self) -> int: ...
    def drain_external_midi(self, max_records: int = 1024) -> list[ExternalMidiEvent]: ...
    def push_midi_cc(
        self,
        destination_id: int,
        group: int,
        channel: int,
        controller: int,
        value: int,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live control change; ``controller`` and ``value`` are 0..127, ``channel`` and
        ``group`` 0..15, ``render_frame`` None for immediate."""
        ...
    def push_midi_sysex(
        self,
        destination_id: int,
        data: bytes | bytearray | memoryview,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live SysEx frame (1..512 bytes including 0xF0 and 0xF7); an oversize payload
        raises SonareValueError."""
        ...
    def push_midi_ump(
        self,
        destination_id: int,
        words: Iterable[int],
        render_frame: int | None = None,
    ) -> None:
        """Queue a live raw UMP message of 1..4 words, most significant first; data messages (MT
        0x3 / 0x5) are refused."""
        ...
    def push_midi_program(
        self,
        destination_id: int,
        group: int,
        channel: int,
        program: int,
        bank_valid: bool = False,
        bank_msb: int = 0,
        bank_lsb: int = 0,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live program change, with a bank when ``bank_valid``; ``program`` and bank bytes
        are 0..127, ``channel`` and ``group`` 0..15."""
        ...
    def push_midi_panic(self, render_frame: int | None = None) -> None:
        """Queue a MIDI panic (all-notes-off) at ``render_frame`` (None: immediate)."""
        ...
    def set_builtin_instrument(
        self, config: BuiltinSynthConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind the built-in polyphonic synth to ``destination_id``; ``config`` None installs the
        default sine patch."""
        ...
    def set_synth_instrument(
        self,
        patch: SynthPatch | str | None = None,
        destination_id: int = 0,
        *,
        sample_bank: SampleBank | None = None,
    ) -> None:
        """Bind the patch-driven NativeSynth to ``destination_id``; ``patch`` is a SynthPatch, a
        preset name, or None for the default subtractive patch."""
        ...
    def resolve_instrument_automation_id(self, destination_id: int, param_name: str) -> int:
        """Resolve a hosted instrument's continuous parameter to its automation id."""
        ...
    def load_sound_font(self, data: bytes | bytearray | memoryview) -> None:
        """Parse SoundFont 2 bytes into the engine, replacing any previous one; malformed input
        raises SonareError."""
        ...
    def set_sf2_instrument(
        self, config: Sf2InstrumentConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind a SoundFont player to ``destination_id``; ``config`` None installs the defaults."""
        ...
    def clear_midi_instrument(self, destination_id: int = 0) -> None:
        """Clear any realtime instrument bound to ``destination_id``."""
        ...
    @property
    def midi_instrument_count(self) -> int:
        """Number of bound realtime MIDI instruments."""
        ...
    def set_controller_profile(self, destination_id: int, preset_name: str) -> None:
        """Replace the instrument's controller profile with a named preset (see
        ``controller_profile_names``)."""
        ...
    def bind_controller(
        self,
        destination_id: int,
        *,
        input: ControllerInput,
        index: int = 0,
        axis: ControllerAxis,
        lo: float = 0.0,
        hi: float = 1.0,
        curve: float = 1.0,
    ) -> None:
        """Add one controller binding mapping ``input`` (at ``index`` for control change) to
        ``axis`` over ``lo``..``hi`` with a ``curve`` exponent."""
        ...
    def clear_controller_bindings(self, destination_id: int) -> None:
        """Drop every binding of the instrument's controller profile."""
        ...
    def controller_binding_count(self, destination_id: int) -> int:
        """Number of bindings the instrument's controller profile holds."""
        ...
    def set_controller_velocity_meaningful(self, destination_id: int, meaningful: bool) -> None:
        """Say whether note-on velocity carries expression for this instrument."""
        ...
    def controller_velocity_meaningful(self, destination_id: int) -> bool:
        """Read back :meth:`set_controller_velocity_meaningful`."""
        ...
    def set_controller_note_tracking(
        self, destination_id: int, dimension: MpeDimension, tracking: NoteTracking
    ) -> None:
        """Say which note a channel-addressed value of one MPE dimension belongs to."""
        ...
    def controller_note_tracking(
        self, destination_id: int, dimension: MpeDimension
    ) -> NoteTracking:
        """Read back :meth:`set_controller_note_tracking` as its canonical name."""
        ...
    def set_articulation(
        self, destination_id: int, channel: int, articulation: Articulation
    ) -> None:
        """Set what a channel does with an overlapping note-on (poly, mono retrigger or mono
        legato)."""
        ...
    def set_part_rig(
        self,
        destination_id: int,
        part: int,
        *,
        mode: PartRigMode,
        inserts: Sequence[Mapping[str, object]] | None = ...,
    ) -> None:
        """Set one part's rig on the instrument bound to ``destination_id``; an unbound destination
        or out-of-range part raises SonareValueError."""
        ...
    def articulation(self, destination_id: int, channel: int) -> Articulation:
        """Read back :meth:`set_articulation` as its canonical name."""
        ...
    def legato_fallback_count(self, destination_id: int) -> int:
        """Number of times a legato continuation was asked for and refused."""
        ...
    def bind_midi_cc(self, binding: MidiCcBinding) -> None:
        """Bind a 7/14-bit CC, RPN or NRPN descriptor to a live engine parameter."""
        ...
    def clear_midi_cc_bindings(self) -> None:
        """Clear all live MIDI CC to parameter bindings."""
        ...
    @property
    def midi_cc_binding_count(self) -> int:
        """Number of live MIDI CC bindings."""
        ...
    def set_midi_fx(self, destination_id: int, config: MidiFxConfig) -> None:
        """Install or replace a live MIDI-FX insert for ``destination_id`` from a MidiFxConfig."""
        ...
    def clear_midi_fx(self, destination_id: int = 0) -> None:
        """Clear the live MIDI-FX insert for ``destination_id``."""
        ...
    def set_midi_input_source(self, destination_id: int = 0) -> None:
        """Enable the engine-owned live MIDI input source for ``destination_id``."""
        ...
    def clear_midi_input_source(self) -> None:
        """Clear the engine-owned live MIDI input source."""
        ...
    @property
    def midi_input_pending_count(self) -> int:
        """Number of events queued in the live MIDI input source."""
        ...
    def push_midi_input_note_on(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a note-on into the live MIDI input source; ``offset_in_block_samples`` is samples
        into the next drained block (negative reads as 0)."""
        ...
    def push_midi_input_note_off(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a note-off into the live MIDI input source; ``offset_in_block_samples`` is samples
        into the next drained block (negative reads as 0)."""
        ...
    def push_midi_input_cc(
        self,
        group: int,
        channel: int,
        controller: int,
        value: int,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a control change into the live MIDI input source; ``offset_in_block_samples`` is
        samples into the next drained block (negative reads as 0)."""
        ...
    def push_midi_input_pitch_bend(
        self,
        group: int,
        channel: int,
        bend14: int,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a pitch bend (``bend14`` 0..16383, centre 8192) into the live MIDI input source;
        requires :meth:`set_midi_input_source`, else SonareError (InvalidState)."""
        ...
    def push_midi_input_channel_pressure(
        self,
        group: int,
        channel: int,
        pressure: int,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a channel pressure (0..127) into the live MIDI input source; requires
        :meth:`set_midi_input_source`, else SonareError (InvalidState)."""
        ...
    def push_midi_input_poly_pressure(
        self,
        group: int,
        channel: int,
        note: int,
        pressure: int,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a key pressure (``note`` and ``pressure`` 0..127) into the live MIDI input source;
        requires :meth:`set_midi_input_source`, else SonareError (InvalidState)."""
        ...
    def push_midi_input_program(
        self,
        group: int,
        channel: int,
        program: int,
        bank_valid: bool = False,
        bank_msb: int = 0,
        bank_lsb: int = 0,
        offset_in_block_samples: int = 0,
    ) -> None:
        """Queue a program change into the live MIDI input source; requires
        :meth:`set_midi_input_source`, else SonareError (InvalidState)."""
        ...
    def push_midi_input_ump(self, words: Iterable[int], offset_in_block_samples: int = 0) -> None:
        """Queue a raw UMP message into the live MIDI input source; requires
        :meth:`set_midi_input_source`, else SonareError (InvalidState)."""
        ...
    def push_midi_note_on(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live note-on to a MIDI destination; ``render_frame`` None for immediate."""
        ...
    def push_midi_note_off(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live note-off to a MIDI destination; ``render_frame`` None for immediate."""
        ...
    def push_midi_pitch_bend(
        self,
        destination_id: int,
        group: int,
        channel: int,
        bend14: int,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live pitch bend (``bend14`` 0..16383, centre 8192; larger is refused) to a MIDI
        destination."""
        ...
    def push_midi_channel_pressure(
        self,
        destination_id: int,
        group: int,
        channel: int,
        pressure: int,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live channel pressure (0..127) to a MIDI destination."""
        ...
    def push_midi_poly_pressure(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        pressure: int,
        render_frame: int | None = None,
    ) -> None:
        """Queue a live key pressure (``note`` and ``pressure`` 0..127) to a MIDI destination."""
        ...
    def transport_state(self) -> TransportState: ...
