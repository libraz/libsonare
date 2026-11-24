"""Headless arrangement / DAW project wrapper for libsonare.

This wraps the curated ``sonare_project_*`` C ABI (``include/sonare/sonare_c_project.h``):
an opaque :class:`SonareProject` handle over the arrangement control plane
(Project / EditHistory / EditCompiler / serializer / MIR bridges / SMF).

Every entry point is control-thread-only and performs no file or device I/O:
project JSON and SMF bytes are exchanged through in-memory buffers, so the host
owns storage. Heap buffers returned by the C layer are freed here (no leaks).

Availability: the symbols are always exported, but when libsonare was built
without ``SONARE_WITH_ARRANGEMENT`` every call returns
``SONARE_ERROR_NOT_SUPPORTED`` and :func:`project_abi_version` returns ``None``. The
:class:`Project` constructor checks the ABI version and raises a clear
``RuntimeError`` in that case.
"""

from __future__ import annotations

import ctypes
import math
import numbers
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from typing import (
    TYPE_CHECKING,
    Any,
    Literal,
    NotRequired,
    SupportsFloat,
    SupportsIndex,
    TypeAlias,
    TypedDict,
    cast,
    get_args,
)

import numpy as np

from ._errors import _entry, _malformed_output, _refuse, _refuse_type
from ._narrowing import _MAX_PUBLIC_PPQ, _narrow_int
from ._types_analysis import TimeSignature

if TYPE_CHECKING:
    from ._project import Project, ProjectTimeline

from ._engine_conversions import _automation_points_to_c
from ._errors import ErrorCode, SonareError
from ._ffi_types_mastering_project import (
    SONARE_ERROR_INVALID_STATE as SONARE_ERROR_INVALID_STATE,
)
from ._project_instruments import (
    _GS_EFX_REALIZATIONS as _GS_EFX_REALIZATIONS,
)
from ._project_instruments import (
    _SOURCE_BACKENDS as _SOURCE_BACKENDS,
)
from ._project_instruments import (
    BuiltinSynthConfig as BuiltinSynthConfig,
)
from ._project_instruments import (
    ExternalInstrument as ExternalInstrument,
)
from ._project_instruments import (
    GsEfxRealization as GsEfxRealization,
)
from ._project_instruments import (
    ProjectInstrumentBinding as ProjectInstrumentBinding,
)
from ._project_instruments import (
    ProjectSf2InstrumentBinding as ProjectSf2InstrumentBinding,
)
from ._project_instruments import (
    Sf2InstrumentConfig as Sf2InstrumentConfig,
)
from ._project_instruments import (
    Sf2ProgramStatus as Sf2ProgramStatus,
)
from ._project_instruments import (
    SourceBackend as SourceBackend,
)
from ._project_instruments import (
    SynthModRouting as SynthModRouting,
)
from ._project_instruments import (
    SynthPatch as SynthPatch,
)
from ._project_instruments import (
    _gs_efx_realization_value as _gs_efx_realization_value,
)
from ._project_instruments import (
    _make_instrument_callbacks as _make_instrument_callbacks,
)
from ._project_instruments import (
    _synth_param_info as _synth_param_info,
)
from ._project_instruments import (
    _synth_patch_arg as _synth_patch_arg,
)
from ._project_instruments import (
    _synth_waveform_value as _synth_waveform_value,
)
from ._project_instruments import (
    controller_profile_names as controller_profile_names,
)
from ._project_instruments import (
    synth_engine_param_info as synth_engine_param_info,
)
from ._project_instruments import (
    synth_gs_drum_kit_is_voiced_apart as synth_gs_drum_kit_is_voiced_apart,
)
from ._project_instruments import (
    synth_gs_drum_kit_name as synth_gs_drum_kit_name,
)
from ._project_instruments import (
    synth_gs_variation_is_voiced_apart as synth_gs_variation_is_voiced_apart,
)
from ._project_instruments import (
    synth_patch_param_info as synth_patch_param_info,
)
from ._project_instruments import (
    synth_preset_names as synth_preset_names,
)
from ._project_instruments import (
    synth_preset_patch as synth_preset_patch,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    Articulation as Articulation,
)
from ._project_synth import (
    ControllerAxis as ControllerAxis,
)
from ._project_synth import (
    ControllerInput as ControllerInput,
)
from ._project_synth import (
    MpeDimension as MpeDimension,
)
from ._project_synth import (
    NoteTracking as NoteTracking,
)
from ._project_synth import (
    PartRigMode as PartRigMode,
)
from ._project_synth import (
    SampleBank as SampleBank,
)
from ._project_synth import (
    SampleDescLoopMode as SampleDescLoopMode,
)
from ._project_synth import (
    SampleKeyTrack as SampleKeyTrack,
)
from ._project_synth import (
    SampleLoopMode as SampleLoopMode,
)
from ._project_synth import (
    SynthBodyType as SynthBodyType,
)
from ._project_synth import (
    SynthEngineMode as SynthEngineMode,
)
from ._project_synth import (
    SynthFilterModel as SynthFilterModel,
)
from ._project_synth import (
    SynthFilterOutput as SynthFilterOutput,
)
from ._project_synth import (
    SynthLfoShape as SynthLfoShape,
)
from ._project_synth import (
    SynthModDestination as SynthModDestination,
)
from ._project_synth import (
    SynthModSource as SynthModSource,
)
from ._project_synth import (
    SynthOscWaveform as SynthOscWaveform,
)
from ._project_synth import (
    SynthRetrigger as SynthRetrigger,
)
from ._project_synth import (
    SynthWaveform as SynthWaveform,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    _UINT32_MAX,
    SonareAutomationLaneDesc,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    _bool_flag,
    _check,
    _get_lib,
    _joined_names,
    _ordinal_token,
    _to_c_size_t,
    _token_ordinal,
    _utf8_arg,
)
from ._types_engine import AutomationPoint

