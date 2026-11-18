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
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal, SupportsFloat, TypeAlias, cast, get_args

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
    SOURCE_BACKEND_SF2 as SOURCE_BACKEND_SF2,
)
from ._project_instruments import (
    SOURCE_BACKEND_SYNTH as SOURCE_BACKEND_SYNTH,
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
    Sf2InstrumentConfig as Sf2InstrumentConfig,
)
from ._project_instruments import (
    Sf2ProgramStatus as Sf2ProgramStatus,
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
    SonareValueError,
    _check,
    _get_lib,
    _require_export,
    _require_instance,
    _to_c_size_t,
    _token_ordinal,
    _utf8_arg,
)
from ._types_engine import AutomationPoint

# Must match SONARE_PROJECT_ABI_VERSION (include/sonare/sonare_c_project.h) and the other
# bindings' expected project ABI constant. A mismatch means the loaded native
# binary lays out the flat project PODs differently than this wrapper expects,
# or the arrangement subsystem was compiled out (runtime version 0).
_EXPECTED_PROJECT_ABI_VERSION = 2

# Track kind; a token's index is its SonareProjectTrackKind ordinal.
ProjectTrackKind: TypeAlias = Literal["audio", "midi", "aux"]
"""What a track carries: audio clips, MIDI clips, or an auxiliary bus."""
_TRACK_KINDS: tuple[str, ...] = get_args(ProjectTrackKind)

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

# Automation target kind; a token's index is its SonareAutomationTargetKind ordinal.
# None selects the legacy C call, while an explicit "opaque" selects the
# extended call and is therefore observable at the FFI boundary.
ProjectAutomationTargetKind: TypeAlias = Literal["opaque", "trackFaderDb", "trackPan"]
"""What an automation lane drives: an opaque parameter, a track fader or a track pan."""
_AUTOMATION_TARGET_KINDS: tuple[str, ...] = get_args(ProjectAutomationTargetKind)


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


def _automation_target_kind_value(value: object) -> int:
    return _token_ordinal(value, _AUTOMATION_TARGET_KINDS, "automation target kind")


def _automation_target_param_id_value(value: object) -> int:
    """Validate a non-zero uint32 automation target parameter id."""
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise SonareValueError("target_param_id must be a finite integer in [1, 4294967295]")
    target = float(cast(SupportsFloat, value))
    if not math.isfinite(target) or not target.is_integer() or target < 1.0 or target > _UINT32_MAX:
        if math.isfinite(target) and target == 0.0:
            raise SonareValueError("target_param_id must be non-zero")
        raise SonareValueError("target_param_id must be a finite integer in [1, 4294967295]")
    return int(target)


def _fade_curve_value(curve: ProjectFadeCurve) -> int:
    return _token_ordinal(curve, _FADE_CURVES, "fade curve")


def _loop_mode_value(mode: ProjectLoopMode) -> int:
    return _token_ordinal(mode, _LOOP_MODES, "loop mode")


def _automation_lane_desc(
    target_param_id: object,
    points: Sequence[AutomationPoint] | None,
) -> tuple[SonareAutomationLaneDesc, object]:
    """Marshal :class:`AutomationPoint` records into a SonareAutomationLaneDesc.

    Returns ``(desc, backing)``; the caller must keep ``backing`` (the C point
    array) alive for the duration of the native call.
    """
    target_id = _automation_target_param_id_value(target_param_id)
    c_points = _automation_points_to_c(points if points is not None else [])
    desc = SonareAutomationLaneDesc(
        target_param_id=target_id,
        points=c_points,
        point_count=_to_c_size_t(len(c_points), "count"),
    )
    return desc, c_points


@dataclass(frozen=True)
class AssistSidecar:
    """One opaque assist sidecar stored on a :class:`Project`.

    ``payload`` is module-owned opaque bytes; ``module_id`` identifies the
    producing module. ``schema_version`` / ``target_track_id`` and the
    ``region_*_ppq`` scope mirror the C ``SonareProjectAssistSidecar``.
    """

    module_id: str
    schema_version: int
    target_track_id: int
    region_start_ppq: float
    region_end_ppq: float
    payload: bytes


