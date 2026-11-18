"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal, get_args

import numpy as np

from ._types_analysis import (
    TimeSignature,
)
from ._types_enums import (
    AutomationCurve,
    EngineTelemetryError,
    EngineTelemetryType,
    ProjectWarpMode,
    SectionType,
)


@dataclass(frozen=True, slots=True)
class MixMeterSnapshot:
    """Realtime mixer meter snapshot for one strip.

    ``true_peak_dbtp_l``/``true_peak_dbtp_r``/``max_true_peak_dbtp`` are inter-sample
    peaks from the ITU-R BS.1770-4 polyphase reconstruction at 4x. They are a
    streaming measurement that runs one reconstruction-filter group delay (a
    few samples) behind the block, so the reading does not depend on the block
    size. :func:`metering_true_peak_dbtp` measures a whole signal at once.
    """

    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    correlation: float
    mono_compat_width: float
    mono_compat_peak: float
    mono_compat_side_rms: float
    likely_mono_compatible: bool
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    true_peak_dbtp_l: float
    true_peak_dbtp_r: float
    max_true_peak_dbtp: float
    seq: int
    # Per-plane surround meters (5.1/7.1). Each tuple holds num_channels values;
    # indices 0/1 mirror the *_l/*_r stereo fields above. None before any audio.
    num_channels: int | None = None
    peak_db: tuple[float, ...] = ()
    rms_db: tuple[float, ...] = ()
    true_peak_dbtp: tuple[float, ...] = ()


@dataclass(frozen=True, slots=True)
class GoniometerPoint:
    """A single left/right sample pair for goniometer (vectorscope) display."""

    left: float
    right: float


@dataclass(frozen=True, slots=True)
class MixResult:
    """Result of rendering a small stereo mixer scene."""

    left: list[float]
    right: list[float]
    sample_rate: int
    meters: list[MixMeterSnapshot]


@dataclass(frozen=True, slots=True)
class EngineTelemetry:
    """Realtime engine telemetry event."""

    type: EngineTelemetryType
    error: EngineTelemetryError
    render_frame: int
    timeline_sample: int
    audible_timeline_sample: int
    graph_latency_samples_q8: int
    value: int


EngineTrackMonitorMode = Literal["off", "pfl", "afl"]
"""Per-track-lane monitor tap mode for the realtime engine."""
_TRACK_MONITOR_MODES: tuple[str, ...] = get_args(EngineTrackMonitorMode)


@dataclass(frozen=True, slots=True)
class ParameterInfo:
    """DAW parameter metadata for automation/introspection UIs."""

    id: int
    name: str
    unit: str
    min_value: float
    max_value: float
    default_value: float
    rt_safe: bool
    default_curve: AutomationCurve


@dataclass(frozen=True, slots=True)
class AutomationPoint:
    """PPQ automation breakpoint."""

    ppq: float
    value: float
    curve_to_next: AutomationCurve = "linear"


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

EngineCaptureSource = Literal["output", "input"]
"""What the realtime engine's recording sink captures."""
_CAPTURE_SOURCES: tuple[str, ...] = get_args(EngineCaptureSource)


@dataclass(frozen=True, slots=True)
class SidechainCheck:
    """Outcome of a ``can_set_*_sidechain`` query.

    ``ok`` is True when the matching setter would accept the binding;
    otherwise ``reason`` names the first refusal as a :data:`SidechainRefusal`.
    """

    ok: bool
    reason: SidechainRefusal | None = None


@dataclass(frozen=True, slots=True)
class EngineMarker:
    """Timeline marker used by the realtime engine transport.

    ``kind`` is a :data:`MarkerKind` token; ``key_fifths`` (-7..7, sharps
    positive) and ``key_minor`` apply only to the ``"keySignature"`` kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: MarkerKind = "marker"
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class ProjectMarker:
    """Timeline marker stored on a headless :class:`~libsonare._project.Project`.

    Same shape as :class:`EngineMarker`: ``kind`` is a :data:`MarkerKind`
    token and ``key_fifths`` / ``key_minor`` apply only to the
    ``"keySignature"`` kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: MarkerKind = "marker"
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class ProjectTrack:
    """Read-only, stored-order project track descriptor."""

    id: int
    kind: int
    midi_destination_id: int
    gain: float
    pan: float
    mute: bool
    solo: bool
    name: str


@dataclass(frozen=True, slots=True)
class ProjectClip:
    """Read-only, stored-order project clip descriptor."""

    id: int
    track_id: int
    source_id: int
    source_kind: int
    start_ppq: float
    length_ppq: float
    source_offset_ppq: float
    gain: float
    loop_mode: int
    loop_length_ppq: float


