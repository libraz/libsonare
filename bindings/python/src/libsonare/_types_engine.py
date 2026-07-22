"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum

from ._types_analysis import (
    AutomationCurve,
    EngineTelemetryError,
    EngineTelemetryType,
    SectionType,
    TimeSignature,
)


@dataclass(frozen=True, slots=True)
class MixMeterSnapshot:
    """Realtime mixer meter snapshot for one strip."""

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
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
    seq: int
    # Per-plane surround meters (5.1/7.1). Each tuple holds channel_count values;
    # indices 0/1 mirror the *_l/*_r stereo fields above.
    channel_count: int = 0
    peak_db: tuple[float, ...] = ()
    rms_db: tuple[float, ...] = ()
    true_peak_db: tuple[float, ...] = ()


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

    @property
    def renderFrame(self) -> int:  # noqa: N802
        return self.render_frame

    @property
    def timelineSample(self) -> int:  # noqa: N802
        return self.timeline_sample

    @property
    def audibleTimelineSample(self) -> int:  # noqa: N802
        return self.audible_timeline_sample

    @property
    def graphLatencySamplesQ8(self) -> int:  # noqa: N802
        return self.graph_latency_samples_q8


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
    curve_to_next: AutomationCurve = AutomationCurve.LINEAR


class MarkerKind(IntEnum):
    """Timeline marker kind. Mirrors SonareMarkerKind in the C ABI and the
    other bindings' marker-kind enums; the values are part of the ABI."""

    MARKER = 0
    TEXT = 1
    LYRIC = 2
    CUE_POINT = 3
    KEY_SIGNATURE = 4


@dataclass(frozen=True, slots=True)
class EngineMarker:
    """Timeline marker used by the realtime engine transport.

    ``kind`` is a :class:`MarkerKind` ordinal; ``key_fifths`` (-7..7, sharps
    positive) and ``key_minor`` apply only to the key-signature kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: int = 0
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class ProjectMarker:
    """Timeline marker stored on a headless :class:`~libsonare._project.Project`.

    Same shape as :class:`EngineMarker`: ``kind`` is a :class:`MarkerKind`
    ordinal and ``key_fifths`` / ``key_minor`` apply only to the key-signature
    kind.
    """

    id: int
    ppq: float
    name: str = ""
    kind: int = 0
    key_fifths: int = 0
    key_minor: bool = False


@dataclass(frozen=True, slots=True)
class EngineMetronomeConfig:
    """Realtime engine metronome click configuration."""

    enabled: bool = False
    beat_gain: float = 0.35
    accent_gain: float = 0.7
    click_samples: int = 96
    # Click duration in seconds; used when click_samples is 0 to derive the click
    # length from the prepared sample rate. 0.0 selects the engine default (2 ms).
    click_seconds: float = 0.0


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
    warp_mode: str | int = "off"
    warp_anchors: list[tuple[float, float]] | None = None
    page_provider: object | None = None


@dataclass(frozen=True, slots=True)
class EngineMidiEvent:
    """Absolute render-frame MIDI event for realtime engine MIDI clips."""

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
    destination_id: int = 0
    start_sample: int = 0
    start_ppq: float = 0.0
    length_samples: int = 0
    loop: bool = False
    loop_length_samples: int = 0


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
    source: str
    record_offset_samples: int


@dataclass(frozen=True, slots=True)
class EngineBounceOptions:
    """Offline export options for the realtime engine."""

    total_frames: int
    block_size: int = 128
    num_channels: int = 2
    target_sample_rate: int = 48000
    source_sample_rate: int = 48000
    normalize_lufs: bool = False
    target_lufs: float = -14.0
    dither: int = 0
    dither_bits: int = 16
    dither_seed: int = 0


@dataclass(frozen=True, slots=True)
class EngineBounceResult:
    """Interleaved offline export result from the realtime engine."""

    interleaved: list[float]
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float


@dataclass(frozen=True, slots=True)
class EngineFreezeOptions:
    """Offline freeze options for replacing current engine output with a clip."""

    total_frames: int
    block_size: int = 128
    num_channels: int = 2
    clip_id: int = 1
    start_ppq: float = 0.0
    gain: float = 1.0


@dataclass(frozen=True, slots=True)
class EngineFreezeResult:
    """Result of freezing current engine output into a scheduled clip."""

    clip_id: int
    frames: int
    num_channels: int


class EngineGraphNodeType(IntEnum):
    """Builtin processor node type for realtime engine graphs."""

    PASS_THROUGH = 0
    GAIN = 1


class EngineGraphMix(IntEnum):
    """Connection mix mode for realtime engine graphs."""

    REPLACE = 0
    ADD = 1


@dataclass(frozen=True, slots=True)
class EngineGraphNode:
    """Prepared realtime engine graph node."""

    id: str
    type: EngineGraphNodeType = EngineGraphNodeType.PASS_THROUGH
    gain_db: float = 0.0
    num_ports: int = 0


@dataclass(frozen=True, slots=True)
class EngineGraphConnection:
    """Prepared realtime engine graph connection."""

    source_node: str
    source_port: int
    dest_node: str
    dest_port: int
    mix: EngineGraphMix = EngineGraphMix.ADD


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
    """A meter snapshot drained from the realtime engine's meter tap."""

    target_id: int
    render_frame: int
    seq: int
    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int


@dataclass(frozen=True, slots=True)
class MeterTelemetryRecordWide:
    """A per-plane meter snapshot for a surround mix target.

    ``peak_db``/``rms_db``/``true_peak_db`` carry ``channel_count`` valid planes
    in canonical WAVE order (5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Lss Rss Ls
    Rs). Use this drain for a surround target; ``MeterTelemetryRecord`` stays the
    stereo fast path.
    """

    target_id: int
    render_frame: int
    seq: int
    channel_count: int
    peak_db: list[float]
    rms_db: list[float]
    true_peak_db: list[float]
    max_true_peak_db: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int


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

    Each event is a single MIDI 1.0 byte message (``bytes`` is 1..3 bytes). A
    ``destination_id`` of ``0xFFFFFFFF`` (4294967295) tags a clock/transport byte
    forwarded for external tempo sync; any other value is the MIDI destination id
    the event was routed to.
    """

    destination_id: int
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
    """A detected song-structure section."""

    type: SectionType
    start: float
    end: float
    energy_level: float
    confidence: float

    @property
    def name(self) -> str:
        names = {
            SectionType.INTRO: "Intro",
            SectionType.VERSE: "Verse",
            SectionType.PRE_CHORUS: "PreChorus",
            SectionType.CHORUS: "Chorus",
            SectionType.BRIDGE: "Bridge",
            SectionType.INSTRUMENTAL: "Instrumental",
            SectionType.OUTRO: "Outro",
            SectionType.UNKNOWN: "Unknown",
        }
        return names[self.type]


@dataclass(frozen=True, slots=True)
class SectionResult:
    """Song-structure analysis result."""

    sections: list[Section]


@dataclass(frozen=True, slots=True)
class MelodyPoint:
    """A single point on a melody contour."""

    time: float
    frequency: float
    confidence: float


@dataclass(frozen=True, slots=True)
class MelodyResult:
    """Melody contour analysis result."""

    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
