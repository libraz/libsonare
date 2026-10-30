"""Headless arrangement / DAW project wrapper for libsonare.

This wraps the curated ``sonare_project_*`` C ABI (``include/sonare/sonare_c_project.h``):
an opaque :class:`SonareProject` handle over the arrangement control plane
(Project / EditHistory / EditCompiler / serializer / MIR bridges / SMF).

Every entry point is control-thread-only and performs no file or device I/O:
project JSON and SMF bytes are exchanged through in-memory buffers, so the host
owns storage. Heap buffers returned by the C layer are freed here (no leaks).

Availability: the symbols are always exported, but when libsonare was built
without ``SONARE_WITH_ARRANGEMENT`` every call returns
``SONARE_ERROR_NOT_SUPPORTED`` and :func:`project_abi_version` returns 0. The
:class:`Project` constructor checks the ABI version and raises a clear
``RuntimeError`` in that case.
"""

from __future__ import annotations

import ctypes
import math
import numbers
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal, SupportsFloat, TypeAlias, cast

if TYPE_CHECKING:
    from ._project import Project, ProjectTimeline

from ._errors import ErrorCode, SonareError, _not_supported
from ._project_instruments import (
    _GS_EFX_REALIZATIONS as _GS_EFX_REALIZATIONS,
)
from ._project_instruments import (
    _SYNTH_WAVEFORM_NAMES as _SYNTH_WAVEFORM_NAMES,
)
from ._project_instruments import (
    SOURCE_BACKEND_SF2 as SOURCE_BACKEND_SF2,
)
from ._project_instruments import (
    SOURCE_BACKEND_SYNTH as SOURCE_BACKEND_SYNTH,
)
from ._project_instruments import (
    SYNTH_WAVEFORM_SAW as SYNTH_WAVEFORM_SAW,
)
from ._project_instruments import (
    SYNTH_WAVEFORM_SINE as SYNTH_WAVEFORM_SINE,
)
from ._project_instruments import (
    SYNTH_WAVEFORM_SQUARE as SYNTH_WAVEFORM_SQUARE,
)
from ._project_instruments import (
    SYNTH_WAVEFORM_TRIANGLE as SYNTH_WAVEFORM_TRIANGLE,
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
    SampleBank as SampleBank,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    _UINT32_MAX,
    SonareAutomationLaneDesc,
    SonareAutomationPoint,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    SonareValueError,
    _check,
    _curve_value,
    _get_lib,
    _resolve_enum,
    _to_c_size_t,
    _utf8_arg,
)

# Mirrors SONARE_ERROR_INVALID_STATE in sonare_c.h. The pure MIDI conversion
# helpers return this when no result is produced (e.g. learn found no binding);
# the Python wrappers translate it to ``None`` rather than raising.
SONARE_ERROR_INVALID_STATE = 7

# Must match SONARE_PROJECT_ABI_VERSION (include/sonare/sonare_c_project.h) and the other
# bindings' expected project ABI constant. A mismatch means the loaded native
# binary lays out the flat project PODs differently than this wrapper expects,
# or the arrangement subsystem was compiled out (runtime version 0).
EXPECTED_PROJECT_ABI_VERSION = 2

# Track kind ordinals (mirror SonareProjectTrackKind).
TRACK_AUDIO = 0
TRACK_MIDI = 1
TRACK_AUX = 2

_TRACK_KIND_NAMES = {
    "audio": TRACK_AUDIO,
    "midi": TRACK_MIDI,
    "aux": TRACK_AUX,
}

# Clip fade-curve ordinals (mirror SonareProjectFadeCurve).
FADE_CURVE_LINEAR = 0
FADE_CURVE_EQUAL_POWER = 1
FADE_CURVE_EXPONENTIAL = 2
FADE_CURVE_LOGARITHMIC = 3

_FADE_CURVE_NAMES = {
    "linear": FADE_CURVE_LINEAR,
    "equal-power": FADE_CURVE_EQUAL_POWER,
    "equal_power": FADE_CURVE_EQUAL_POWER,
    "equalpower": FADE_CURVE_EQUAL_POWER,
    "exponential": FADE_CURVE_EXPONENTIAL,
    "exp": FADE_CURVE_EXPONENTIAL,
    "logarithmic": FADE_CURVE_LOGARITHMIC,
    "log": FADE_CURVE_LOGARITHMIC,
}

# Clip loop-mode ordinals (mirror SonareProjectLoopMode).
LOOP_MODE_OFF = 0
LOOP_MODE_LOOP = 1

_LOOP_MODE_NAMES = {
    "off": LOOP_MODE_OFF,
    "none": LOOP_MODE_OFF,
    "loop": LOOP_MODE_LOOP,
    "on": LOOP_MODE_LOOP,
}