@dataclass(frozen=True, slots=True)
class ProjectSource:
    """Read-only, stored-order project source descriptor."""

    id: int
    kind: int
    num_channels: int
    storage_handle_id: int
    sample_rate_hint: float
    name_or_uri: str
    content_hash: str = ""
    external_stem_role: str = ""


@dataclass(frozen=True, slots=True)
class EngineMetronomeConfig:
    """Realtime engine metronome click configuration."""

    enabled: bool = False
    beat_gain: float = 0.35
    accent_gain: float = 0.7
    # Explicit click length in samples (0..384000). Zero selects the
    # sample-rate-derived duration below.
    click_samples: int = 0
    # Click duration in seconds (0..1); used when click_samples is 0. 0.0 selects
    # the engine default (2 ms).
    click_sec: float = 0.0


@dataclass(frozen=True, slots=True)
class EngineClip:
    """Owned audio clip schedule for realtime engine playback."""

    id: int
    channels: list[list[float]] | None
    start_ppq: float
    track_id: int = 0
    length_samples: int | None = None
    clip_offset_samples: int = 0
    loop: bool = False
    gain: float = 1.0
    fade_in_samples: int = 0
    fade_out_samples: int = 0
    warp_mode: ProjectWarpMode = "off"
    warp_anchors: list[tuple[float, float]] | None = None
    page_provider: object | None = None


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
    word0: int = 0
    word1: int = 0
    word2: int = 0
    word3: int = 0
    word_count: int = 0
    group: int = 0
    sysex_handle: int = 0


@dataclass(frozen=True, slots=True)
class EngineMidiClipSchedule:
    """Compiled realtime MIDI clip schedule."""

    events: list[EngineMidiEvent]
    id: int = 0
    track_id: int = 0
    # MIDI destination to route to. None (the default) falls back to track_id;
    # an explicit value, including 0, is forwarded unchanged.
    destination_id: int | None = None
    start_sample: int = 0
    start_ppq: float = 0.0
    length_samples: int = 0
    loop: bool = False
    loop_length_samples: int = 0
    # Linear gain applied to the destination instrument's rendered audio while
    # this clip is the most recently started active clip on it.
    gain: float = 1.0
    # Linear fade lengths over the clip's full length (not per internal loop);
    # fade_out_samples must be 0 for an open-ended clip (length_samples <= 0).
    fade_in_samples: int = 0
    fade_out_samples: int = 0


@dataclass(frozen=True, slots=True)
class ClipPageRequest:
    """Paged clip sample request drained from the realtime engine."""

    clip_id: int
    channel: int
    sample: int


@dataclass(frozen=True, slots=True)
class EngineCaptureStatus:
    """Capture progress for the realtime engine recording sink."""

    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool
    source: EngineCaptureSource
    record_offset_samples: int


@dataclass(frozen=True, slots=True)
class EngineBounceOptions:
    """Offline export options for the realtime engine.

    ``num_channels`` is required. An omitted ``block_size`` is the default
    offline block, capped at the engine's prepared maximum; an explicit 0 is
    refused. ``source_sample_rate`` and ``target_sample_rate`` both default to
    ``0``: the source is the engine's prepared rate, and the target is the
    source (no resampling). A non-zero ``source_sample_rate`` must equal the
    prepared rate.
    """

    total_frames: int
    num_channels: int
    block_size: int | None = None
    target_sample_rate: int = 0
    source_sample_rate: int = 0
    normalize_lufs: bool = False
    target_lufs: float = -14.0
    dither: int = 0
    dither_bits: int = 16
    dither_seed: int = 0


@dataclass(frozen=True, slots=True)
class EngineBounceResult:
    """Interleaved offline export result from the realtime engine."""

    interleaved: np.ndarray
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float


@dataclass(frozen=True, slots=True)
class EngineFreezeOptions:
    """Offline freeze options for replacing current engine output with a clip.

    ``num_channels`` is required; ``block_size`` is as in :class:`EngineBounceOptions`.
    """

    total_frames: int
    num_channels: int
    block_size: int | None = None
    clip_id: int = 1
    start_ppq: float = 0.0
    gain: float = 1.0


@dataclass(frozen=True, slots=True)
class EngineFreezeResult:
    """Result of freezing current engine output into a scheduled clip."""

    clip_id: int
    frames: int
    num_channels: int


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
    type: EngineGraphNodeType = "passThrough"
    gain_db: float = 0.0
    num_ports: int = 0


@dataclass(frozen=True, slots=True)
class EngineGraphConnection:
    """Prepared realtime engine graph connection."""

    source_node: str
    source_port: int
    dest_node: str
    dest_port: int
    mix: EngineGraphMix = "add"