# Must match SONARE_PROJECT_ABI_VERSION (include/sonare/sonare_c_project.h) and the other
# bindings' expected project ABI constant. A mismatch means the loaded native
# binary lays out the flat project PODs differently than this wrapper expects,
# or the arrangement subsystem was compiled out (runtime version 0).
_EXPECTED_PROJECT_ABI_VERSION = 3

# Track kind; a token's index is its SonareProjectTrackKind ordinal.
ProjectTrackKind: TypeAlias = Literal["audio", "midi", "aux"]
"""What a track carries: audio clips, MIDI clips, or an auxiliary bus."""
_TRACK_KINDS: tuple[str, ...] = get_args(ProjectTrackKind)

# Source kind; a token's index is its SonareProjectSourceKind ordinal.
ProjectSourceKind: TypeAlias = Literal["audio", "midi"]
"""What a project source holds: decoded audio or MIDI events."""
_SOURCE_KINDS: tuple[str, ...] = get_args(ProjectSourceKind)

# External stem layout; a token's index plus one is its SonareExternalStemLayout value.
ProjectStemLayout: TypeAlias = Literal["mono", "stereo"]
"""Channel layout of one stem given to :meth:`Project.import_external_stems`."""
_STEM_LAYOUTS: tuple[str, ...] = get_args(ProjectStemLayout)

# Clip fade curve; a token's index is its SonareProjectFadeCurve ordinal.
ProjectFadeCurve: TypeAlias = Literal["linear", "equalPower", "exponential", "logarithmic"]
"""Shape of a clip fade-in or fade-out."""
_FADE_CURVES: tuple[str, ...] = get_args(ProjectFadeCurve)

# Clip loop mode; a token's index is its SonareProjectLoopMode ordinal.
ProjectLoopMode: TypeAlias = Literal["off", "loop"]
"""Whether a clip loops."""
_LOOP_MODES: tuple[str, ...] = get_args(ProjectLoopMode)

# Clip-overlap policy; a token's index is its SonareProjectOverlapPolicy ordinal.
ProjectOverlapPolicy: TypeAlias = Literal["disallow", "allow"]
"""Whether clips on one track may overlap."""
_OVERLAP_POLICIES: tuple[str, ...] = get_args(ProjectOverlapPolicy)

# Automation target kind; runtime token order comes from the C ABI.
ProjectAutomationTargetKind: TypeAlias = Literal[
    "opaque",
    "trackFaderDb",
    "trackPan",
    "trackSendDb",
    "trackWidth",
    "trackInsertParam",
]
"""What an automation lane drives, as a token from the C ABI."""
_AUTOMATION_TARGET_KINDS: tuple[str, ...] = get_args(ProjectAutomationTargetKind)


@dataclass(frozen=True, slots=True)
class ProjectAutomationLane:
    """One automation lane returned by :meth:`Project.automation_lanes`."""

    track_id: int
    """Owning track id."""
    lane_id: int
    """Stable project-allocated lane id."""
    target_param_id: int | None
    """Opaque engine parameter id, or ``None`` for a typed target."""
    target_kind: ProjectAutomationTargetKind
    """Target kind token from the library."""
    points: tuple[AutomationPoint, ...]
    """Breakpoints in PPQ order."""
    send_id: str | None
    """Send identity for ``trackSendDb``."""
    insert_index: int | None
    """Strip insert position for ``trackInsertParam``."""
    processor: str | None
    """Processor catalog id for ``trackInsertParam``."""
    param_name: str | None
    """Processor parameter name for ``trackInsertParam``."""