# Automation target-kind ordinals (mirror SonareAutomationTargetKind).  The
# optional Python value is deliberately kept separate from the legacy C
# descriptor: omission selects the legacy ABI call, while an explicit opaque
# value (0) selects the extended call and is therefore observable by callers
# that instrument the FFI boundary.
AUTOMATION_TARGET_OPAQUE = 0
AUTOMATION_TARGET_TRACK_FADER_DB = 1
AUTOMATION_TARGET_TRACK_PAN = 2

PROJECT_AUTOMATION_TARGET_OPAQUE = AUTOMATION_TARGET_OPAQUE
PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB = AUTOMATION_TARGET_TRACK_FADER_DB
PROJECT_AUTOMATION_TARGET_TRACK_PAN = AUTOMATION_TARGET_TRACK_PAN

ProjectAutomationTargetKind: TypeAlias = Literal["opaque", "track-fader-db", "track-pan", 0, 1, 2]

_AUTOMATION_TARGET_KIND_NAMES = {
    "opaque": AUTOMATION_TARGET_OPAQUE,
    "track-fader-db": AUTOMATION_TARGET_TRACK_FADER_DB,
    "track_fader_db": AUTOMATION_TARGET_TRACK_FADER_DB,
    "track-pan": AUTOMATION_TARGET_TRACK_PAN,
    "track_pan": AUTOMATION_TARGET_TRACK_PAN,
}

# A private sentinel lets the facade distinguish an omitted optional kind from
# an explicit ``None`` (which is not a valid target-kind value).
_AUTOMATION_TARGET_KIND_UNSET = cast(ProjectAutomationTargetKind | None, object())


def _automation_target_kind_value(value: object) -> int:
    """Resolve and validate one optional automation target-kind value."""
    if isinstance(value, str):
        key = value.lower()
        if key in _AUTOMATION_TARGET_KIND_NAMES:
            return _AUTOMATION_TARGET_KIND_NAMES[key]
        raise SonareValueError(
            "unknown automation target kind; expected opaque, track-fader-db, or track-pan"
        )
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise TypeError("automation target kind must be an integer in [0, 2]")
    ordinal = float(cast(SupportsFloat, value))
    if not math.isfinite(ordinal) or not ordinal.is_integer() or ordinal < 0.0 or ordinal > 2.0:
        raise SonareValueError("automation target kind must be an integer in [0, 2]")
    return int(ordinal)


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


def _automation_lane_args(
    target_param_id: object,
    points: (
        Sequence[tuple[float, float, int | str]]
        | Sequence[Sequence[float]]
        | Sequence[Mapping[str, object]]
        | Mapping[str, object]
        | None
    ),
    target_kind: object,
) -> tuple[
    object,
    Sequence[tuple[float, float, int | str]]
    | Sequence[Sequence[float]]
    | Sequence[Mapping[str, object]]
    | None,
    object,
]:
    """Accept the historical positional form and a descriptor mapping.

    The mapping form mirrors the Node/WASM descriptor keys while preserving
    the existing Python ``(target_param_id, points)`` call shape.
    """
    if isinstance(target_param_id, Mapping):
        descriptor = target_param_id
        resolved_target = (
            descriptor["target_param_id"]
            if "target_param_id" in descriptor
            else descriptor.get("targetParamId")
        )
    elif isinstance(points, Mapping):
        descriptor = points
        resolved_target = target_param_id
    else:
        return target_param_id, points, target_kind
    if "target_param_id" in descriptor:
        resolved_target = descriptor["target_param_id"]
    elif "targetParamId" in descriptor:
        resolved_target = descriptor["targetParamId"]
    resolved_points = descriptor.get("points", None if isinstance(points, Mapping) else points)
    resolved_kind = target_kind
    if target_kind is _AUTOMATION_TARGET_KIND_UNSET:
        if "target_kind" in descriptor:
            resolved_kind = descriptor["target_kind"]
        elif "targetKind" in descriptor:
            resolved_kind = descriptor["targetKind"]
    return (
        resolved_target,
        cast(
            Sequence[tuple[float, float, int | str]]
            | Sequence[Sequence[float]]
            | Sequence[Mapping[str, object]]
            | None,
            resolved_points,
        ),
        resolved_kind,
    )


def _fade_curve_value(curve: str | int) -> int:
    if isinstance(curve, str):
        out = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_fade_curve_from_name(
                _utf8_arg(curve, "curve"), ctypes.byref(out)
            )
        )
        return int(out.value)
    return _resolve_enum(curve, _FADE_CURVE_NAMES, "fade curve")


def _loop_mode_value(mode: str | int) -> int:
    return _resolve_enum(mode, _LOOP_MODE_NAMES, "loop mode")


