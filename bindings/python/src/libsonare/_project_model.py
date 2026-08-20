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
from typing import TYPE_CHECKING, Any, Literal, Protocol, SupportsFloat, TypeAlias, cast

import numpy as np

if TYPE_CHECKING:
    from ._project import Project

from ._ffi_types_mastering_project import (
    SONARE_SYNTH_FIELD_AMP_ATTACK_MS,
    SONARE_SYNTH_FIELD_AMP_DECAY_MS,
    SONARE_SYNTH_FIELD_AMP_RELEASE_MS,
    SONARE_SYNTH_FIELD_AMP_SUSTAIN,
    SONARE_SYNTH_FIELD_BODY_MIX,
    SONARE_SYNTH_FIELD_BUS_DRIVE,
    SONARE_SYNTH_FIELD_CUTOFF_HZ,
    SONARE_SYNTH_FIELD_DETUNE_CENTS,
    SONARE_SYNTH_FIELD_DRIFT_CENTS,
    SONARE_SYNTH_FIELD_DRIVE,
    SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
    SONARE_SYNTH_FIELD_FILTER_ATTACK_MS,
    SONARE_SYNTH_FIELD_FILTER_DECAY_MS,
    SONARE_SYNTH_FIELD_FILTER_RELEASE_MS,
    SONARE_SYNTH_FIELD_FILTER_SUSTAIN,
    SONARE_SYNTH_FIELD_GAIN,
    SONARE_SYNTH_FIELD_GLIDE_MS,
    SONARE_SYNTH_FIELD_KEY_TRACK,
    SONARE_SYNTH_FIELD_LFO2_RATE_HZ,
    SONARE_SYNTH_FIELD_LFO_RATE_HZ,
    SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS,
    SONARE_SYNTH_FIELD_MOD_ROUTINGS,
    SONARE_SYNTH_FIELD_POLYPHONY,
    SONARE_SYNTH_FIELD_RESONANCE_Q,
    SONARE_SYNTH_FIELD_STEREO_SPREAD,
    SONARE_SYNTH_FIELD_UNISON,
    SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
    SONARE_SYNTH_PATCH_MOD_ROUTINGS,
    SONARE_SYNTH_PRESET_NAME_MAX,
)
from ._project_synth import (
    _SYNTH_BODY_TYPES,
    _SYNTH_ENGINE_MODES,
    _SYNTH_FILTER_MODELS,
    _SYNTH_FILTER_OUTPUTS,
    _SYNTH_MOD_DESTINATIONS,
    _SYNTH_MOD_SOURCES,
    _SYNTH_OSC_WAVEFORMS,
    _strip_va_prefix,
    _synth_enum_name,
    _synth_enum_value,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareAutomationLaneDesc,
    SonareAutomationPoint,
    SonareBuiltinSynthConfig,
    SonareInstrumentCallbacks,
    SonareInstrumentOnEventCallback,
    SonareInstrumentPrepareCallback,
    SonareInstrumentRenderCallback,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    SonareSf2InstrumentConfig,
    SonareSynthModRouting,
    SonareSynthPatch,
    SonareValueError,
    _check,
    _curve_value,
    _get_lib,
    _resolve_enum,
)

# Mirrors SONARE_ERROR_INVALID_STATE in sonare_c.h. The pure MIDI conversion
# helpers return this when no result is produced (e.g. learn found no binding);
# the Python wrappers translate it to ``None`` rather than raising.
SONARE_ERROR_INVALID_STATE = 7

# Built-in synth waveform ordinals (mirror SonareSynthWaveform).
SYNTH_WAVEFORM_SINE = 0
SYNTH_WAVEFORM_SAW = 1
SYNTH_WAVEFORM_SQUARE = 2
SYNTH_WAVEFORM_TRIANGLE = 3