# MIDI CC binding kind; a token's index is its SonareMidiCcBindingKind ordinal.
ProjectMidiCcBindingKind: TypeAlias = Literal["controlChange7", "controlChange14", "rpn", "nrpn"]
"""Controller shape of a :class:`MidiCcBinding`: 7-bit CC, 14-bit CC, RPN or NRPN."""
_MIDI_CC_BINDING_KINDS: tuple[str, ...] = get_args(ProjectMidiCcBindingKind)

# Key-annotation mode; a token's index is its project KeyMode ordinal.
ProjectKeyMode: TypeAlias = Literal[
    "unknown", "major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"
]
"""Mode of a key annotation segment (:meth:`Project.annotate_keys`)."""
_PROJECT_KEY_MODES: tuple[str, ...] = get_args(ProjectKeyMode)

# Chord-annotation quality; a token's index is its project ChordQuality ordinal.
ProjectChordQuality: TypeAlias = Literal[
    "unknown",
    "major",
    "minor",
    "diminished",
    "augmented",
    "dominant",
    "halfDiminished",
    "suspended",
]
"""Quality family of a chord annotation (:meth:`Project.annotate_chords`)."""
_PROJECT_CHORD_QUALITIES: tuple[str, ...] = get_args(ProjectChordQuality)


class ProjectKeySegment(TypedDict):
    """Key annotation mapping returned by :meth:`Project.key_annotations`.

    The same mapping shape is accepted by :meth:`Project.annotate_keys`.
    """

    start_ppq: float
    """Start of the key segment in PPQ."""
    end_ppq: float
    """End of the key segment in PPQ, after ``start_ppq``."""
    tonic_pc: int | None
    """Tonic pitch class 0..11, or ``None`` when unknown."""
    mode: ProjectKeyMode
    """Key-mode token, including ``"unknown"``."""


class ProjectChordSymbol(TypedDict):
    """Chord annotation mapping returned by :meth:`Project.chord_annotations`.

    The same mapping shape is accepted by :meth:`Project.annotate_chords`.
    """

    start_ppq: float
    """Start of the chord segment in PPQ."""
    end_ppq: float
    """End of the chord segment in PPQ, after ``start_ppq``."""
    root_pc: int | None
    """Root pitch class 0..11, or ``None`` when unknown."""
    quality: ProjectChordQuality
    """Chord quality token, including ``"unknown"``."""
    extensions: list[int]
    """Scale-degree extensions, empty when there are none."""
    slash_bass_pc: int | None
    """Slash-bass pitch class 0..11, or ``None`` when absent."""
    modulation_boundary: bool
    """Whether this chord marks a key change."""
    roman_numeral: NotRequired[str]
    """Optional Roman-numeral label, omitted when absent."""


def _automation_target_kind_value(value: object, key: str) -> int:
    return _token_ordinal(value, _automation_target_kind_names(), key)


def _automation_target_kind_names() -> tuple[ProjectAutomationTargetKind, ...]:
    raw = _get_lib().sonare_automation_target_kind_names()
    names = tuple(_joined_names(raw, "automation target kind names"))
    if not names:
        raise _malformed_output("automation target kind names are empty")
    if names != _AUTOMATION_TARGET_KINDS:
        unknown = next((name for name in names if name not in _AUTOMATION_TARGET_KINDS), None)
        if unknown is not None:
            raise _malformed_output(f"unknown automation target kind token: {unknown}")
        raise _malformed_output("automation target kind names do not match the Python mirror")
    return cast(tuple[ProjectAutomationTargetKind, ...], names)


@_entry("automation_target_kind_names")
def automation_target_kind_names() -> list[ProjectAutomationTargetKind]:
    """Return the automation target kind tokens in C ABI ordinal order."""
    return list(_automation_target_kind_names())


def _automation_target_kind_token(
    value: int, names: tuple[str, ...] | None = None
) -> ProjectAutomationTargetKind:
    token = _ordinal_token(
        value,
        _automation_target_kind_names() if names is None else names,
        "automation target kind",
    )
    return cast(ProjectAutomationTargetKind, token)


def _automation_target_param_id_value(value: object) -> int:
    """Validate a non-zero uint32 automation target parameter id; 0 is the unset id."""
    return _narrow_int(value, "target_param_id", 1, _UINT32_MAX)


def _fade_curve_value(curve: ProjectFadeCurve, key: str) -> int:
    return _token_ordinal(curve, _FADE_CURVES, key)


def _loop_mode_value(mode: ProjectLoopMode, key: str) -> int:
    return _token_ordinal(mode, _LOOP_MODES, key)