@dataclass(frozen=True, slots=True)
class EngineGraphParameterBinding:
    """Map an engine automation parameter id to a graph node processor."""

    param_id: int
    node_id: str


@dataclass(frozen=True, slots=True)
class EngineGraphSpec:
    """Prepared realtime engine graph specification."""

    nodes: list[EngineGraphNode]
    connections: list[EngineGraphConnection]
    input_node: str
    output_node: str
    num_channels: int = 2
    parameter_bindings: list[EngineGraphParameterBinding] | None = None


@dataclass(frozen=True, slots=True)
class MeterTelemetryRecord:
    """A meter snapshot drained from the realtime engine's meter tap.

    ``true_peak_dbtp_l``/``true_peak_dbtp_r``/``max_true_peak_dbtp`` are inter-sample
    peaks from the ITU-R BS.1770-4 polyphase reconstruction at 4x. They are a
    streaming measurement that runs one reconstruction-filter group delay (a
    few samples) behind the block, so the reading does not depend on the block
    size. :func:`metering_true_peak_dbtp` measures a whole signal at once.
    """

    target_id: int
    render_frame: int
    seq: int
    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    true_peak_dbtp_l: float
    true_peak_dbtp_r: float
    max_true_peak_dbtp: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    # Input tap peaks are appended to preserve positional construction of the
    # pre-V2 public record while making the source/input relationship explicit.
    input_peak_db_l: float = -120.0
    input_peak_db_r: float = -120.0


@dataclass(frozen=True, slots=True)
class MeterTelemetryRecordWide:
    """A per-plane meter snapshot for a surround mix target.

    ``peak_db``/``rms_db``/``true_peak_dbtp`` carry ``num_channels`` valid planes
    in canonical WAVE order (5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lss
    Rss). Use this drain for a surround target; ``MeterTelemetryRecord`` stays the
    stereo fast path.

    ``true_peak_dbtp``/``max_true_peak_dbtp`` are inter-sample peaks from the
    ITU-R BS.1770-4 polyphase reconstruction at 4x. They are a streaming
    measurement that runs one reconstruction-filter group delay (a few samples)
    behind the block, so the reading does not depend on the block size.
    :func:`metering_true_peak_dbtp` measures a whole signal at once.
    """

    target_id: int
    render_frame: int
    seq: int
    num_channels: int
    peak_db: list[float]
    rms_db: list[float]
    true_peak_dbtp: list[float]
    max_true_peak_dbtp: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    input_peak_db: list[float] = field(default_factory=list)


@dataclass(frozen=True, slots=True)
class ScopeTelemetryRecord:
    """A spectrum/vectorscope snapshot drained from the realtime engine.

    Each vectorscope point is an ``(left, right)`` tuple — the idiomatic Python
    shape. The Node / WASM surfaces expose the same data as ``{left, right}``
    objects; this representation difference is intentional, the values match.
    """

    target_id: int
    render_frame: int
    seq: int
    dropped_records: int
    bands: list[float]
    points: list[tuple[float, float]]


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
    render_frame: int
    bytes: bytes


@dataclass(frozen=True, slots=True)
class TransportState:
    """Read-only snapshot of the realtime engine transport state."""

    playing: bool
    looping: bool
    render_frame: int
    sample_position: int
    ppq_position: float
    bpm: float
    loop_start_ppq: float
    loop_end_ppq: float
    sample_rate: float
    # Musical position derived from the tempo map (computed every block).
    bar_start_ppq: float
    bar_count: int
    time_signature: TimeSignature
    # One-based beat within the current bar; ``bar_count`` above is zero-based.
    beat: int
    # Fractional position within the current beat, in [0, 1).
    beat_fraction: float


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
    start_sec: float
    end_sec: float
    energy_level: float
    confidence: float
    group: int = 0
    name: str = ""


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
    novelty_curve: np.ndarray
    """Scaled by its own maximum, so values are in [0, 1] and a peak of 1.0 means
    "the most novel frame here" rather than "a large change". Multiply by
    :attr:`novelty_peak` to recover the raw response ``absolute_threshold`` is
    compared against."""
    novelty_peak: float | None
    """``None`` when nothing rose above the numerical floor."""
    sample_rate: int
    """The rate the analysis ran at, not the input's."""
    hop_length: int
    frame_stride: int
    """Raw frames averaged per analysis frame; 1 unless the input was pooled."""


@dataclass(frozen=True, slots=True)
class MelodyPoint:
    """A single point on a melody contour; ``frequency_hz`` is ``None`` when unvoiced."""

    time_sec: float
    frequency_hz: float | None
    confidence: float


@dataclass(frozen=True, slots=True)
class MelodyResult:
    """Melody contour analysis result."""

    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency_hz: float
    vibrato_rate_hz: float