_SYNTH_WAVEFORM_NAMES = {
    "sine": SYNTH_WAVEFORM_SINE,
    "saw": SYNTH_WAVEFORM_SAW,
    "sawtooth": SYNTH_WAVEFORM_SAW,
    "square": SYNTH_WAVEFORM_SQUARE,
    "triangle": SYNTH_WAVEFORM_TRIANGLE,
}

# Must match SONARE_PROJECT_ABI_VERSION (include/sonare/sonare_c_project.h) and the other
# bindings' expected project ABI constant. A mismatch means the loaded native
# binary lays out the flat project PODs differently than this wrapper expects,
# or the arrangement subsystem was compiled out (runtime version 0).
EXPECTED_PROJECT_ABI_VERSION = 1

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
        raise SonareValueError("automation target kind must be an integer in [0, 2]")
    ordinal = float(cast(SupportsFloat, value))
    if not math.isfinite(ordinal) or not ordinal.is_integer() or ordinal < 0.0 or ordinal > 2.0:
        raise SonareValueError("automation target kind must be an integer in [0, 2]")
    return int(ordinal)


def _automation_target_param_id_value(value: object) -> int:
    """Validate a non-zero uint32 automation target parameter id."""
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise SonareValueError("target_param_id must be a finite integer in [1, 4294967295]")
    target = float(cast(SupportsFloat, value))
    if not math.isfinite(target) or not target.is_integer() or target < 1.0 or target > 0xFFFFFFFF:
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
            _get_lib().sonare_project_fade_curve_from_name(curve.encode("utf-8"), ctypes.byref(out))
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
        point_count=ctypes.c_size_t(count),
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

    return SonareMidiCcBinding(
        cc_number=int(cast(int, _get("cc_number", 0))) & 0xFF,
        channel=int(cast(int, _get("channel", 0xFF))) & 0xFF,
        kind=int(cast(int, _get("kind", MIDI_CC_CONTROL_CHANGE_7))) & 0xFF,
        cc_lsb_number=int(cast(int, _get("cc_lsb_number", 0))) & 0xFF,
        selector_msb=int(cast(int, _get("selector_msb", 0))) & 0xFF,
        selector_lsb=int(cast(int, _get("selector_lsb", 0))) & 0xFF,
        reserved=0,
        param_id=int(cast(int, _get("param_id", 0))) & 0xFFFFFFFF,
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


@dataclass(frozen=True)
class BuiltinSynthConfig:
    """Patch for the built-in polyphonic oscillator synth (see
    :meth:`Project.bounce_with_builtin_instrument`).

    Every numeric field uses "0 (or non-positive) => sensible default", so a
    default-constructed config is the default sine patch; override only what you
    need. ``waveform`` may be an ordinal (0=sine, 1=saw, 2=square, 3=triangle)
    or a name (``"sine"`` / ``"saw"`` / ``"square"`` / ``"triangle"``).
    """

    waveform: str | int = SYNTH_WAVEFORM_SINE
    gain: float = 0.0
    attack_ms: float = 0.0
    decay_ms: float = 0.0
    sustain: float = 0.0
    release_ms: float = 0.0
    polyphony: int = 0

    def _to_c(self) -> SonareBuiltinSynthConfig:
        return SonareBuiltinSynthConfig(
            waveform=_synth_waveform_value(self.waveform),
            gain=float(self.gain),
            attack_ms=float(self.attack_ms),
            decay_ms=float(self.decay_ms),
            sustain=float(self.sustain),
            release_ms=float(self.release_ms),
            polyphony=int(self.polyphony),
        )


# Source backend ordinals (mirror SonareSourceBackend).
SOURCE_BACKEND_SYNTH = 0
SOURCE_BACKEND_SF2 = 1


@dataclass(frozen=True)
class Sf2InstrumentConfig:
    """Patch for the SoundFont (SF2) player (see
    :meth:`Project.bounce_with_sf2_instrument`).

    Every field uses "0 (or non-positive) => sensible default" (gain 0.5,
    polyphony 48). ``prefer_model_for_modeled_families`` defaults to false,
    retaining the established SoundFont-first behavior.
    """

    gain: float = 0.0
    polyphony: int = 0
    prefer_model_for_modeled_families: bool = False

    def _to_c(self) -> SonareSf2InstrumentConfig:
        return SonareSf2InstrumentConfig(
            struct_version=2,
            gain=float(self.gain),
            polyphony=int(self.polyphony),
            prefer_model_for_modeled_families=int(self.prefer_model_for_modeled_families),
        )


@dataclass(frozen=True)
class Sf2ProgramStatus:
    """One :meth:`Project.soundfont_manifest` entry: a (channel, bank, program)
    combination the arrangement plays, with the backend it resolves to
    (:data:`SOURCE_BACKEND_SF2` or :data:`SOURCE_BACKEND_SYNTH`) and the
    resolved SF2 preset name (GS fallback included; empty for the synth
    fallback)."""

    channel: int
    bank: int
    program: int
    backend: int
    preset_name: str


def _marker_name_bytes(name: str | None) -> bytes:
    """UTF-8-encode a marker name for the fixed 64-byte C ``name`` field.

    Truncates on a codepoint boundary so a split multi-byte sequence never
    leaves invalid UTF-8 (mirrors the engine binding's ``_fixed_bytes``).
    """
    if not name:
        return b""
    return name.encode("utf-8")[:63].decode("utf-8", "ignore").encode("utf-8")


def _synth_waveform_value(waveform: str | int) -> int:
    if isinstance(waveform, str):
        resolver = getattr(_get_lib(), "sonare_synth_builtin_waveform_from_name", None)
        if resolver is not None:
            value = int(resolver(waveform.encode("utf-8")))
            if value >= 0:
                return value
            raise SonareValueError(f"unknown synth waveform: {waveform!r}")
    return _resolve_enum(waveform, _SYNTH_WAVEFORM_NAMES, "synth waveform")


@dataclass(frozen=True)
class SynthModRouting:
    """One NativeSynth mod-matrix routing (see :class:`SynthPatch`).

    ``source`` / ``destination`` accept the ordinal or a name (sources:
    ``"amp-env"`` / ``"filter-env"`` / ``"lfo1"`` / ``"lfo2"`` / ``"velocity"``
    / ``"key-track"`` / ``"mod-wheel"`` / ``"random"``; destinations:
    ``"pitch-cents"`` / ``"cutoff-cents"`` / ``"amp-gain"`` / ``"pan-units"``).
    ``depth`` is in destination units at full source deflection.
    """

    source: str | int
    destination: str | int
    depth: float


@dataclass(frozen=True)
class SynthPatch:
    """Versioned NativeSynth patch (see
    :meth:`Project.bounce_with_synth_instrument` and
    :meth:`RealtimeEngine.set_synth_instrument`).

    The patch starts from a BASE — the named ``preset`` (see
    :func:`synth_preset_names`; a ``"va:"`` routing prefix is accepted) or,
    when ``preset`` is empty, the default subtractive patch. Every numeric
    field defaults to ``None``, meaning "keep the base value"; any value given
    overrides (clamped to its audible range), including an explicit ``0``. Enum
    fields accept the C ordinal or a name with ``"default"`` = keep. A
    ``mod_routings`` tuple REPLACES the base mod matrix, and an empty tuple
    clears it, while ``None`` keeps it.

    Mode-specific deep parameters (FM operator stacks, modal mode tables,
    drawbar registrations, kit pieces, piano strings) travel inside the named
    presets; the struct exposes the wrapper sections every engine shares.
    """

    preset: str = ""
    engine_mode: str | int = 0
    waveform: str | int = 0
    unison: int | None = None
    detune_cents: float | None = None
    drift_cents: float | None = None
    drive: float | None = None
    filter_model: str | int = 0
    filter_output: str | int = 0
    cutoff_hz: float | None = None
    resonance_q: float | None = None
    key_track: float | None = None
    env_to_cutoff_cents: float | None = None
    vel_to_cutoff_cents: float | None = None
    amp_attack_ms: float | None = None
    amp_decay_ms: float | None = None
    amp_sustain: float | None = None
    amp_release_ms: float | None = None
    filter_attack_ms: float | None = None
    filter_decay_ms: float | None = None
    filter_sustain: float | None = None
    filter_release_ms: float | None = None
    lfo_rate_hz: float | None = None
    lfo_to_pitch_cents: float | None = None
    lfo2_rate_hz: float | None = None
    glide_ms: float | None = None
    body: str | int = 0
    body_mix: float | None = None
    stereo_spread: float | None = None
    mod_routings: tuple[SynthModRouting, ...] | None = None
    gain: float | None = None
    polyphony: int | None = None
    bus_drive: float | None = None

    def _to_c(self) -> SonareSynthPatch:
        if not isinstance(self.preset, str):
            raise TypeError("synth patch preset must be a string")
        c = SonareSynthPatch()
        c.struct_version = 2

        # A field left at None keeps the base; anything supplied — including a
        # zero — is marked present so the core overrides with it.
        def _set_float(name: str, bit: int, value: float | None) -> None:
            if value is None:
                return
            setattr(c, name, float(value))
            c.present_fields |= bit

        def _set_int(name: str, bit: int, value: int | None) -> None:
            if value is None:
                return
            setattr(c, name, int(value))
            c.present_fields |= bit

        c.preset = _strip_va_prefix(self.preset).encode("utf-8")[: SONARE_SYNTH_PRESET_NAME_MAX - 1]
        c.engine_mode = _synth_enum_value(self.engine_mode, _SYNTH_ENGINE_MODES, "engine mode")
        c.waveform = _synth_enum_value(self.waveform, _SYNTH_OSC_WAVEFORMS, "oscillator waveform")
        _set_int("unison", SONARE_SYNTH_FIELD_UNISON, self.unison)
        _set_float("detune_cents", SONARE_SYNTH_FIELD_DETUNE_CENTS, self.detune_cents)
        _set_float("drift_cents", SONARE_SYNTH_FIELD_DRIFT_CENTS, self.drift_cents)
        _set_float("drive", SONARE_SYNTH_FIELD_DRIVE, self.drive)
        c.filter_model = _synth_enum_value(self.filter_model, _SYNTH_FILTER_MODELS, "filter model")
        c.filter_output = _synth_enum_value(
            self.filter_output, _SYNTH_FILTER_OUTPUTS, "filter output"
        )
        _set_float("cutoff_hz", SONARE_SYNTH_FIELD_CUTOFF_HZ, self.cutoff_hz)
        _set_float("resonance_q", SONARE_SYNTH_FIELD_RESONANCE_Q, self.resonance_q)
        _set_float("key_track", SONARE_SYNTH_FIELD_KEY_TRACK, self.key_track)
        _set_float(
            "env_to_cutoff_cents",
            SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
            self.env_to_cutoff_cents,
        )
        _set_float(
            "vel_to_cutoff_cents",
            SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
            self.vel_to_cutoff_cents,
        )
        _set_float("amp_attack_ms", SONARE_SYNTH_FIELD_AMP_ATTACK_MS, self.amp_attack_ms)
        _set_float("amp_decay_ms", SONARE_SYNTH_FIELD_AMP_DECAY_MS, self.amp_decay_ms)
        _set_float("amp_sustain", SONARE_SYNTH_FIELD_AMP_SUSTAIN, self.amp_sustain)
        _set_float("amp_release_ms", SONARE_SYNTH_FIELD_AMP_RELEASE_MS, self.amp_release_ms)
        _set_float("filter_attack_ms", SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, self.filter_attack_ms)
        _set_float("filter_decay_ms", SONARE_SYNTH_FIELD_FILTER_DECAY_MS, self.filter_decay_ms)
        _set_float("filter_sustain", SONARE_SYNTH_FIELD_FILTER_SUSTAIN, self.filter_sustain)
        _set_float(
            "filter_release_ms", SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, self.filter_release_ms
        )
        _set_float("lfo_rate_hz", SONARE_SYNTH_FIELD_LFO_RATE_HZ, self.lfo_rate_hz)
        _set_float(
            "lfo_to_pitch_cents", SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, self.lfo_to_pitch_cents
        )
        _set_float("lfo2_rate_hz", SONARE_SYNTH_FIELD_LFO2_RATE_HZ, self.lfo2_rate_hz)
        _set_float("glide_ms", SONARE_SYNTH_FIELD_GLIDE_MS, self.glide_ms)
        c.body = _synth_enum_value(self.body, _SYNTH_BODY_TYPES, "body type")
        _set_float("body_mix", SONARE_SYNTH_FIELD_BODY_MIX, self.body_mix)
        _set_float("stereo_spread", SONARE_SYNTH_FIELD_STEREO_SPREAD, self.stereo_spread)
        routings = list(self.mod_routings or ())
        if self.mod_routings is not None:
            c.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS
        if len(routings) > SONARE_SYNTH_PATCH_MOD_ROUTINGS:
            raise SonareValueError(
                f"a synth patch supports at most {SONARE_SYNTH_PATCH_MOD_ROUTINGS} mod routings"
            )
        c.num_mod_routings = len(routings)
        for i, routing in enumerate(routings):
            c.mod_routings[i] = SonareSynthModRouting(
                source=_synth_enum_value(routing.source, _SYNTH_MOD_SOURCES, "mod source"),
                destination=_synth_enum_value(
                    routing.destination, _SYNTH_MOD_DESTINATIONS, "mod destination"
                ),
                depth=float(routing.depth),
            )
        _set_float("gain", SONARE_SYNTH_FIELD_GAIN, self.gain)
        _set_int("polyphony", SONARE_SYNTH_FIELD_POLYPHONY, self.polyphony)
        _set_float("bus_drive", SONARE_SYNTH_FIELD_BUS_DRIVE, self.bus_drive)
        return c

    @classmethod
    def _from_c(cls, c: SonareSynthPatch) -> SynthPatch:
        routings = tuple(
            SynthModRouting(
                source=_synth_enum_name(int(r.source), _SYNTH_MOD_SOURCES),
                destination=_synth_enum_name(int(r.destination), _SYNTH_MOD_DESTINATIONS),
                depth=float(r.depth),
            )
            for r in c.mod_routings[: int(c.num_mod_routings)]
        )
        return cls(
            preset=c.preset.decode("utf-8", errors="replace"),
            engine_mode=_synth_enum_name(int(c.engine_mode), _SYNTH_ENGINE_MODES),
            waveform=_synth_enum_name(int(c.waveform), _SYNTH_OSC_WAVEFORMS),
            unison=int(c.unison),
            detune_cents=float(c.detune_cents),
            drift_cents=float(c.drift_cents),
            drive=float(c.drive),
            filter_model=_synth_enum_name(int(c.filter_model), _SYNTH_FILTER_MODELS),
            filter_output=_synth_enum_name(int(c.filter_output), _SYNTH_FILTER_OUTPUTS),
            cutoff_hz=float(c.cutoff_hz),
            resonance_q=float(c.resonance_q),
            key_track=float(c.key_track),
            env_to_cutoff_cents=float(c.env_to_cutoff_cents),
            vel_to_cutoff_cents=float(c.vel_to_cutoff_cents),
            amp_attack_ms=float(c.amp_attack_ms),
            amp_decay_ms=float(c.amp_decay_ms),
            amp_sustain=float(c.amp_sustain),
            amp_release_ms=float(c.amp_release_ms),
            filter_attack_ms=float(c.filter_attack_ms),
            filter_decay_ms=float(c.filter_decay_ms),
            filter_sustain=float(c.filter_sustain),
            filter_release_ms=float(c.filter_release_ms),
            lfo_rate_hz=float(c.lfo_rate_hz),
            lfo_to_pitch_cents=float(c.lfo_to_pitch_cents),
            lfo2_rate_hz=float(c.lfo2_rate_hz),
            glide_ms=float(c.glide_ms),
            body=_synth_enum_name(int(c.body), _SYNTH_BODY_TYPES),
            body_mix=float(c.body_mix),
            stereo_spread=float(c.stereo_spread),
            mod_routings=routings,
            gain=float(c.gain),
            polyphony=int(c.polyphony),
            bus_drive=float(c.bus_drive),
        )


def _synth_patch_arg(instrument: SynthPatch | str | None) -> SynthPatch:
    if instrument is None:
        return SynthPatch()
    if isinstance(instrument, str):
        return SynthPatch(preset=instrument)
    return instrument


def synth_preset_names() -> list[str]:
    """NativeSynth preset catalog names (``"sine"``, ``"saw-lead"``,
    ``"e-piano"``, ``"drum-kit"``, ...). Use these to discover valid
    :class:`SynthPatch` preset names instead of hardcoding magic strings."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_synth_preset_names"):
        raise RuntimeError("libsonare was built without the NativeSynth ABI")
    raw = lib.sonare_synth_preset_names()
    if not raw:
        return []
    return [name for name in raw.decode("utf-8").split("\n") if name]


def synth_preset_patch(name: str) -> SynthPatch:
    """Fetch the named catalog preset as a :class:`SynthPatch` (the preset name
    plus the wrapper-section values), so hosts can inspect a preset and tweak
    fields before binding it. A ``"va:"`` routing prefix is accepted. Raises
    :class:`SonareError` for unknown names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_synth_preset_patch"):
        raise RuntimeError("libsonare was built without the NativeSynth ABI")
    out = SonareSynthPatch()
    _check(lib.sonare_synth_preset_patch(_strip_va_prefix(name).encode("utf-8"), ctypes.byref(out)))
    return SynthPatch._from_c(out)


class ExternalInstrument(Protocol):
    """A host-supplied instrument driven during :meth:`Project.bounce_with_instruments`.

    The object models a MIDI instrument the bounce engine hosts in place of the
    built-in synth. Only :meth:`render` is required; ``prepare``, ``on_event``
    and a ``latency_samples`` attribute are optional (duck-typed). Every method
    is called synchronously on the thread that invokes the bounce, so no
    cross-thread state sharing is involved.

    Optional members::

        def prepare(self, sample_rate: float, max_block_size: int) -> None: ...
                def on_event(self, destination_id: int,
                             ump_words: tuple[int, ...], render_frame: int) -> None: ...
                latency_samples: int  # reported plugin-delay (PDC); defaults to 0
                tail_samples: int  # release/effect tail for auto-length bounce; defaults to 0
    """

    def render(self, channels: np.ndarray, num_frames: int) -> None:
        """Add ``num_frames`` of audio into the planar ``(channels, num_frames)``
        float32 array (already zero-filled). Sum into it; do not overwrite
        unrelated frames."""
        ...


def _make_instrument_callbacks(
    instrument: ExternalInstrument,
    errors: list[BaseException],
    keepalive: list[object],
) -> SonareInstrumentCallbacks:
    """Build the C callback table for one external instrument.

    The returned closures capture ``instrument`` directly (so ``user_data`` is
    unused) and record the first exception raised by any user callback into
    ``errors`` instead of propagating through the C frames (ctypes would only
    print it). The ``ctypes`` callback objects are appended to ``keepalive``;
    the caller MUST keep that list referenced for the whole bounce call, since
    copying the struct into the bindings array does not pin the closures.
    """
    cbs = SonareInstrumentCallbacks()
    cbs.user_data = None
    latency = getattr(instrument, "latency_samples", 0)
    cbs.latency_samples = int(latency) if latency else 0
    tail = getattr(instrument, "tail_samples", 0)
    cbs.tail_samples = int(tail) if tail else 0

    prepare = getattr(instrument, "prepare", None)
    if callable(prepare):

        def _prepare(_user, sample_rate, max_block_size, _fn=prepare):  # type: ignore[no-untyped-def]
            if errors:
                return
            try:
                _fn(float(sample_rate), int(max_block_size))
            except BaseException as exc:  # noqa: BLE001
                if not errors:
                    errors.append(exc)

        cb_prepare = SonareInstrumentPrepareCallback(_prepare)
        keepalive.append(cb_prepare)
        cbs.prepare = cb_prepare

    on_event = getattr(instrument, "on_event", None)
    if callable(on_event):

        def _on_event(_user, dest, words, count, frame, _fn=on_event):  # type: ignore[no-untyped-def]
            if errors:
                return
            try:
                ump = tuple(int(words[i]) for i in range(int(count)))
                _fn(int(dest), ump, int(frame))
            except BaseException as exc:  # noqa: BLE001
                if not errors:
                    errors.append(exc)

        cb_event = SonareInstrumentOnEventCallback(_on_event)
        keepalive.append(cb_event)
        cbs.on_event = cb_event

    render = instrument.render

    def _render(_user, channels, num_channels, num_frames, _fn=render):  # type: ignore[no-untyped-def]
        if errors:
            return
        try:
            nch = int(num_channels)
            nfr = int(num_frames)
            if nch <= 0 or nfr <= 0:
                return
            # The engine zero-fills the C scratch before this call; the
            # instrument sums into `block`, and we add `block` back into each
            # planar channel buffer (a view that shares the C memory).
            block = np.zeros((nch, nfr), dtype=np.float32)
            _fn(block, nfr)
            for ch in range(nch):
                dst = np.ctypeslib.as_array(channels[ch], shape=(nfr,))
                dst += block[ch]
        except BaseException as exc:  # noqa: BLE001
            if not errors:
                errors.append(exc)

    cb_render = SonareInstrumentRenderCallback(_render)
    keepalive.append(cb_render)
    cbs.render = cb_render
    return cbs


@dataclass(frozen=True)
class ProjectCompileResult:
    """Structured compile result matching the C/Node/WASM project surface.

    Iteration preserves the legacy ``has_timeline, messages = project.compile()``
    pattern while exposing structured diagnostics.
    """

    has_timeline: bool
    messages: str
    diagnostics: tuple[ProjectDiagnostic, ...]

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
        raise RuntimeError("libsonare was built without arrangement support")
    abi = int(lib.sonare_project_abi_version())
    if abi != EXPECTED_PROJECT_ABI_VERSION:
        raise RuntimeError(
            f"libsonare project ABI mismatch: native binary reports {abi}, "
            f"expected {EXPECTED_PROJECT_ABI_VERSION}. The installed shared "
            "library is incompatible with this Python binding (0 = arrangement "
            "support not compiled in)."
        )


def _track_kind_value(kind: str | int) -> int:
    return _resolve_enum(kind, _TRACK_KIND_NAMES, "track kind")


def _midi_event_tuple(name: str, *args: float | int) -> tuple[float, int, int]:
    lib = _get_lib()
    event = SonareMidiEventPod()
    fn = getattr(lib, name)
    _check(fn(*args, ctypes.byref(event)))
    return (float(event.ppq), int(event.data0), int(event.data1))


def _validate_midi_event_word(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise SonareValueError(f"{label} must be an integer in [0, 4294967295]")
    word = float(cast(SupportsFloat, value))
    if not math.isfinite(word) or not word.is_integer() or word < 0.0 or word > 0xFFFFFFFF:
        raise SonareValueError(f"{label} must be an integer in [0, 4294967295]")
    return int(word)


def _validate_midi_event_ppq(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise SonareValueError(f"{label} must be a non-negative finite number")
    ppq = float(cast(SupportsFloat, value))
    if not math.isfinite(ppq) or ppq < 0.0:
        raise SonareValueError(f"{label} must be a non-negative finite number")
    return ppq