def _automation_lane_desc(
    target_param_id: object,
    points: Sequence[AutomationPoint] | None,
    target_kind: int,
    *,
    send_id: str | None = None,
    insert_index: int | None = None,
    processor: str | None = None,
    param_name: str | None = None,
) -> tuple[SonareAutomationLaneDesc, object]:
    """Marshal :class:`AutomationPoint` records into a SonareAutomationLaneDesc.

    Returns ``(desc, backing)``; the caller must keep ``backing`` (the C point
    array) alive for the duration of the native call.
    """
    target_id = _narrow_int(target_param_id, "target_param_id", 0, _UINT32_MAX)
    c_points = _automation_points_to_c(points if points is not None else [])
    desc = SonareAutomationLaneDesc(
        target_param_id=target_id,
        target_kind=target_kind,
        points=c_points,
        point_count=_to_c_size_t(len(c_points), "count"),
    )
    identity_buffers: list[bytes] = []
    if send_id is not None:
        encoded_send_id = _utf8_arg(send_id, "send_id")
        desc.send_id = encoded_send_id
        identity_buffers.append(encoded_send_id)
    if insert_index is not None:
        desc.insert_index = _narrow_int(insert_index, "insert_index", 0, (1 << 31) - 1)
    if processor is not None:
        encoded_processor = _utf8_arg(processor, "processor")
        desc.processor = encoded_processor
        identity_buffers.append(encoded_processor)
    if param_name is not None:
        encoded_param_name = _utf8_arg(param_name, "param_name")
        desc.param_name = encoded_param_name
        identity_buffers.append(encoded_param_name)
    return desc, (c_points, identity_buffers)


@dataclass(frozen=True)
class AssistSidecar:
    """One opaque assist sidecar stored on a :class:`Project`.

    ``payload`` is module-owned opaque bytes; ``module_id`` identifies the
    producing module. ``schema_version`` / ``target_track_id`` and the
    ``region_*_ppq`` scope mirror the C ``SonareProjectAssistSidecar``.
    """

    module_id: str
    """Id of the module that produced the sidecar; the key it is stored under."""
    schema_version: int
    """Module-defined schema version of ``payload``."""
    target_track_id: int
    """Id of the track the sidecar targets; 0 means project scope."""
    region_start_ppq: float
    """Start of the scoped region in PPQ."""
    region_end_ppq: float
    """End of the scoped region in PPQ."""
    payload: bytes
    """Opaque module-owned payload bytes."""


# A token's index is its SonareProjectDiagnostic severity ordinal.
ProjectDiagnosticSeverity: TypeAlias = Literal["error", "warning"]
"""How serious a compile diagnostic is; an error means no timeline was produced."""

# A token's index is its arrangement Diagnostic::Code ordinal.
ProjectDiagnosticCode: TypeAlias = Literal[
    "ok",
    "danglingSourceRef",
    "clipOverlap",
    "invalidTempo",
    "invalidPpq",
    "unsupportedGraph",
    "unsupportedMixing",
    "emptyAudioSource",
    "invalidSampleRate",
    "sourceKindMismatch",
    "midiClipNoInstrument",
    "raggedAudioSource",
    "loopCrossfadeUnavailable",
    "sharedChannelStrip",
    "automationLaneConflict",
    "invalidAutomationTargetKind",
    "automationLaneCapacity",
    "reservedAutomationRouteConflict",
    "automationTargetUnrouted",
]
"""What a compile diagnostic reports."""


@dataclass(frozen=True)
class ProjectDiagnostic:
    """One compile diagnostic surfaced by :meth:`Project.compile`."""

    code: ProjectDiagnosticCode
    """What the diagnostic reports."""
    severity: ProjectDiagnosticSeverity
    """``"error"`` means no timeline was produced; ``"warning"`` does not block one."""
    target_id: int | None
    """Affected clip, track or source id; ``None`` when the diagnostic names none."""
    message: str = ""
    """Human-readable detail for this diagnostic."""


@dataclass(frozen=True)
class NotePairValidation:
    """Result of :meth:`Project.validate_midi_notes`.

    ``ok`` is True when every note-on in the clip has a matching note-off;
    otherwise ``unmatched_note_ons`` / ``unmatched_note_offs`` count the
    hanging (unpaired) events.
    """

    ok: bool
    """True when every note-on has a matching note-off."""
    unmatched_note_ons: int
    """Note-ons with no matching note-off."""
    unmatched_note_offs: int
    """Note-offs with no matching note-on."""


