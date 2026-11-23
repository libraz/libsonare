"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import KW_ONLY, dataclass, field
from typing import TYPE_CHECKING, Any, Literal, TypeAlias, get_args

import numpy as np
import numpy.typing as npt

from ._types_analysis import (
    TimeSignature,
    _empty_float32,
)
from ._types_enums import (
    AutomationCurve,
    EngineTelemetryError,
    EngineTelemetryType,
    ProjectWarpMode,
    SectionType,
    SidechainSourceKind,
)

if TYPE_CHECKING:
    from ._project_model import ProjectLoopMode, ProjectSourceKind, ProjectTrackKind

PlanarChannels: TypeAlias = Sequence[Sequence[float] | np.ndarray] | np.ndarray
"""Planar audio: one buffer per channel, or a 2-D ``(channels, frames)`` array."""


@dataclass(frozen=True, slots=True, eq=False)
class EngineProcessWithMonitorResult:
    """Result of ``RealtimeEngine.process(..., with_monitor=True)``: program and monitor planes."""

    output: tuple[npt.NDArray[np.float32], ...]
    """Processed program channel planes, one float32 array per channel."""
    monitor: tuple[npt.NDArray[np.float32], ...]
    """Monitor-bus channel planes, one float32 array per channel."""


@dataclass(frozen=True, slots=True)
class MixMeterSnapshot:
    """Realtime mixer meter snapshot for one strip.

    ``true_peak_dbtp_l``/``true_peak_dbtp_r``/``max_true_peak_dbtp`` are inter-sample
    peaks from the ITU-R BS.1770-4 polyphase reconstruction at 4x. They are a
    streaming measurement that runs one reconstruction-filter group delay (a
    few samples) behind the block, so the reading does not depend on the block
    size. :func:`metering_true_peak_dbtp` measures a whole signal at once.
    """

    peak_db_l: float | None
    """Left sample peak, in dBFS; -inf for silence, None when not measured."""
    peak_db_r: float | None
    """Right sample peak, in dBFS; -inf for silence, None when not measured."""
    rms_db_l: float | None
    """Left RMS level, in dBFS; -inf for silence, None when not measured."""
    rms_db_r: float | None
    """Right RMS level, in dBFS; -inf for silence, None when not measured."""
    correlation: float | None
    """Stereo phase correlation, -1..1; None when not measured."""
    mono_compat_width: float | None
    """Stereo width measure for mono compatibility; None when not measured, inf when the mid
    signal is silent but the side is not."""
    mono_compat_peak: float
    """Peak of the mono fold-down 0.5 * (L + R), as a linear full-scale amplitude."""
    mono_compat_side_rms: float
    """RMS of the side signal 0.5 * (L - R), as a linear full-scale amplitude."""
    likely_mono_compatible: bool
    """Whether the signal is likely to fold down to mono safely."""
    momentary_lufs: float | None
    """Momentary loudness, in LUFS; -inf for silence, None until the window fills."""
    short_term_lufs: float | None
    """Short-term loudness, in LUFS; -inf for silence, None until the window fills."""
    integrated_lufs: float | None
    """Integrated loudness, in LUFS; None before the first gating block, -inf while nothing
    passes the gate."""
    gain_reduction_db: float
    """Gain reduction applied, in dB."""
    true_peak_dbtp_l: float | None
    """Left true peak, in dBTP; -inf for silence, None when not measured."""
    true_peak_dbtp_r: float | None
    """Right true peak, in dBTP; -inf for silence, None when not measured."""
    max_true_peak_dbtp: float | None
    """Maximum true peak over both channels, in dBTP; -inf for silence, None when not
    measured."""
    seq: int
    """Sequence number of the snapshot."""
    # Per-plane surround meters (5.1/7.1). Each tuple holds num_channels values;
    # indices 0/1 mirror the *_l/*_r stereo fields above. None before any audio.
    num_channels: int | None = None
    """Number of valid planes in the per-plane tuples; None before any audio."""
    peak_db: tuple[float, ...] = ()
    """Sample peak per plane, in dBFS; -inf for silence, NaN in a plane not measured."""
    rms_db: tuple[float, ...] = ()
    """RMS level per plane, in dBFS; -inf for silence, NaN in a plane not measured."""
    true_peak_dbtp: tuple[float, ...] = ()
    """True peak per plane, in dBTP; -inf for silence, NaN in a plane not measured."""


@dataclass(frozen=True, slots=True)
class GoniometerPoint:
    """A single left/right sample pair for goniometer (vectorscope) display."""

    left: float
    """Left sample value."""
    right: float
    """Right sample value."""