def _automation_lane_desc(
    target_param_id: object,
    points: (
        Sequence[tuple[float, float, int | str]]
        | Sequence[Sequence[float]]
        | Sequence[Mapping[str, object]]
        | None
    ),
) -> tuple[SonareAutomationLaneDesc, object]:
    """Marshal ``(ppq, value, curve)`` breakpoints into a SonareAutomationLaneDesc.

    Returns ``(desc, backing)``; the caller must keep ``backing`` (the C point
    array) alive for the duration of the native call.
    """
    target_id = _automation_target_param_id_value(target_param_id)
    rows = list(points) if points is not None else []
    count = len(rows)
    c_points = (SonareAutomationPoint * count)()
    for i, pt in enumerate(rows):
        seq: tuple[object, ...]
        if isinstance(pt, Mapping):
            ppq_value = pt.get("ppq")
            value_value = pt.get("value")
            curve_value = pt.get("curve", pt.get("curve_to_next", pt.get("curveToNext", 0)))
            seq = (ppq_value, value_value, curve_value)
        else:
            seq = tuple(pt)
        if len(seq) < 2:
            raise SonareValueError(f"points[{i}] must contain at least (ppq, value)")
        ppq = float(cast(SupportsFloat, seq[0]))
        value = float(cast(SupportsFloat, seq[1]))
        curve = _curve_value(cast(int | str, seq[2])) if len(seq) >= 3 else 0
        if not math.isfinite(ppq):
            raise SonareValueError(f"points[{i}].ppq must be a finite number")
        if not math.isfinite(value):
            raise SonareValueError(f"points[{i}].value must be a finite number")
        c_points[i].ppq = ppq
        c_points[i].value = value
        c_points[i].curve_to_next = int(curve)
    desc = SonareAutomationLaneDesc(
        target_param_id=target_id,
        points=c_points,
        point_count=_to_c_size_t(count, "count"),
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
    """One compile diagnostic surfaced by :meth:`Project.compile`."""

    code: int
    severity: int
    target_id: int
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

    ``kind`` is a :data:`MIDI_CC_*` ordinal (0=7-bit CC, 1=14-bit CC,
    2=RPN, 3=NRPN). For a 14-bit binding ``cc_lsb_number`` carries the LSB
    controller; for RPN/NRPN ``selector_msb`` / ``selector_lsb`` carry the
    parameter selector. ``param_id`` is the bound automation parameter; the
    unit-interval automation range maps onto ``[min_value, max_value]``.
    """

    cc_number: int
    channel: int
    kind: int
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


# MIDI CC binding kind ordinals (mirror SonareMidiCcBindingKind).
MIDI_CC_CONTROL_CHANGE_7 = 0
MIDI_CC_CONTROL_CHANGE_14 = 1
MIDI_CC_RPN = 2
MIDI_CC_NRPN = 3


def _cc_binding_to_c(binding: MidiCcBinding | Mapping[str, object]) -> SonareMidiCcBinding:
    """Marshal a :class:`MidiCcBinding` (or a Mapping with the same keys) into C.

    ``channel`` defaults to ``0xFF`` (any channel) when unset, matching the
    native binding's "wildcard channel" sentinel.
    """
    if isinstance(binding, Mapping):

        def _get(key: str, default: object) -> object:
            return binding.get(key, default)
    else:

        def _get(key: str, default: object) -> object:
            return getattr(binding, key, default)

    # Passed unconverted so the struct's own narrowing sees each caller value;
    # int() would truncate a fraction past it and land on a neighbouring CC.
    return SonareMidiCcBinding(
        cc_number=_get("cc_number", 0),
        channel=_get("channel", 0xFF),
        kind=_get("kind", MIDI_CC_CONTROL_CHANGE_7),
        cc_lsb_number=_get("cc_lsb_number", 0),
        selector_msb=_get("selector_msb", 0),
        selector_lsb=_get("selector_lsb", 0),
        reserved=0,
        param_id=_get("param_id", 0),
        min_value=float(cast(float, _get("min_value", 0.0))),
        max_value=float(cast(float, _get("max_value", 1.0))),
    )


def _cc_binding_from_c(b: SonareMidiCcBinding) -> MidiCcBinding:
    return MidiCcBinding(
        cc_number=int(b.cc_number),
        channel=int(b.channel),
        kind=int(b.kind),
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


def project_abi_version() -> int:
    """Return the runtime project ABI version of the loaded native library.

    Equals :data:`EXPECTED_PROJECT_ABI_VERSION` when the arrangement subsystem
    is compiled in, ``0`` when libsonare was built without it.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_project_abi_version"):
        return 0
    return int(lib.sonare_project_abi_version())


def _check_project_abi(lib: Any) -> None:
    if not hasattr(lib, "sonare_project_abi_version"):
        raise _not_supported("libsonare was built without arrangement support")
    abi = int(lib.sonare_project_abi_version())
    if abi != EXPECTED_PROJECT_ABI_VERSION:
        raise SonareError(
            int(ErrorCode.ABI_MISMATCH),
            f"libsonare project ABI mismatch: native binary reports {abi}, "
            f"expected {EXPECTED_PROJECT_ABI_VERSION}. The installed shared "
            "library is incompatible with this Python binding (0 = arrangement "
            "support not compiled in).",
        )


def _track_kind_value(kind: str | int) -> int:
    return _resolve_enum(kind, _TRACK_KIND_NAMES, "track kind")


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