@dataclass(frozen=True, kw_only=True)
class MidiCcBinding:
    """A MIDI CC <-> automation-parameter binding (see :func:`midi_cc_learn`).

    ``kind`` is a :data:`ProjectMidiCcBindingKind` token. For a 14-bit
    binding ``cc_lsb_number`` carries the LSB controller; for RPN/NRPN
    ``selector_msb`` / ``selector_lsb`` carry the parameter selector.
    ``param_id`` is the bound automation parameter; the unit-interval
    automation range maps onto ``[min_value, max_value]``.
    ``channel`` is 0..15, or ``None`` for a binding that answers any channel.
    Every field but ``cc_number`` and ``param_id`` has a default, so a 7-bit CC
    binding over the unit range is ``MidiCcBinding(cc_number=1, param_id=42)``.
    """

    cc_number: int
    """Controller number 0..127 (the MSB controller for a 14-bit CC)."""
    channel: int | None = None
    """MIDI channel 0..15; None matches any channel."""
    kind: ProjectMidiCcBindingKind = "controlChange7"
    """Controller shape, a ``ProjectMidiCcBindingKind`` token."""
    cc_lsb_number: int = 0
    """LSB controller number 0..127 of a 14-bit CC."""
    selector_msb: int = 0
    """Selector MSB 0..127 of an RPN/NRPN binding."""
    selector_lsb: int = 0
    """Selector LSB 0..127 of an RPN/NRPN binding."""
    consume: bool = False
    """Whether the live engine consumes matching MIDI CC events."""
    param_id: int
    """Automation parameter id the controller drives."""
    min_value: float = 0.0
    """Lower end of the mapped parameter range."""
    max_value: float = 1.0
    """Upper end of the mapped parameter range."""


@dataclass(frozen=True, slots=True)
class ProjectMidiEvent:
    """One project MIDI event: a PPQ position and a UMP message."""

    ppq: float
    """Event position in PPQ (quarter notes)."""
    word0: int
    """First UMP word of the message."""
    word1: int = 0
    """Second UMP word of the message."""
    sysex: bytes | None = None
    """Owned SysEx bytes for a Data64 event, including embedded zeros, or None without a payload."""


@dataclass(frozen=True, slots=True)
class ProjectTimeSignatureSegment:
    """The normalized time-signature segment active at a project position."""

    start_ppq: float
    """PPQ position where this normalized segment begins."""
    numerator: int
    """Beats per bar."""
    denominator: int
    """Beat unit, such as 4 for a quarter note."""


@dataclass(frozen=True, slots=True)
class ProjectPosition:
    """Effective tempo, time signature, bar, and beat at one PPQ position."""

    bpm: float
    """Instantaneous tempo at this position."""
    time_signature: ProjectTimeSignatureSegment
    """Normalized time-signature segment active at this position."""
    bar_start_ppq: float
    """PPQ start of the bar containing the position."""
    bar_index: int
    """Zero-based bar index."""
    beat: int
    """One-based beat within the bar."""
    beat_fraction: float
    """Position within the beat, in [0, 1)."""


class ProjectSection(TypedDict):
    """Labeled PPQ interval mapping used by :meth:`Project.annotate_sections`."""

    start_ppq: float
    """Section start in PPQ."""
    end_ppq: float
    """Section end in PPQ, after ``start_ppq``."""
    label: str
    """Nonblank UTF-8 label."""


@dataclass(frozen=True, slots=True)
class ProjectAutoTempoResult:
    """The selected tempo and detected meter from :meth:`Project.auto_tempo`."""

    bpm: float
    """Tempo installed in the project."""
    bpm_confidence: float
    """Confidence of the selected tempo candidate, in [0, 1]."""
    time_signature: TimeSignature
    """Detected meter, whether or not it was installed in the project."""


@dataclass(frozen=True, slots=True)
class ProjectBounceResult:
    """Rendered planar project audio and its sample rate."""

    channels: np.ndarray
    """Float32 planar audio with shape ``(num_channels, frames)``."""
    sample_rate: int
    """Sample rate of the rendered channels in Hz."""


@dataclass(frozen=True, slots=True, kw_only=True)
class ProjectControllerProfileBinding:
    """A controller profile selected for one instrument in a project bounce."""

    destination_id: int
    """ID of a hosted instrument destination."""
    preset_name: str
    """Name returned by :func:`controller_profile_names`."""


@dataclass(frozen=True, slots=True, kw_only=True)
class ProjectArticulationBinding:
    """A note articulation selected for one destination and channel in a project bounce."""

    destination_id: int
    """ID of a hosted instrument destination."""
    channel: int
    """MIDI channel in [0, 15]."""
    articulation: Articulation
    """Mode for overlapping notes on this channel."""


@dataclass(frozen=True)
class MidiRouteResult:
    """Result of :func:`midi_route_events`.

    ``events`` is the filtered / remapped event stream. ``overflowed`` is True
    when the router or caller capacity dropped events; ``overflow_count`` is how
    many were dropped.
    """

    events: list[ProjectMidiEvent]
    """Events that passed the filter, remapped as configured."""
    overflowed: bool
    """True when events were dropped for lack of router or caller capacity."""
    overflow_count: int
    """Number of events dropped."""