@dataclass(frozen=True, slots=True, eq=False)
class MixResult:
    """Result of rendering a small stereo mixer scene."""

    left: npt.NDArray[np.float32]
    """Left channel samples."""
    right: npt.NDArray[np.float32]
    """Right channel samples."""
    sample_rate: int
    """Sample rate in Hz."""
    meters: list[MixMeterSnapshot]
    """Meter snapshots taken during the render."""


@dataclass(frozen=True, slots=True)
class EngineTelemetry:
    """Realtime engine telemetry event."""

    type: EngineTelemetryType
    """Event type (:data:`EngineTelemetryType`)."""
    error: EngineTelemetryError
    """Error code when :attr:`type` reports an error (:data:`EngineTelemetryError`)."""
    render_frame: int
    """Absolute render frame the event was produced at."""
    timeline_sample: int
    """Timeline position in samples."""
    audible_timeline_sample: int
    """Timeline position in samples of the audio now audible."""
    graph_latency_samples: float
    """Engine processing latency in samples (fractional)."""
    value: int
    """Event-specific integer payload."""


EngineTrackMonitorMode = Literal["off", "pfl", "afl"]
"""Per-track-lane monitor tap mode for the realtime engine."""
_TRACK_MONITOR_MODES: tuple[str, ...] = get_args(EngineTrackMonitorMode)


@dataclass(frozen=True, slots=True)
class ParameterInfo:
    """DAW parameter metadata for automation/introspection UIs."""

    id: int
    """Automation parameter id."""
    name: str
    """Display name of the parameter."""
    unit: str
    """Unit label of the parameter value."""
    min_value: float
    """Minimum value of the parameter."""
    max_value: float
    """Maximum value of the parameter."""
    default_value: float
    """Default value of the parameter."""
    rt_safe: bool
    """Whether the parameter can be changed on the realtime thread."""
    default_curve: AutomationCurve
    """Default curve shape between automation points (:data:`AutomationCurve`)."""


@dataclass(frozen=True, slots=True)
class AutomationPoint:
    """PPQ automation breakpoint."""

    ppq: float
    """Position of the breakpoint in PPQ (quarter notes)."""
    value: float
    """Parameter value at the breakpoint, in the parameter's own unit."""
    curve_to_next: AutomationCurve = "linear"
    """Curve shape of the segment from this point to the next (:data:`AutomationCurve`)."""


MarkerKind = Literal["marker", "text", "lyric", "cuePoint", "keySignature"]
"""Timeline marker kind, in SonareMarkerKind ordinal order."""
_MARKER_KINDS: tuple[str, ...] = get_args(MarkerKind)

SidechainRefusal = Literal[
    "invalidTarget",
    "insertOutOfRange",
    "undeclaredSource",
    "invalidSourceKind",
    "selfKey",
    "cycle",
    "tableFull",
    "planRefused",
]
"""Why a ``can_set_*_sidechain`` query refused a binding."""
_SIDECHAIN_REFUSALS: tuple[str, ...] = get_args(SidechainRefusal)

MidiFxConfig = dict[str, Any]
"""A MIDI-FX chain configuration document with camelCase keys.

Every key is optional; a stage runs only when one of its keys is given, and stages
apply in this order: ``transposeSemitones``; ``velocityScale`` / ``velocityOffset`` /
``velocityGamma``; ``quantizePpq`` / ``quantizeStrength``; ``chordIntervals``;
``arpeggiatorIntervals`` / ``arpeggiatorStepPpq`` / ``arpeggiatorGatePpq``;
``humanizePpq`` / ``humanizeVelocity`` / ``seed``. An earlier snake_case key or an
unknown key raises ``SonareError`` (InvalidFormat) naming it; a non-finite value raises
InvalidFormat too. JSON text is parsed by the caller (``json.loads``) before it is passed.
"""

MixSceneStripInput = dict[str, Any]
"""One mixer scene strip with camelCase keys, as :meth:`RealtimeEngine.set_strip` takes it.

``id`` is required; every other key is optional and takes the scene parser's default. An unknown
key or token raises ``SonareError`` (InvalidFormat) naming it.
"""

MixSceneDocument = dict[str, Any]
"""A whole mixer scene with camelCase keys (``strips``, ``buses``, ``vcaGroups`` and the rest).

:func:`mixing_scene_preset` returns one and :class:`Mixer` takes one as ``scene``; its JSON text
is what :meth:`Mixer.from_json` reads and :meth:`Mixer.to_json` writes.
"""

MixSceneBusInput = dict[str, Any]
"""One mixer scene bus with camelCase keys, as :meth:`RealtimeEngine.set_bus_strip` takes it.

``id`` and ``role`` are required; every other key is optional and takes the scene parser's default.
An unknown key or token raises ``SonareError`` (InvalidFormat) naming it.
"""