@dataclass(frozen=True)
class ProjectDiagnostic:
    """One compile diagnostic surfaced by :meth:`Project.compile`.

    ``target_id`` is the affected clip, track or source id, or ``None`` when the
    diagnostic concerns no single one.
    """

    code: int
    severity: int
    target_id: int | None
    message: str = ""


@dataclass(frozen=True)
class NotePairValidation:
    """Result of :meth:`Project.validate_midi_notes`.

    ``ok`` is True when every note-on in the clip has a matching note-off;
    otherwise ``unmatched_note_ons`` / ``unmatched_note_offs`` count the
    hanging (unpaired) events.
    """

    ok: bool
    unmatched_note_ons: int
    unmatched_note_offs: int


@dataclass(frozen=True)
class MidiCcBinding:
    """A MIDI CC <-> automation-parameter binding (see :meth:`Project.midi_cc_learn`).

    ``kind`` is a :data:`ProjectMidiCcBindingKind` token. For a 14-bit
    binding ``cc_lsb_number`` carries the LSB controller; for RPN/NRPN
    ``selector_msb`` / ``selector_lsb`` carry the parameter selector.
    ``param_id`` is the bound automation parameter; the unit-interval
    automation range maps onto ``[min_value, max_value]``.
    ``channel`` is 0..15, or ``None`` for a binding that answers any channel.
    """

    cc_number: int
    channel: int | None
    kind: ProjectMidiCcBindingKind
    cc_lsb_number: int
    selector_msb: int
    selector_lsb: int
    param_id: int
    min_value: float
    max_value: float


@dataclass(frozen=True)
class MidiRouteResult:
    """Result of :meth:`Project.midi_route_events`.

    ``events`` is the filtered / remapped event stream (``(ppq, data0, data1)``
    tuples). ``overflowed`` is True when the router or caller capacity dropped
    events; ``overflow_count`` is how many were dropped.
    """

    events: list[tuple[float, int, int]]
    overflowed: bool
    overflow_count: int


# The C ABI's "no pitch class" code; absence on the Python side.
_NO_PITCH_CLASS = 255


def _absent_or_in_range(value: object, name: str, upper: int, absent: int) -> int:
    """Map ``None`` to the C ABI's ``absent`` code; refuse anything outside 0..``upper`` by name."""
    if value is None:
        return absent
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= upper:
        raise SonareValueError(f"{name} {value!r} must be in 0..{upper}, or None when absent")
    return value


def _cc_binding_to_c(binding: MidiCcBinding) -> SonareMidiCcBinding:
    """Marshal a :class:`MidiCcBinding` into C; any other value is a ``TypeError``.

    A ``None`` ``channel`` is the any-channel binding; the C ABI's ``0xFF``
    spelling of it is refused as a value.
    """
    _require_instance(binding, MidiCcBinding, "binding")
    # Passed unconverted so the struct's own narrowing sees each caller value;
    # int() would truncate a fraction past it and land on a neighbouring CC.
    return SonareMidiCcBinding(
        cc_number=binding.cc_number,
        channel=_absent_or_in_range(binding.channel, "channel", 15, 0xFF),
        kind=_token_ordinal(binding.kind, _MIDI_CC_BINDING_KINDS, "MIDI CC binding kind"),
        cc_lsb_number=binding.cc_lsb_number,
        selector_msb=binding.selector_msb,
        selector_lsb=binding.selector_lsb,
        reserved=0,
        param_id=binding.param_id,
        min_value=float(binding.min_value),
        max_value=float(binding.max_value),
    )