# Largest 7-bit MIDI data byte: a controller number or a selector byte.
_MIDI_DATA_MAX = 127

# The C ABI's "no pitch class" code; absence on the Python side.
_NO_PITCH_CLASS = 255


def _absent_or_in_range(value: object, name: str, upper: int, absent: int) -> int:
    """Map ``None`` to the C ABI's ``absent`` code; refuse a value outside [0, ``upper``]."""
    if value is None:
        return absent
    message = f"{name} must be an integer in [0, {upper}], or None"
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _refuse_type(message)
    if not isinstance(value, int) or not 0 <= value <= upper:
        raise _refuse(message)
    return value


def _require_midi_shapes(*fields: tuple[str, object, str]) -> None:
    """Check MIDI request shapes before any field's domain is narrowed."""
    for name, value, kind in fields:
        if kind == "integer":
            valid = not isinstance(value, bool) and isinstance(value, (numbers.Real, SupportsIndex))
            expected = "an integer"
        elif kind == "optional_index_integer":
            valid = value is None or (
                not isinstance(value, bool) and isinstance(value, (numbers.Real, SupportsIndex))
            )
            expected = "an integer or None"
        elif kind == "index":
            valid = isinstance(value, SupportsIndex)
            expected = "an integer"
        elif kind == "float_conversion":
            if isinstance(value, (str, bytes, bytearray, memoryview)):
                float(value)
                continue
            valid = isinstance(value, (SupportsFloat, SupportsIndex))
            expected = "a number"
        elif kind == "optional_integer":
            valid = value is None or (
                not isinstance(value, bool) and isinstance(value, (int, float))
            )
            expected = "an integer or None"
        elif kind == "number":
            valid = not isinstance(value, bool) and isinstance(value, SupportsFloat)
            expected = "a number"
        elif kind == "real":
            valid = not isinstance(value, bool) and isinstance(value, numbers.Real)
            expected = "a number"
        elif kind == "boolean":
            valid = isinstance(value, (bool, np.bool_))
            expected = "a bool"
        elif kind == "string":
            valid = isinstance(value, str)
            expected = "a string"
        elif kind == "bytes_like":
            valid = isinstance(value, (bytes, bytearray, memoryview))
            expected = "bytes, bytearray or memoryview"
        else:
            raise AssertionError(f"unknown MIDI shape kind: {kind}")
        if not valid:
            raise _refuse_type(f"{name} must be {expected}")


def _require_midi_cc_binding(binding: object, label: str) -> MidiCcBinding:
    """Check every field's representation before any binding domain is read."""
    if not isinstance(binding, MidiCcBinding):
        raise _refuse_type(f"{label} must be a MidiCcBinding")
    _require_midi_shapes(
        (f"{label}.cc_number", binding.cc_number, "integer"),
        (f"{label}.channel", binding.channel, "optional_integer"),
        (f"{label}.kind", binding.kind, "string"),
        (f"{label}.cc_lsb_number", binding.cc_lsb_number, "integer"),
        (f"{label}.selector_msb", binding.selector_msb, "integer"),
        (f"{label}.selector_lsb", binding.selector_lsb, "integer"),
        (f"{label}.consume", binding.consume, "boolean"),
        (f"{label}.param_id", binding.param_id, "index"),
        (f"{label}.min_value", binding.min_value, "float_conversion"),
        (f"{label}.max_value", binding.max_value, "float_conversion"),
    )
    return binding


def _require_midi_cc_bindings(bindings: object, label: str) -> tuple[MidiCcBinding, ...]:
    """Materialize a binding iterable and validate every record's shape first."""
    if not isinstance(bindings, Iterable):
        raise _refuse_type(f"{label} must be an iterable of MidiCcBinding")
    rows = iter(bindings)
    return tuple(_require_midi_cc_binding(row, f"{label}[{i}]") for i, row in enumerate(rows))


def _cc_binding_to_c(binding: MidiCcBinding, label: str = "binding") -> SonareMidiCcBinding:
    """Marshal a shape-checked MidiCcBinding into C under its caller's subject."""
    binding = _require_midi_cc_binding(binding, label)
    # Controller numbers and selector bytes are 7-bit MIDI data, narrower than their uint8 fields.
    return SonareMidiCcBinding(
        cc_number=_narrow_int(binding.cc_number, f"{label}.cc_number", 0, _MIDI_DATA_MAX),
        channel=_absent_or_in_range(binding.channel, f"{label}.channel", 15, 0xFF),
        kind=_token_ordinal(binding.kind, _MIDI_CC_BINDING_KINDS, f"{label}.kind"),
        cc_lsb_number=_narrow_int(
            binding.cc_lsb_number, f"{label}.cc_lsb_number", 0, _MIDI_DATA_MAX
        ),
        selector_msb=_narrow_int(binding.selector_msb, f"{label}.selector_msb", 0, _MIDI_DATA_MAX),
        selector_lsb=_narrow_int(binding.selector_lsb, f"{label}.selector_lsb", 0, _MIDI_DATA_MAX),
        consume=_bool_flag(binding.consume, f"{label}.consume"),
        reserved=0,
        param_id=binding.param_id,
        min_value=float(binding.min_value),
        max_value=float(binding.max_value),
    )