EngineCaptureSource = Literal["output", "input"]
"""What the realtime engine's recording sink captures."""
_CAPTURE_SOURCES: tuple[str, ...] = get_args(EngineCaptureSource)


@dataclass(frozen=True, slots=True)
class CaptureInputMonitor:
    """Input monitoring for :meth:`RealtimeEngine.configure_capture`."""

    enabled: bool
    """Whether the live input is monitored through the engine output."""
    gain: float = 1.0
    """Linear monitor gain."""


@dataclass(frozen=True, slots=True, kw_only=True)
class CapturePunch:
    """A half-open punch-in window ``[start, end)`` for :meth:`RealtimeEngine.configure_capture`.

    Each bound is given exactly once, in quarter notes (``*_ppq``, resolved through the
    engine tempo map), timeline samples (``*_sample``) or timeline seconds (``*_sec``,
    rounded to the nearest sample). A negative bound is refused.
    """

    start_ppq: float | None = None
    """Start in quarter notes."""
    start_sample: int | None = None
    """Start in timeline samples."""
    start_sec: float | None = None
    """Start in timeline seconds."""
    end_ppq: float | None = None
    """End in quarter notes."""
    end_sample: int | None = None
    """End in timeline samples."""
    end_sec: float | None = None
    """End in timeline seconds."""


@dataclass(frozen=True, slots=True)
class SidechainSource:
    """A sidechain key source: a track lane or a bus, by id.

    Passed to ``set_strip_sidechain`` / ``set_bus_sidechain`` and their ``can_set_*``
    queries; pass ``None`` instead of a source to remove a binding.
    """

    kind: SidechainSourceKind
    """``"track"`` or ``"bus"``: what ``id`` names."""
    id: int
    """Track id or bus id, per ``kind``; never 0."""


@dataclass(frozen=True, slots=True)
class SidechainCheck:
    """Outcome of a ``can_set_*_sidechain`` query.

    ``ok`` is True when the matching setter would accept the binding;
    otherwise ``reason`` names the first refusal as a :data:`SidechainRefusal`.
    """

    ok: bool
    """True when the matching setter would accept the binding."""
    reason: SidechainRefusal | None = None
    """First refusal (:data:`SidechainRefusal`); None when ``ok``."""


@dataclass(frozen=True, slots=True)
class EngineMarker:
    """Timeline marker stored on the realtime engine transport, as read back.

    ``kind`` is a :data:`MarkerKind` token; ``key_fifths`` (-7..7, sharps
    positive) and ``key_minor`` apply only to the ``"keySignature"`` kind.
    Every field after ``ppq`` is keyword-only. A marker to store whose id the
    engine should allocate is an :class:`EngineMarkerInput`.
    """

    ppq: float
    """Marker position in PPQ."""
    _: KW_ONLY
    id: int
    """Marker id, unique within the engine."""
    name: str | None = None
    """Marker text, at most 63 UTF-8 bytes; ``None`` for an unnamed marker."""
    kind: MarkerKind = "marker"
    """Marker kind (:data:`MarkerKind`)."""
    key_fifths: int = 0
    """Key signature in sharps (positive) or flats (negative), -7..7."""
    key_minor: bool = False
    """Whether the key is minor."""


@dataclass(frozen=True, slots=True)
class EngineMarkerInput:
    """A marker handed to :meth:`~libsonare.RealtimeEngine.set_markers`.

    Same fields as :class:`EngineMarker`, except that ``id`` may be omitted to
    have one allocated that no stored or listed marker carries.
    """

    ppq: float
    """Marker position in PPQ."""
    _: KW_ONLY
    id: int | None = None
    """Marker id, positive and unique within the list; ``None`` allocates one."""
    name: str | None = None
    """Marker text, at most 63 UTF-8 bytes; ``None`` for an unnamed marker."""
    kind: MarkerKind = "marker"
    """Marker kind (:data:`MarkerKind`)."""
    key_fifths: int = 0
    """Key signature in sharps (positive) or flats (negative), -7..7."""
    key_minor: bool = False
    """Whether the key is minor."""


@dataclass(frozen=True, slots=True)
class ProjectMarker:
    """Timeline marker stored on a headless :class:`~libsonare._project.Project`.

    Same shape as :class:`EngineMarker`: ``kind`` is a :data:`MarkerKind`
    token and ``key_fifths`` / ``key_minor`` apply only to the
    ``"keySignature"`` kind. Every field after ``ppq`` is keyword-only.
    """

    ppq: float
    """Marker position in PPQ."""
    _: KW_ONLY
    id: int
    """Marker id."""
    name: str | None = None
    """Marker text; ``None`` for an unnamed marker."""
    kind: MarkerKind = "marker"
    """Marker kind (:data:`MarkerKind`)."""
    key_fifths: int = 0
    """Key signature in sharps (positive) or flats (negative), -7..7."""
    key_minor: bool = False
    """Whether the key is minor."""