def _cc_binding_from_c(b: SonareMidiCcBinding) -> MidiCcBinding:
    return MidiCcBinding(
        cc_number=int(b.cc_number),
        channel=None if int(b.channel) == 0xFF else int(b.channel),
        kind=cast(ProjectMidiCcBindingKind, _MIDI_CC_BINDING_KINDS[int(b.kind)]),
        cc_lsb_number=int(b.cc_lsb_number),
        selector_msb=int(b.selector_msb),
        selector_lsb=int(b.selector_lsb),
        param_id=int(b.param_id),
        min_value=float(b.min_value),
        max_value=float(b.max_value),
    )


def _marker_name_bytes(name: str | None) -> bytes:
    """UTF-8-encode a marker name for the fixed 64-byte C ``name`` field.

    Truncates on a codepoint boundary so a split multi-byte sequence never
    leaves invalid UTF-8 (mirrors the engine binding's ``_fixed_bytes``).
    """
    if not name:
        return b""
    return _utf8_arg(name, "name")[:63].decode("utf-8", "ignore").encode("utf-8")


@dataclass(frozen=True)
class ProjectCompileResult:
    """Structured compile result matching the C/Node/WASM project surface.

    Iteration preserves the legacy ``has_timeline, messages = project.compile()``
    pattern while exposing structured diagnostics.
    """

    has_timeline: bool
    messages: str
    diagnostics: tuple[ProjectDiagnostic, ...]
    timeline: ProjectTimeline | None = None

    @property
    def diagnostic_count(self) -> int:
        return len(self.diagnostics)

    def __iter__(self) -> Iterator[object]:
        yield self.has_timeline
        yield self.messages


@dataclass(frozen=True)
class ProjectDeserializeResult:
    """Project plus warning diagnostics emitted by a successful JSON load."""

    project: Project
    diagnostics: str


def project_abi_version() -> int | None:
    """Return the project C ABI version of the loaded native library.

    ``None`` when libsonare was built without the arrangement subsystem;
    presence is ``capabilities().features.project``.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_project_abi_version"):
        return None
    return int(lib.sonare_project_abi_version()) or None


def _check_project_abi(lib: Any) -> None:
    _require_export(
        lib, "sonare_project_abi_version", "libsonare was built without arrangement support"
    )
    abi = int(lib.sonare_project_abi_version())
    if abi != _EXPECTED_PROJECT_ABI_VERSION:
        raise SonareError(
            int(ErrorCode.ABI_MISMATCH),
            f"libsonare project ABI mismatch: native binary reports {abi}, "
            f"expected {_EXPECTED_PROJECT_ABI_VERSION}. The installed shared "
            "library is incompatible with this Python binding (0 = arrangement "
            "support not compiled in).",
        )


def _track_kind_value(kind: ProjectTrackKind) -> int:
    return _token_ordinal(kind, _TRACK_KINDS, "track kind")


def _midi_event_tuple(name: str, *args: float | ctypes._SimpleCData[int]) -> tuple[float, int, int]:
    """Call one packer and read its event back as a tuple.

    Each argument arrives already narrowed, because the widths are not uniform
    across the family (``uint8``, ``uint16``, ``uint32``, ``int32``), and this
    sees only ``*args``.
    """
    lib = _get_lib()
    event = SonareMidiEventPod()
    fn = getattr(lib, name)
    _check(fn(*args, ctypes.byref(event)))
    return (float(event.ppq), int(event.data0), int(event.data1))


def _validate_midi_event_word(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise TypeError(f"{label} must be an integer in [0, 4294967295]")
    word = float(cast(SupportsFloat, value))
    if not math.isfinite(word) or not word.is_integer() or word < 0.0 or word > _UINT32_MAX:
        raise SonareValueError(f"{label} must be an integer in [0, 4294967295]")
    return int(word)


def _validate_midi_event_ppq(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise TypeError(f"{label} must be a non-negative finite number")
    ppq = float(cast(SupportsFloat, value))
    if not math.isfinite(ppq) or ppq < 0.0:
        raise SonareValueError(f"{label} must be a non-negative finite number")
    return ppq