def _cc_binding_from_c(b: SonareMidiCcBinding) -> MidiCcBinding:
    kind = _ordinal_token(int(b.kind), _MIDI_CC_BINDING_KINDS, "MIDI CC binding kind")
    consume = int(b.consume)
    if consume not in (0, 1):
        raise _malformed_output(f"unknown MIDI CC consume value: {consume}")
    return MidiCcBinding(
        cc_number=int(b.cc_number),
        channel=None if int(b.channel) == 0xFF else int(b.channel),
        kind=cast(ProjectMidiCcBindingKind, kind),
        cc_lsb_number=int(b.cc_lsb_number),
        selector_msb=int(b.selector_msb),
        selector_lsb=int(b.selector_lsb),
        consume=bool(consume),
        param_id=int(b.param_id),
        min_value=float(b.min_value),
        max_value=float(b.max_value),
    )


def _marker_name_arg(name: str | None) -> str:
    """Spell a marker name for the C call, where ``""`` is its encoding of no name."""
    if name is None:
        return ""
    if name == "":
        raise _refuse("marker name must not be empty; omit it for an unnamed marker")
    return name


@dataclass(frozen=True)
class ProjectCompileResult:
    """Result of :meth:`Project.compile` and :meth:`Project.last_bounce_compile_result`."""

    diagnostics: tuple[ProjectDiagnostic, ...]
    """Every diagnostic the compile produced; empty when it was clean. An
    ``"error"`` entry means no timeline was produced."""
    timeline: ProjectTimeline | None = None
    """The compiled playback snapshot, owned by the caller; ``None`` unless
    :meth:`Project.compile` ran with ``with_timeline=True`` and compilation produced
    one."""


@dataclass(frozen=True)
class ProjectDeserializeResult:
    """Project plus warning diagnostics emitted by a successful JSON load."""

    project: Project
    """The loaded :class:`Project`."""
    diagnostics: str
    """Warning text from the load; empty when there were none."""


@dataclass(frozen=True, slots=True)
class ProjectLoopRecordingResult:
    """Result of :meth:`Project.add_loop_recording_takes`."""

    clip_id: int
    """Id of the clip that holds the takes."""
    take_count: int
    """Number of takes the capture was split into."""


@dataclass(frozen=True, slots=True)
class ProjectMidiClipResult:
    """Result of :meth:`Project.add_midi_clip`."""

    track_id: int
    """Id of the new MIDI track."""
    clip_id: int
    """Id of the new MIDI clip."""


@dataclass(frozen=True, slots=True, eq=False)
class ExternalSeparatedStemImportResult:
    """Result of :meth:`Project.import_external_stems`; entry ``i`` of each array is stem ``i``."""

    track_ids: np.ndarray
    """Ids of the tracks created, as ``uint32``."""
    clip_ids: np.ndarray
    """Ids of the clips created, as ``uint32``."""


@dataclass(frozen=True, slots=True)
class PartRigEntry:
    """A part-rig entry read back by :meth:`Project.part_rig`."""

    mode: PartRigMode
    """The :data:`PartRigMode` token."""
    inserts: tuple[dict[str, object], ...] | None
    """``{"processor", "params"}`` dicts for a ``"chain"`` entry; ``None`` otherwise."""


@_entry("project_abi_version")
def project_abi_version() -> int | None:
    """Return the project C ABI version of the loaded native library.

    ``None`` when libsonare was built without the arrangement subsystem;
    presence is ``capabilities().features.project``.
    """
    lib = _get_lib()
    return int(lib.sonare_project_abi_version()) or None


def _check_project_abi(lib: Any) -> None:
    abi = int(lib.sonare_project_abi_version())
    if abi != _EXPECTED_PROJECT_ABI_VERSION:
        raise SonareError(
            int(ErrorCode.ABI_MISMATCH),
            f"libsonare project ABI mismatch: native binary reports {abi}, "
            f"expected {_EXPECTED_PROJECT_ABI_VERSION}. The installed shared "
            "library is incompatible with this Python binding (0 = arrangement "
            "support not compiled in).",
        )