@dataclass(frozen=True, slots=True)
class ProjectTrack:
    """Read-only, stored-order project track descriptor."""

    id: int
    """Track id."""
    kind: ProjectTrackKind
    """What the track carries (:data:`ProjectTrackKind`)."""
    midi_destination_id: int
    """MIDI destination id the track routes to."""
    gain: float
    """Linear track gain."""
    pan: float
    """Track pan position."""
    mute: bool
    """Whether the track is muted."""
    solo: bool
    """Whether the track is soloed."""
    name: str | None
    """Track name; None for an unnamed track."""


@dataclass(frozen=True, slots=True)
class ProjectClip:
    """Read-only, stored-order project clip descriptor."""

    id: int
    """Clip id."""
    track_id: int
    """Id of the track holding the clip."""
    source_id: int
    """Id of the clip's source."""
    source_kind: ProjectSourceKind
    """What the clip's source holds (:data:`ProjectSourceKind`)."""
    start_ppq: float
    """Clip start position in PPQ."""
    length_ppq: float
    """Clip length in PPQ."""
    source_offset_ppq: float
    """Offset into the source in PPQ where the clip starts reading."""
    gain: float
    """Linear gain of the clip."""
    loop_mode: ProjectLoopMode
    """Whether the clip loops (:data:`ProjectLoopMode`)."""
    loop_length_ppq: float | None
    """Loop length in PPQ as set; ``None`` when the clip loops its whole length."""


@dataclass(frozen=True, slots=True)
class ProjectSource:
    """Read-only, stored-order project source descriptor."""

    id: int
    """Source id."""
    kind: ProjectSourceKind
    """What the source holds (:data:`ProjectSourceKind`)."""
    num_channels: int
    """Number of channels."""
    storage_handle_id: int
    """Id of the storage handle that holds the source data."""
    sample_rate_hint: float
    """Sample rate hint of the source, in Hz."""
    name_or_uri: str
    """Display name or URI of the source."""
    content_hash: str | None = None
    """Content hash of an audio source; ``None`` when unset."""
    external_stem_role: str | None = None
    """External stem role of an audio source; ``None`` when unset."""


@dataclass(frozen=True, slots=True)
class EngineMetronomeConfig:
    """Realtime engine metronome click configuration."""

    enabled: bool = False
    """Whether the metronome clicks."""
    beat_gain: float = 0.35
    """Linear gain of the other beat clicks."""
    accent_gain: float = 0.7
    """Linear gain of the downbeat click."""
    click_samples: int | None = None
    """Click length in samples, 1..384000; None uses :attr:`click_sec`."""
    click_sec: float | None = None
    """Click length in seconds, up to 1; None selects the engine default (2 ms)."""


@dataclass(frozen=True, slots=True)
class EngineClip:
    """Owned audio clip schedule for realtime engine playback."""

    id: int
    """Clip id."""
    channels: PlanarChannels | None
    """Per-channel sample data; None when the clip is read through :attr:`page_provider`."""
    start_ppq: float
    """Clip start position in PPQ."""
    track_id: int | None = None
    """Id of the track the clip plays on; None plays it on no track lane."""
    length_samples: int | None = None
    """Playback length in frames; None plays to the end of the source."""
    clip_offset_samples: int = 0
    """Offset into the source, in samples, where playback starts."""
    loop: bool = False
    """Whether the clip loops over its length."""
    gain: float = 1.0
    """Linear gain applied to the clip."""
    fade_in_samples: int = 0
    """Length of the linear fade-in, in samples."""
    fade_out_samples: int = 0
    """Length of the linear fade-out, in samples."""
    warp_mode: ProjectWarpMode = "off"
    """Warp mode (:data:`ProjectWarpMode`)."""
    warp_anchors: list[tuple[float, float]] | None = None
    """Warp anchors as (source, target) pairs; None for no anchors."""
    page_provider: object | None = None
    """Provider that supplies source pages on demand; None for in-memory channels."""


@dataclass(frozen=True, slots=True)
class EngineMidiEvent:
    """Absolute render-frame MIDI event for realtime engine MIDI clips.

    ``group`` is redundant with ``word0``, which already carries the UMP group in
    bits 24..27. The engine reads the group from ``word0`` -- the form that
    reaches a device or a file -- so packing it there is sufficient and a
    ``group`` that contradicts ``word0`` is ignored. It must still be in
    ``[0, 15]``; anything else is rejected as a malformed event.

    Utility (``word0`` type nibble ``0x0``) and UMP Stream (``0xF``) messages
    have no group field -- those bits are Reserved and ``form``/``status``
    respectively -- so they always read as group 0 and packing a group into
    them has no effect.
    """

    render_frame: int
    """Absolute sample position of the event on the engine timeline."""
    word0: int = 0
    """First UMP word."""
    word1: int = 0
    """Second UMP word."""
    word2: int = 0
    """Third UMP word."""
    word3: int = 0
    """Fourth UMP word."""
    word_count: int = 0
    """Number of valid UMP words."""
    group: int = 0
    """UMP group, 0..15; the group in :attr:`word0` takes precedence."""
    sysex_handle: int = 0
    """Handle of the SysEx payload the event refers to; 0 for none."""


@dataclass(frozen=True, slots=True)
class EngineMidiClipSchedule:
    """Compiled realtime MIDI clip schedule."""

    events: list[EngineMidiEvent]
    """Events of the clip, with absolute render frames."""
    id: int | None = None
    """Clip id; None leaves the clip unidentified."""
    track_id: int | None = None
    """Id of the track the clip belongs to; None is no track."""
    # MIDI destination to route to. None (the default) falls back to track_id;
    # an explicit value, including 0, is forwarded unchanged.
    destination_id: int | None = None
    """MIDI destination id; None falls back to :attr:`track_id`."""
    start_sample: int = 0
    """Clip start position in samples."""
    start_ppq: float = 0.0
    """Clip start position in PPQ."""
    length_samples: int | None = None
    """Clip length in samples; None is open-ended."""
    loop: bool = False
    """Whether the clip loops."""
    loop_length_samples: int | None = None
    """Loop length in samples, read only when :attr:`loop` is set; None is no loop length."""
    # Linear gain applied to the destination instrument's rendered audio while
    # this clip is the most recently started active clip on it.
    gain: float = 1.0
    """Linear gain applied to the destination instrument's audio while the clip is active."""
    # Linear fade lengths over the clip's full length (not per internal loop);
    # fade_out_samples must be 0 for an open-ended clip (length_samples None).
    fade_in_samples: int = 0
    """Linear fade-in length in samples over the clip's full length."""
    fade_out_samples: int = 0
    """Linear fade-out length in samples; 0 for an open-ended clip."""


@dataclass(frozen=True, slots=True)
class ClipPageRequest:
    """Paged clip sample request drained from the realtime engine."""

    clip_id: int
    """Id of the clip whose samples are requested."""
    channel: int
    """Zero-based channel index of the requested page."""
    sample: int
    """First sample frame of the requested page within the clip source."""


@dataclass(frozen=True, slots=True)
class EngineCaptureStatus:
    """Capture progress for the realtime engine recording sink."""

    captured_frames: int
    """Number of frames captured so far."""
    overflow_count: int
    """Number of capture overflows since the sink was prepared."""
    armed: bool
    """Whether the recording sink is armed."""
    punch_enabled: bool
    """Whether punch in/out gating is enabled."""
    source: EngineCaptureSource
    """What the sink captures (:data:`EngineCaptureSource`: ``"output"`` or ``"input"``)."""
    record_offset_samples: int
    """Capture alignment offset in samples relative to the punch start."""


EngineDither: TypeAlias = Literal["rpdf", "tpdf", "noiseShaped"]
"""Dither shape of :attr:`EngineBounceOptions.dither`; omitting it renders undithered."""
_ENGINE_DITHERS: tuple[str, ...] = get_args(EngineDither)


@dataclass(frozen=True, slots=True)
class EngineBounceOptions:
    """Offline export options for the realtime engine.

    ``num_channels`` is required. An omitted ``block_size`` is the default
    offline block, capped at the engine's prepared maximum; an explicit 0 is
    refused. ``source_sample_rate`` and ``target_sample_rate`` both default to
    ``None``: the source is the engine's prepared rate, and the target is the
    source (no resampling). A given ``source_sample_rate`` must equal the
    prepared rate, and an explicit 0 is refused for either. ``target_lufs`` and
    ``dither_seed`` are ``None`` for the library value (-14 LUFS and the
    built-in seed); a given value is read literally, so ``0`` is 0 LUFS or seed 0.
    """

    total_frames: int
    """Number of frames to render."""
    num_channels: int
    """Number of channels rendered."""
    block_size: int | None = None
    """Offline render block in frames; None selects the default block."""
    target_sample_rate: int | None = None
    """Output sample rate in Hz; None keeps the source rate (no resampling)."""
    source_sample_rate: int | None = None
    """Rate the engine renders at, in Hz; None is the engine's prepared rate."""
    normalize_lufs: bool = False
    """Whether to normalize the result to :attr:`target_lufs`."""
    target_lufs: float | None = None
    """Loudness target in LUFS, applied when :attr:`normalize_lufs` is set; None is -14."""
    dither: EngineDither | None = None
    """Dither shape, an :data:`EngineDither` token; None renders undithered."""
    dither_bits: int = 16
    """Target bit depth the dither is shaped for, in [2, 32]."""
    dither_seed: int | None = None
    """Seed of the dither noise generator; None is the built-in seed."""