def _track_kind_value(kind: ProjectTrackKind, key: str) -> int:
    return _token_ordinal(kind, _TRACK_KINDS, key)


def _midi_event_packed(name: str, *args: float | ctypes._SimpleCData[int]) -> ProjectMidiEvent:
    """Call one packer and read its event back as a record.

    Each argument arrives already narrowed, because the widths are not uniform
    across the family (``uint8``, ``uint16``, ``uint32``, ``int32``), and this
    sees only ``*args``.
    """
    lib = _get_lib()
    event = SonareMidiEventPod()
    fn = getattr(lib, name)
    _check(fn(*args, ctypes.byref(event)))
    return _midi_event_from_c(event)


def _midi_event_from_c(event: SonareMidiEventPod) -> ProjectMidiEvent:
    sysex_size = int(event.sysex_size)
    if event.sysex:
        if sysex_size == 0:
            raise _malformed_output("MIDI event has a SysEx payload pointer with zero size")
        sysex = ctypes.string_at(event.sysex, sysex_size)
    elif sysex_size:
        raise _malformed_output("MIDI event has a SysEx size without a payload")
    else:
        sysex = None
    return ProjectMidiEvent(float(event.ppq), int(event.word0), int(event.word1), sysex)


def _require_midi_event(event: object, label: str) -> ProjectMidiEvent:
    """Check a MIDI event's record and field shapes before narrowing any row."""
    if not isinstance(event, ProjectMidiEvent):
        raise _refuse_type(f"{label} must be a ProjectMidiEvent")
    _require_midi_shapes(
        (f"{label}.ppq", event.ppq, "real"),
    )
    _require_midi_event_word_shape(event.word0, f"{label}.word0")
    _require_midi_event_word_shape(event.word1, f"{label}.word1")
    if event.sysex is not None and not isinstance(event.sysex, bytes):
        raise _refuse_type(f"{label}.sysex must be bytes or None")
    return event


def _require_midi_events(events: object, name: str) -> tuple[ProjectMidiEvent, ...]:
    """Validate an event sequence and all event shapes before domain conversion."""
    if isinstance(events, (str, bytes)) or not isinstance(events, Sequence):
        raise _refuse_type(f"{name} must be a sequence of ProjectMidiEvent")
    return tuple(_require_midi_event(event, f"{name}[{i}]") for i, event in enumerate(events))


def _pack_midi_events(
    rows: Sequence[ProjectMidiEvent], name: str
) -> tuple[ctypes.Array[SonareMidiEventPod], int]:
    """Narrow a shape-checked event sequence into its C POD array."""
    c_events = (SonareMidiEventPod * len(rows))()
    for i, event in enumerate(rows):
        c_events[i] = _midi_event_to_c(event, f"{name}[{i}]")
    return c_events, len(rows)


def _midi_event_to_c(event: ProjectMidiEvent, label: str) -> SonareMidiEventPod:
    """Pack a shape-checked project MIDI event for the C ABI."""
    sysex_pointer = None
    sysex_size = 0
    if event.sysex is not None:
        # c_char_p keeps the immutable bytes alive, including an empty bytes
        # object's non-null address, so C can distinguish it from absence.
        sysex_pointer = ctypes.cast(ctypes.c_char_p(event.sysex), ctypes.POINTER(ctypes.c_uint8))
        sysex_size = len(event.sysex)
    return SonareMidiEventPod(
        ppq=_validate_midi_event_ppq(event.ppq, f"{label}.ppq"),
        word0=_validate_midi_event_word(event.word0, f"{label}.word0"),
        word1=_validate_midi_event_word(event.word1, f"{label}.word1"),
        sysex=sysex_pointer,
        sysex_size=sysex_size,
    )


def _midi_events_to_c(events: object, name: str) -> tuple[ctypes.Array[SonareMidiEventPod], int]:
    """Check every event shape and pack the sequence into C PODs."""
    return _pack_midi_events(_require_midi_events(events, name), name)


def _require_midi_event_word_shape(value: object, label: str) -> None:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise _refuse_type(f"{label} must be an integer in [0, 4294967295]")


def _validate_midi_event_word(value: object, label: str) -> int:
    _require_midi_event_word_shape(value, label)
    word = float(cast(SupportsFloat, value))
    if not math.isfinite(word) or not word.is_integer() or word < 0.0 or word > _UINT32_MAX:
        raise _refuse(f"{label} must be an integer in [0, 4294967295]")
    return int(word)


def _validate_midi_event_ppq(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise _refuse_type(f"{label} must be a number")
    ppq = float(cast(SupportsFloat, value))
    if not (math.isfinite(ppq) and 0.0 <= ppq <= _MAX_PUBLIC_PPQ):
        raise _refuse(f"{label} must be finite and in [0, 1e12]")
    return ppq