@dataclass(frozen=True, slots=True)
class EngineBounceResult:
    """Interleaved offline export result from the realtime engine."""

    interleaved: np.ndarray
    """Rendered samples, channel-interleaved."""
    frames: int
    """Number of frames in :attr:`interleaved`."""
    num_channels: int
    """Number of interleaved channels."""
    sample_rate: int
    """Sample rate of the result, in Hz."""
    integrated_lufs: float
    """Integrated loudness of the rendered audio, in LUFS."""
    #: Samples whose value before quantization and dither was past full scale
    #: (``|x| > 1.0``), summed over the channels.
    quantize_clipped_samples: int | None = None
    """Samples clipped by the dither; None when the bounce did not dither."""


@dataclass(frozen=True, slots=True)
class EngineFreezeOptions:
    """Offline freeze options for replacing current engine output with a clip.

    ``num_channels`` is required; ``block_size`` is as in :class:`EngineBounceOptions`.
    """

    total_frames: int
    """Number of frames to render."""
    num_channels: int
    """Number of channels rendered."""
    block_size: int | None = None
    """Offline render block in frames; None selects the default block."""
    clip_id: int = 1
    """Id given to the frozen clip."""
    start_ppq: float = 0.0
    """Start position of the frozen clip in PPQ."""
    gain: float = 1.0
    """Linear gain of the frozen clip."""


@dataclass(frozen=True, slots=True)
class EngineFreezeResult:
    """Result of freezing current engine output into a scheduled clip."""

    clip_id: int
    """Id of the scheduled frozen clip."""
    frames: int
    """Number of frames rendered into the clip."""
    num_channels: int
    """Number of channels of the clip."""


EngineGraphNodeType = Literal["passThrough", "gain"]
"""Builtin processor node type for realtime engine graphs."""
_GRAPH_NODE_TYPES: tuple[str, ...] = get_args(EngineGraphNodeType)


EngineGraphMix = Literal["replace", "add"]
"""Connection mix mode for realtime engine graphs.

Edges into one destination port are taken in connection order. ``"replace"``
overwrites what earlier edges put on the port; ``"add"`` accumulates onto it.
A port's first edge initializes it either way, so the two differ only on a
shared port. An omitted ``mix`` is ``"add"``, unlike a zero-initialized C
connection, which is ``"replace"``.
"""
_GRAPH_MIXES: tuple[str, ...] = get_args(EngineGraphMix)


@dataclass(frozen=True, slots=True)
class EngineGraphNode:
    """Prepared realtime engine graph node."""

    id: str
    """Node id, unique within the graph."""
    type: EngineGraphNodeType = "passThrough"
    """Builtin processor type (:data:`EngineGraphNodeType`)."""
    gain_db: float = 0.0
    """Gain in dB, applied by the ``"gain"`` node type."""
    num_ports: int | None = None
    """Port count of the node; None uses the spec's ``num_channels``."""


@dataclass(frozen=True, slots=True)
class EngineGraphConnection:
    """Prepared realtime engine graph connection."""

    source_node: str
    """Id of the source node."""
    source_port: int
    """Zero-based output port on the source node."""
    dest_node: str
    """Id of the destination node."""
    dest_port: int
    """Zero-based input port on the destination node."""
    mix: EngineGraphMix = "add"
    """How the edge combines with earlier edges on the port (:data:`EngineGraphMix`)."""


@dataclass(frozen=True, slots=True)
class EngineGraphParameterBinding:
    """Map an engine automation parameter id to a graph node processor."""

    param_id: int
    """Engine automation parameter id."""
    node_id: str
    """Id of the graph node whose processor receives the parameter."""


@dataclass(frozen=True, slots=True)
class EngineGraphSpec:
    """Prepared realtime engine graph specification."""

    nodes: list[EngineGraphNode]
    """Nodes of the graph."""
    connections: list[EngineGraphConnection]
    """Edges between node ports."""
    input_node: str
    """Id of the node that receives the engine input."""
    output_node: str
    """Id of the node whose output is the graph output."""
    num_channels: int = 2
    """Channel count of the graph."""
    parameter_bindings: list[EngineGraphParameterBinding] | None = None
    """Automation parameter bindings; None for none."""


@dataclass(frozen=True, slots=True, eq=False)
class MeterTelemetryRecord:
    """A per-plane meter snapshot for a mix target.

    ``peak_db``/``rms_db``/``true_peak_dbtp`` carry ``num_channels`` valid planes
    in canonical WAVE order (5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lss
    Rss); a stereo target's left and right are planes 0 and 1.

    ``true_peak_dbtp``/``max_true_peak_dbtp`` are inter-sample peaks from the
    ITU-R BS.1770-4 polyphase reconstruction at 4x. They are a streaming
    measurement that runs one reconstruction-filter group delay (a few samples)
    behind the block, so the reading does not depend on the block size.
    :func:`metering_true_peak_dbtp` measures a whole signal at once.
    """

    target_id: int
    """Id of the mix target the snapshot belongs to."""
    render_frame: int
    """Render frame the snapshot was taken at."""
    seq: int
    """Sequence number of the record."""
    num_channels: int
    """Number of valid planes."""
    peak_db: npt.NDArray[np.float32]
    """Sample peak per plane, in dBFS; -inf for silence, NaN in a plane not written."""
    rms_db: npt.NDArray[np.float32]
    """RMS level per plane, in dBFS; -inf for silence, NaN in a plane not measured."""
    true_peak_dbtp: npt.NDArray[np.float32]
    """True peak per plane, in dBTP; -inf for silence, NaN in a plane not measured."""
    max_true_peak_dbtp: float | None
    """Maximum true peak over all planes, in dBTP; -inf for silence, None when not measured."""
    correlation: float | None
    """Stereo phase correlation, -1..1; None when not measured."""
    mono_compat_width: float | None
    """Stereo width measure for mono compatibility; None when not measured, inf when the mid
    signal is silent but the side is not."""
    momentary_lufs: float | None
    """Momentary loudness, in LUFS; -inf for silence, None until the window fills."""
    short_term_lufs: float | None
    """Short-term loudness, in LUFS; -inf for silence, None until the window fills."""
    integrated_lufs: float | None
    """Integrated loudness, in LUFS; None before the first gating block, -inf while nothing
    passes the gate."""
    gain_reduction_db: float
    """Gain reduction applied, in dB."""
    dropped_records: int
    """Records dropped because the telemetry queue was full."""
    input_peak_db: npt.NDArray[np.float32] = field(default_factory=_empty_float32)
    """Input-tap sample peak per plane, in dBFS; -inf for silence, NaN in a plane not written."""


@dataclass(frozen=True, slots=True, eq=False)
class ScopeTelemetryRecord:
    """A spectrum/vectorscope snapshot drained from the realtime engine.

    Each vectorscope point is an ``(left, right)`` tuple — the idiomatic Python
    shape. The Node / WASM surfaces expose the same data as ``{left, right}``
    objects; this representation difference is intentional, the values match.
    """

    target_id: int
    """Id of the mix target the snapshot belongs to."""
    render_frame: int
    """Render frame the snapshot was taken at."""
    seq: int
    """Sequence number of the record."""
    dropped_records: int
    """Records dropped because the telemetry queue was full."""
    bands: npt.NDArray[np.float32]
    """Spectrum band levels; -inf for a band of zero power, NaN for a band no bin reaches and
    before the first transform."""
    points: list[tuple[float, float]]
    """Vectorscope points as (left, right) tuples."""


@dataclass(frozen=True, slots=True)
class ExternalMidiEvent:
    """One lowered MIDI 1.0 message drained from the external-MIDI output queue.

    Each event is a single MIDI 1.0 byte message (``bytes`` is 1..3 bytes).
    ``destination_id`` is the MIDI destination id the event was routed to, or
    ``None`` for a clock/transport byte meant for every external port (forwarded
    for external tempo sync). ``render_frame`` is the absolute engine device
    render frame for every record, not an offset within a block.
    """

    destination_id: int | None
    """MIDI destination id; None for a clock/transport byte meant for every external port."""
    render_frame: int
    """Absolute engine device render frame of the event."""
    bytes: bytes
    """The MIDI 1.0 message bytes (1..3)."""


@dataclass(frozen=True, slots=True)
class TransportState:
    """Read-only snapshot of the realtime engine transport state."""

    playing: bool
    """Whether the transport is playing."""
    looping: bool
    """Whether looping is enabled."""
    render_frame: int
    """Absolute render frame of the snapshot."""
    sample_position: int
    """Playhead position in samples."""
    ppq_position: float
    """Playhead position in PPQ."""
    bpm: float
    """Tempo in beats per minute."""
    loop_start_ppq: float
    """Loop start in PPQ."""
    loop_end_ppq: float
    """Loop end in PPQ."""
    sample_rate: int
    """Sample rate in Hz."""
    # Musical position derived from the tempo map (computed every block).
    bar_start_ppq: float
    """PPQ position of the start of the current bar."""
    bar_index: int
    """Zero-based index of the current bar."""
    time_signature: TimeSignature
    """Time signature in effect at the playhead."""
    # One-based beat within the current bar; ``bar_index`` above is zero-based.
    beat: int
    """One-based beat within the current bar."""
    # Fractional position within the current beat, in [0, 1).
    beat_fraction: float
    """Fractional position within the current beat, in [0, 1)."""


@dataclass(frozen=True, slots=True)
class Section:
    """A detected song-structure section.

    ``energy_level`` is the median frame RMS in the section over the loudest
    section's, in [0, 1]. ``group`` numbers the section's material: sections
    sharing a value repeat the same material, numbered 0, 1, ... by first
    appearance. ``"unknown"`` means no labelling rule matched and
    comes with ``confidence`` 0.
    """

    type: SectionType
    """Section label (:data:`SectionType`)."""
    start_sec: float
    """Section start in seconds."""
    end_sec: float
    """Section end in seconds."""
    energy_level: float
    """Median frame RMS in the section over the loudest section's, in [0, 1]."""
    confidence: float
    """Confidence of the section label; 0 when the type is ``"unknown"``."""
    group: int
    """Index of the section's material; sections sharing a value repeat the same material."""
    name: str
    """Display name of the section type."""


@dataclass(frozen=True, slots=True)
class SectionAnalysis:
    """What :func:`analyze_sections` returns: the sections and the form they spell."""

    sections: list[Section]
    """Detected song-structure sections, in time order."""
    form: str
    """Musical form label, one letter per section (e.g. ``"AABA"``); empty when none."""


_SECTION_NAMES: dict[SectionType, str] = {
    "intro": "Intro",
    "verse": "Verse",
    "preChorus": "Pre-Chorus",
    "chorus": "Chorus",
    "bridge": "Bridge",
    "instrumental": "Instrumental",
    "outro": "Outro",
    "unknown": "Unknown",
}


@dataclass(frozen=True, slots=True)
class Boundary:
    """A detected structural transition."""

    time_sec: float
    """Seconds. The authoritative output."""
    frame: int
    """Index into the analysis grid, which is not the source's STFT grid: input
    above 22.05 kHz is resampled before any feature is computed, and a long input
    is additionally mean-pooled (see :attr:`BoundaryResult.frame_stride`). Use
    :attr:`time_sec` for any sample or second mapping."""
    strength: float
    """Novelty score at the boundary."""


@dataclass(frozen=True, slots=True, eq=False)
class BoundaryResult:
    """Structural boundaries and the novelty curve (float32) they were picked from."""

    boundaries: list[Boundary]
    """Detected boundaries in time order."""
    novelty_curve: npt.NDArray[np.float32]
    """Scaled by its own maximum, so values are in [0, 1] and a peak of 1.0 means
    "the most novel frame here" rather than "a large change". Multiply by
    :attr:`novelty_peak` to recover the raw response ``absolute_threshold`` is
    compared against."""
    novelty_peak: float | None
    """``None`` when nothing rose above the numerical floor."""
    sample_rate: int
    """The rate the analysis ran at, not the input's."""
    hop_length: int
    """Hop length of the analysis grid, in samples at :attr:`sample_rate`."""
    frame_stride: int
    """Raw frames averaged per analysis frame; 1 unless the input was pooled."""


@dataclass(frozen=True, slots=True)
class MelodyPoint:
    """A single point on a melody contour; ``frequency_hz`` is ``None`` when unvoiced."""

    time_sec: float
    """Time of the point in seconds."""
    frequency_hz: float | None
    """Fundamental frequency in Hz; None when unvoiced."""
    confidence: float
    """Confidence of the pitch estimate."""


@dataclass(frozen=True, slots=True)
class MelodyResult:
    """Melody contour analysis result."""

    points: list[MelodyPoint]
    """Contour points in time order."""
    pitch_range_octaves: float
    """Pitch range of the contour, in octaves."""
    pitch_stability: float
    """Stability of the pitch over the contour."""
    mean_frequency_hz: float
    """Mean fundamental frequency over voiced points, in Hz."""
    vibrato_rate_hz: float
    """Vibrato rate in Hz."""
