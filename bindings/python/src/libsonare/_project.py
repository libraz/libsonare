"""Headless arrangement / DAW project wrapper for libsonare.

This wraps the curated ``sonare_project_*`` C ABI (``src/sonare_c_project.h``):
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
from typing import Any, Protocol, SupportsFloat, cast

import numpy as np

from ._ffi_types_mastering_project import (
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
    SonareBuiltinInstrumentBinding,
    SonareBuiltinSynthConfig,
    SonareInstrumentBinding,
    SonareInstrumentCallbacks,
    SonareInstrumentOnEventCallback,
    SonareInstrumentPrepareCallback,
    SonareInstrumentRenderCallback,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    SonareMidiRouteConfig,
    SonareNotePairValidation,
    SonareProjectAssistSidecar,
    SonareProjectBounceOptions,
    SonareProjectChordSymbol,
    SonareProjectClipCompSegment,
    SonareProjectClipDesc,
    SonareProjectClipFade,
    SonareProjectClipTake,
    SonareProjectCompileResult,
    SonareProjectKeySegment,
    SonareProjectLoopRecordingDesc,
    SonareProjectMarker,
    SonareProjectTempoSegment,
    SonareProjectTimeSignatureSegment,
    SonareProjectTrackDesc,
    SonareProjectWarpAnchor,
    SonareProjectWarpMapDesc,
    SonareSf2InstrumentBinding,
    SonareSf2InstrumentConfig,
    SonareSf2ProgramStatus,
    SonareSynthInstrumentBinding,
    SonareSynthModRouting,
    SonareSynthPatch,
    _check,
    _curve_value,
    _from_c_float_array,
    _get_lib,
    _to_c_float_array,
)
from .types import ProjectMarker

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

# Must match SONARE_PROJECT_ABI_VERSION (src/sonare_c_project.h) and the other
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


def _fade_curve_value(curve: str | int) -> int:
    if isinstance(curve, int):
        return curve
    key = curve.lower()
    if key not in _FADE_CURVE_NAMES:
        raise ValueError(f"unknown fade curve: {curve}")
    return _FADE_CURVE_NAMES[key]


def _loop_mode_value(mode: str | int) -> int:
    if isinstance(mode, int):
        return mode
    key = mode.lower()
    if key not in _LOOP_MODE_NAMES:
        raise ValueError(f"unknown loop mode: {mode}")
    return _LOOP_MODE_NAMES[key]


def _automation_lane_desc(
    target_param_id: int,
    points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None,
) -> tuple[SonareAutomationLaneDesc, object]:
    """Marshal ``(ppq, value, curve)`` breakpoints into a SonareAutomationLaneDesc.

    Returns ``(desc, backing)``; the caller must keep ``backing`` (the C point
    array) alive for the duration of the native call.
    """
    rows = list(points) if points is not None else []
    count = len(rows)
    c_points = (SonareAutomationPoint * count)() if count else None
    for i, pt in enumerate(rows):
        seq = tuple(pt)
        if len(seq) < 2:
            raise ValueError(f"points[{i}] must contain at least (ppq, value)")
        ppq = float(seq[0])
        value = float(seq[1])
        curve = _curve_value(seq[2]) if len(seq) >= 3 else 0
        if not math.isfinite(ppq):
            raise ValueError(f"points[{i}].ppq must be a finite number")
        if not math.isfinite(value):
            raise ValueError(f"points[{i}].value must be a finite number")
        c_points[i].ppq = ppq
        c_points[i].value = value
        c_points[i].curve_to_next = int(curve)
    desc = SonareAutomationLaneDesc(
        target_param_id=int(target_param_id),
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
    polyphony 48); override only what you need.
    """

    gain: float = 0.0
    polyphony: int = 0

    def _to_c(self) -> SonareSf2InstrumentConfig:
        return SonareSf2InstrumentConfig(
            struct_version=0,
            gain=float(self.gain),
            polyphony=int(self.polyphony),
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
    if isinstance(waveform, int):
        return waveform
    key = waveform.lower()
    if key not in _SYNTH_WAVEFORM_NAMES:
        raise ValueError(f"unknown synth waveform: {waveform}")
    return _SYNTH_WAVEFORM_NAMES[key]


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
    field then uses "0 => keep the base value" and non-zero values override
    (clamped to their audible ranges); enum fields accept the C ordinal or a
    name with ``"default"`` = keep. The frozen C ABI has no per-field presence
    bits, so explicit zero numeric overrides such as ``amp_sustain=0`` cannot
    be represented and keep the base value. A non-empty ``mod_routings``
    REPLACES the base mod matrix.

    Mode-specific deep parameters (FM operator stacks, modal mode tables,
    drawbar registrations, kit pieces, piano strings) travel inside the named
    presets; the struct exposes the wrapper sections every engine shares.
    """

    preset: str = ""
    engine_mode: str | int = 0
    waveform: str | int = 0
    unison: int = 0
    detune_cents: float = 0.0
    drift_cents: float = 0.0
    drive: float = 0.0
    filter_model: str | int = 0
    filter_output: str | int = 0
    cutoff_hz: float = 0.0
    resonance_q: float = 0.0
    key_track: float = 0.0
    env_to_cutoff_cents: float = 0.0
    vel_to_cutoff_cents: float = 0.0
    amp_attack_ms: float = 0.0
    amp_decay_ms: float = 0.0
    amp_sustain: float = 0.0  # 0 keeps the base; explicit zero is not representable.
    amp_release_ms: float = 0.0
    filter_attack_ms: float = 0.0
    filter_decay_ms: float = 0.0
    filter_sustain: float = 0.0  # 0 keeps the base; explicit zero is not representable.
    filter_release_ms: float = 0.0
    lfo_rate_hz: float = 0.0
    lfo_to_pitch_cents: float = 0.0
    lfo2_rate_hz: float = 0.0
    glide_ms: float = 0.0
    body: str | int = 0
    body_mix: float = 0.0
    stereo_spread: float = 0.0
    mod_routings: tuple[SynthModRouting, ...] = ()
    gain: float = 0.0
    polyphony: int = 0
    bus_drive: float = 0.0

    def _to_c(self) -> SonareSynthPatch:
        if not isinstance(self.preset, str):
            raise TypeError("synth patch preset must be a string")
        c = SonareSynthPatch()
        c.struct_version = 1
        c.preset = _strip_va_prefix(self.preset).encode("utf-8")[: SONARE_SYNTH_PRESET_NAME_MAX - 1]
        c.engine_mode = _synth_enum_value(self.engine_mode, _SYNTH_ENGINE_MODES, "engine mode")
        c.waveform = _synth_enum_value(self.waveform, _SYNTH_OSC_WAVEFORMS, "oscillator waveform")
        c.unison = int(self.unison)
        c.detune_cents = float(self.detune_cents)
        c.drift_cents = float(self.drift_cents)
        c.drive = float(self.drive)
        c.filter_model = _synth_enum_value(self.filter_model, _SYNTH_FILTER_MODELS, "filter model")
        c.filter_output = _synth_enum_value(
            self.filter_output, _SYNTH_FILTER_OUTPUTS, "filter output"
        )
        c.cutoff_hz = float(self.cutoff_hz)
        c.resonance_q = float(self.resonance_q)
        c.key_track = float(self.key_track)
        c.env_to_cutoff_cents = float(self.env_to_cutoff_cents)
        c.vel_to_cutoff_cents = float(self.vel_to_cutoff_cents)
        c.amp_attack_ms = float(self.amp_attack_ms)
        c.amp_decay_ms = float(self.amp_decay_ms)
        c.amp_sustain = float(self.amp_sustain)
        c.amp_release_ms = float(self.amp_release_ms)
        c.filter_attack_ms = float(self.filter_attack_ms)
        c.filter_decay_ms = float(self.filter_decay_ms)
        c.filter_sustain = float(self.filter_sustain)
        c.filter_release_ms = float(self.filter_release_ms)
        c.lfo_rate_hz = float(self.lfo_rate_hz)
        c.lfo_to_pitch_cents = float(self.lfo_to_pitch_cents)
        c.lfo2_rate_hz = float(self.lfo2_rate_hz)
        c.glide_ms = float(self.glide_ms)
        c.body = _synth_enum_value(self.body, _SYNTH_BODY_TYPES, "body type")
        c.body_mix = float(self.body_mix)
        c.stereo_spread = float(self.stereo_spread)
        routings = list(self.mod_routings)
        if len(routings) > SONARE_SYNTH_PATCH_MOD_ROUTINGS:
            raise ValueError(
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
        c.gain = float(self.gain)
        c.polyphony = int(self.polyphony)
        c.bus_drive = float(self.bus_drive)
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
            try:
                _fn(float(sample_rate), int(max_block_size))
            except BaseException as exc:  # noqa: BLE001
                errors.append(exc)

        cb_prepare = SonareInstrumentPrepareCallback(_prepare)
        keepalive.append(cb_prepare)
        cbs.prepare = cb_prepare

    on_event = getattr(instrument, "on_event", None)
    if callable(on_event):

        def _on_event(_user, dest, words, count, frame, _fn=on_event):  # type: ignore[no-untyped-def]
            try:
                ump = tuple(int(words[i]) for i in range(int(count)))
                _fn(int(dest), ump, int(frame))
            except BaseException as exc:  # noqa: BLE001
                errors.append(exc)

        cb_event = SonareInstrumentOnEventCallback(_on_event)
        keepalive.append(cb_event)
        cbs.on_event = cb_event

    render = instrument.render

    def _render(_user, channels, num_channels, num_frames, _fn=render):  # type: ignore[no-untyped-def]
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
    if isinstance(kind, int):
        return kind
    key = kind.lower()
    if key not in _TRACK_KIND_NAMES:
        raise ValueError(f"unknown track kind: {kind}")
    return _TRACK_KIND_NAMES[key]


def _warp_mode_value(mode: str | int) -> int:
    if isinstance(mode, int):
        return mode
    key = mode.lower()
    if key == "off":
        return 0
    if key == "repitch":
        return 1
    if key == "tempo-sync":
        return 2
    raise ValueError(f"unknown warp mode: {mode}")


def _midi_event_tuple(name: str, *args: float | int) -> tuple[float, int, int]:
    lib = _get_lib()
    event = SonareMidiEventPod()
    fn = getattr(lib, name)
    _check(fn(*args, ctypes.byref(event)))
    return (float(event.ppq), int(event.data0), int(event.data1))


def _validate_midi_event_word(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise ValueError(f"{label} must be an integer in [0, 4294967295]")
    word = float(cast(SupportsFloat, value))
    if not math.isfinite(word) or not word.is_integer() or word < 0.0 or word > 0xFFFFFFFF:
        raise ValueError(f"{label} must be an integer in [0, 4294967295]")
    return int(word)


def _validate_midi_event_ppq(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise ValueError(f"{label} must be a non-negative finite number")
    ppq = float(cast(SupportsFloat, value))
    if not math.isfinite(ppq) or ppq < 0.0:
        raise ValueError(f"{label} must be a non-negative finite number")
    return ppq


class Project:
    """Pythonic wrapper around the native headless-project handle.

    All mutation routes through the native ``EditHistory`` (so :meth:`undo` /
    :meth:`redo` work), musical positions are PPQ (quarter notes), and
    serialization is deterministic (``to_json`` is byte-stable for a given
    project state within one build).
    """

    def __init__(self) -> None:
        lib = _get_lib()
        _check_project_abi(lib)
        handle = ctypes.c_void_p()
        _check(lib.sonare_project_create(ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        if self._handle is not None:
            _get_lib().sonare_project_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> Project:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("Project is closed")
        return self._handle

    # -- serialization ------------------------------------------------------

    def to_json_bytes(self) -> bytes:
        """Serialize the project to deterministic JSON as raw UTF-8 bytes."""
        lib = _get_lib()
        out = ctypes.c_char_p()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_serialize(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out.value:
                return b""
            # `out.value` is a fresh Python bytes copy of the C string; keep the
            # original pointer (`out`) for the free call below.
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_string(out)

    def to_json(self) -> str:
        """Serialize the project to deterministic JSON (UTF-8 decoded)."""
        return self.to_json_bytes().decode("utf-8")

    @classmethod
    def from_json(cls, json: str | bytes) -> Project:
        """Deserialize project JSON into a new :class:`Project`.

        Raises ``ValueError`` on malformed input (with the joined native
        diagnostic messages), never crashing.
        """
        lib = _get_lib()
        _check_project_abi(lib)
        data = json.encode("utf-8") if isinstance(json, str) else bytes(json)
        handle = ctypes.c_void_p()
        diag = ctypes.c_char_p()
        rc = lib.sonare_project_deserialize(
            data, ctypes.c_size_t(len(data)), ctypes.byref(handle), ctypes.byref(diag)
        )
        if rc != 0:
            try:
                detail = diag.value.decode("utf-8") if diag.value else ""
            finally:
                if diag:
                    lib.sonare_free_string(diag)
            raise ValueError(detail or "failed to deserialize project JSON")
        if diag:
            lib.sonare_free_string(diag)
        obj = cls.__new__(cls)
        obj._handle = handle
        return obj

    @classmethod
    def from_json_with_diagnostics(cls, json: str | bytes) -> ProjectDeserializeResult:
        """Deserialize project JSON and return warnings from successful loads."""
        lib = _get_lib()
        _check_project_abi(lib)
        data = json.encode("utf-8") if isinstance(json, str) else bytes(json)
        handle = ctypes.c_void_p()
        diag = ctypes.c_char_p()
        rc = lib.sonare_project_deserialize(
            data, ctypes.c_size_t(len(data)), ctypes.byref(handle), ctypes.byref(diag)
        )
        try:
            diagnostics = diag.value.decode("utf-8") if diag.value else ""
        finally:
            if diag:
                lib.sonare_free_string(diag)
        if rc != 0:
            raise ValueError(diagnostics or "failed to deserialize project JSON")
        obj = cls.__new__(cls)
        obj._handle = handle
        return ProjectDeserializeResult(project=obj, diagnostics=diagnostics)

    def set_sample_rate(self, sample_rate: float) -> None:
        """Set the project sample rate in Hz (must be > 0)."""
        _check(
            _get_lib().sonare_project_set_sample_rate(self._require_handle(), float(sample_rate))
        )

    # -- edit ---------------------------------------------------------------

    def add_track(self, kind: str | int = TRACK_AUDIO, name: str | None = None) -> int:
        """Add a track and return its allocated stable id.

        Args:
            kind: ``"audio"`` / ``"midi"`` / ``"aux"`` (or the integer ordinal).
            name: Optional track name.
        """
        desc = SonareProjectTrackDesc(
            kind=_track_kind_value(kind),
            name=name.encode("utf-8") if name is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_track(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            )
        )
        return int(out_id.value)

    def add_clip(
        self,
        track_id: int,
        start_ppq: float,
        length_ppq: float,
        *,
        is_midi: bool = False,
        source_offset_ppq: float = 0.0,
        gain: float = 1.0,
        audio: Sequence[float] | np.ndarray | None = None,
        audio_channels: int = 1,
        audio_sample_rate: int = 0,
        source_uri: str | None = None,
    ) -> int:
        """Add an audio or MIDI clip and return its allocated clip id.

        For an audio clip, supply decoded interleaved ``audio`` to make it
        renderable by :meth:`bounce`; omit it for a metadata-only source
        (optionally referenced by ``source_uri``). For a MIDI clip pass
        ``is_midi=True`` and set events later via :meth:`set_midi_events`.
        """
        c_audio = None
        audio_frames = 0
        backing = None
        if audio is not None:
            backing, total = _to_c_float_array(audio)
            c_audio = backing
            channels = int(audio_channels)
            if channels <= 0 or total % channels != 0:
                raise ValueError("audio length must be a multiple of audio_channels")
            audio_frames = total // channels
        desc = SonareProjectClipDesc(
            track_id=int(track_id),
            is_midi=1 if is_midi else 0,
            start_ppq=float(start_ppq),
            length_ppq=float(length_ppq),
            source_offset_ppq=float(source_offset_ppq),
            gain=float(gain),
            audio_interleaved=c_audio,
            audio_frames=int(audio_frames),
            audio_channels=int(audio_channels),
            audio_sample_rate=int(audio_sample_rate),
            source_uri=source_uri.encode("utf-8") if source_uri is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_clip(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            )
        )
        # `backing` is kept alive until the call returns above.
        del backing
        return int(out_id.value)

    def add_loop_recording_takes(
        self,
        track_id: int,
        start_ppq: float,
        loop_length_ppq: float,
        audio: Sequence[float] | np.ndarray,
        *,
        audio_channels: int = 1,
        audio_sample_rate: int = 48000,
    ) -> tuple[int, int]:
        """Split a captured loop recording into takes and add one clip.

        Returns ``(clip_id, take_count)``. ``audio`` is interleaved float32
        capture data; each loop-length span becomes a separate take and the
        newest take is made active.
        """
        channels = int(audio_channels)
        backing, total = _to_c_float_array(audio)
        if channels <= 0 or total % channels != 0:
            raise ValueError("audio length must be a multiple of audio_channels")
        frames = total // channels
        desc = SonareProjectLoopRecordingDesc(
            track_id=int(track_id),
            reserved=0,
            start_ppq=float(start_ppq),
            loop_length_ppq=float(loop_length_ppq),
            audio_interleaved=backing,
            audio_frames=int(frames),
            audio_channels=channels,
            audio_sample_rate=int(audio_sample_rate),
        )
        out_clip = ctypes.c_uint32()
        out_take_count = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_add_loop_recording_takes(
                self._require_handle(),
                ctypes.byref(desc),
                ctypes.byref(out_clip),
                ctypes.byref(out_take_count),
            )
        )
        del backing
        return int(out_clip.value), int(out_take_count.value)

    def add_midi_clip(self, start_ppq: float, length_ppq: float) -> tuple[int, int]:
        """Create a MIDI track + clip; return ``(track_id, clip_id)``."""
        out_track = ctypes.c_uint32()
        out_clip = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_midi_clip(
                self._require_handle(),
                float(start_ppq),
                float(length_ppq),
                ctypes.byref(out_track),
                ctypes.byref(out_clip),
            )
        )
        return int(out_track.value), int(out_clip.value)

    def split_clip(self, clip_id: int, split_ppq: float) -> int:
        """Split a clip at ``split_ppq`` (absolute PPQ); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_split_clip(
                self._require_handle(),
                int(clip_id),
                float(split_ppq),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def trim_clip(self, clip_id: int, new_start_ppq: float, new_length_ppq: float) -> None:
        """Trim a clip's start / length (PPQ)."""
        _check(
            _get_lib().sonare_project_trim_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                float(new_length_ppq),
            )
        )

    def move_clip(self, clip_id: int, new_start_ppq: float, new_track_id: int = 0) -> None:
        """Move a clip to ``new_start_ppq`` (and optionally ``new_track_id``)."""
        _check(
            _get_lib().sonare_project_move_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                int(new_track_id),
            )
        )

    def set_track_kind(self, track_id: int, kind: str | int) -> None:
        """Change a track kind via an undoable edit command."""
        _check(
            _get_lib().sonare_project_set_track_kind(
                self._require_handle(),
                int(track_id),
                _track_kind_value(kind),
            )
        )

    def set_clip_warp_ref(self, clip_id: int, warp_ref_id: int) -> None:
        """Set a clip's warp reference id (``0`` clears it)."""
        _check(
            _get_lib().sonare_project_set_clip_warp_ref(
                self._require_handle(),
                int(clip_id),
                int(warp_ref_id),
            )
        )

    def set_clip_warp_mode(self, clip_id: int, mode: str | int) -> None:
        """Set a clip's warp playback mode."""
        _check(
            _get_lib().sonare_project_set_clip_warp_mode(
                self._require_handle(),
                int(clip_id),
                _warp_mode_value(mode),
            )
        )

    def set_clip_takes(
        self,
        clip_id: int,
        takes: Sequence[Mapping[str, object]] | Sequence[tuple[int, int, float, str]] | None,
        active_take_id: int = 0,
    ) -> None:
        """Replace a clip's take list and active take via an undoable edit."""
        take_items = list(takes or [])
        c_takes = (SonareProjectClipTake * len(take_items))()
        name_backing: list[bytes] = []
        for i, item in enumerate(take_items):
            if isinstance(item, Mapping):
                take_id = int(item.get("id", 0))
                source_id = int(item.get("source_id", item.get("sourceId", 0)))
                source_offset = float(
                    item.get("source_offset_ppq", item.get("sourceOffsetPpq", 0.0))
                )
                name = item.get("name", "")
            else:
                take_id, source_id, source_offset, name = item
            c_takes[i].id = int(take_id)
            c_takes[i].source_id = int(source_id)
            c_takes[i].source_offset_ppq = float(source_offset)
            if name:
                encoded = str(name).encode("utf-8")
                name_backing.append(encoded)
                c_takes[i].name = encoded
        _check(
            _get_lib().sonare_project_set_clip_takes(
                self._require_handle(),
                int(clip_id),
                c_takes if take_items else None,
                len(take_items),
                int(active_take_id),
            )
        )
        del name_backing

    def set_clip_comp_segments(
        self,
        clip_id: int,
        segments: Sequence[Mapping[str, object]] | Sequence[tuple[float, float, int]] | None,
    ) -> None:
        """Replace a clip's comp lane via an undoable edit."""
        segment_items = list(segments or [])
        c_segments = (SonareProjectClipCompSegment * len(segment_items))()
        for i, item in enumerate(segment_items):
            if isinstance(item, Mapping):
                start_ppq = float(item.get("start_ppq", item.get("startPpq", 0.0)))
                end_ppq = float(item.get("end_ppq", item.get("endPpq", 0.0)))
                take_id = int(item.get("take_id", item.get("takeId", 0)))
            else:
                start_ppq, end_ppq, take_id = item
            c_segments[i].start_ppq = float(start_ppq)
            c_segments[i].end_ppq = float(end_ppq)
            c_segments[i].take_id = int(take_id)
        _check(
            _get_lib().sonare_project_set_clip_comp_segments(
                self._require_handle(),
                int(clip_id),
                c_segments if segment_items else None,
                len(segment_items),
            )
        )

    def set_warp_map(
        self,
        warp_ref_id: int,
        anchors: Sequence[tuple[float, float]],
        name: str | None = None,
    ) -> None:
        """Add or replace a first-class project warp map."""
        count = len(anchors)
        c_anchors = (SonareProjectWarpAnchor * count)()
        for i, (warp_sample, source_sample) in enumerate(anchors):
            c_anchors[i].warp_sample = float(warp_sample)
            c_anchors[i].source_sample = float(source_sample)
        encoded_name = name.encode("utf-8") if name else None
        desc = SonareProjectWarpMapDesc(
            id=int(warp_ref_id),
            name=encoded_name,
            anchors=c_anchors,
            anchor_count=count,
        )
        _check(_get_lib().sonare_project_set_warp_map(self._require_handle(), ctypes.byref(desc)))

    def remove_warp_map(self, warp_ref_id: int) -> None:
        """Remove a first-class project warp map by id."""
        _check(
            _get_lib().sonare_project_remove_warp_map(
                self._require_handle(),
                int(warp_ref_id),
            )
        )

    def set_track_midi_destination(self, track_id: int, destination_id: int) -> None:
        """Route a track's MIDI to host-instrument ``destination_id`` (0 = default).

        The compiler stamps every MIDI clip on the track with this id so the
        engine dispatches the clip's events to the instrument registered for that
        destination. Routes through an undoable edit command.
        """
        _check(
            _get_lib().sonare_project_set_track_midi_destination(
                self._require_handle(),
                int(track_id),
                int(destination_id),
            )
        )

    def remove_clip(self, clip_id: int) -> None:
        """Remove a clip via an undoable edit command (undo restores it)."""
        _check(_get_lib().sonare_project_remove_clip(self._require_handle(), int(clip_id)))

    def set_clip_gain(self, clip_id: int, gain: float) -> None:
        """Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit."""
        g = float(gain)
        if not math.isfinite(g) or g < 0.0:
            raise ValueError("gain must be a finite number >= 0")
        _check(_get_lib().sonare_project_set_clip_gain(self._require_handle(), int(clip_id), g))

    def set_clip_fade(
        self,
        clip_id: int,
        fade_in_length_ppq: float = 0.0,
        fade_out_length_ppq: float = 0.0,
        *,
        fade_in_curve: str | int = FADE_CURVE_LINEAR,
        fade_out_curve: str | int = FADE_CURVE_LINEAR,
    ) -> None:
        """Set a clip's fade-in and fade-out regions via an undoable edit.

        Each fade length (PPQ) must be finite and >= 0 (0 = no fade); each curve
        is a :data:`FADE_CURVE_*` ordinal or name (``"linear"`` / ``"equal-power"``
        / ``"exponential"`` / ``"logarithmic"``).
        """
        fin = float(fade_in_length_ppq)
        fout = float(fade_out_length_ppq)
        if not math.isfinite(fin) or fin < 0.0:
            raise ValueError("fade_in_length_ppq must be a finite number >= 0")
        if not math.isfinite(fout) or fout < 0.0:
            raise ValueError("fade_out_length_ppq must be a finite number >= 0")
        c_in = SonareProjectClipFade(length_ppq=fin, curve=_fade_curve_value(fade_in_curve))
        c_out = SonareProjectClipFade(length_ppq=fout, curve=_fade_curve_value(fade_out_curve))
        _check(
            _get_lib().sonare_project_set_clip_fade(
                self._require_handle(),
                int(clip_id),
                ctypes.byref(c_in),
                ctypes.byref(c_out),
            )
        )

    def set_clip_loop(
        self, clip_id: int, loop_mode: str | int = LOOP_MODE_OFF, loop_length_ppq: float = 0.0
    ) -> None:
        """Set a clip's loop mode + loop length (PPQ) via an undoable edit.

        ``loop_mode`` is a :data:`LOOP_MODE_*` ordinal or name. When looping,
        ``loop_length_ppq`` must be finite and > 0; otherwise finite and >= 0.
        """
        mode = _loop_mode_value(loop_mode)
        length = float(loop_length_ppq)
        if not math.isfinite(length) or length < 0.0:
            raise ValueError("loop_length_ppq must be a finite number >= 0")
        if mode == LOOP_MODE_LOOP and length <= 0.0:
            raise ValueError("loop_length_ppq must be > 0 when looping")
        _check(
            _get_lib().sonare_project_set_clip_loop(
                self._require_handle(), int(clip_id), int(mode), length
            )
        )

    def set_clip_source(self, clip_id: int, source_id: int) -> None:
        """Rebind a clip to a different (already-registered) source via an undoable edit."""
        _check(
            _get_lib().sonare_project_set_clip_source(
                self._require_handle(), int(clip_id), int(source_id)
            )
        )

    def duplicate_clip(self, clip_id: int, new_start_ppq: float) -> int:
        """Duplicate a clip at ``new_start_ppq`` (same track); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_duplicate_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def remove_track(self, track_id: int) -> None:
        """Remove a track (and its clips) via an undoable edit command."""
        _check(_get_lib().sonare_project_remove_track(self._require_handle(), int(track_id)))

    def rename_track(self, track_id: int, name: str | None = None) -> None:
        """Rename a track via an undoable edit command (``None`` = empty name)."""
        _check(
            _get_lib().sonare_project_rename_track(
                self._require_handle(),
                int(track_id),
                name.encode("utf-8") if name is not None else None,
            )
        )

    def set_track_route(
        self, track_id: int, channel_strip_ref: str | None = None, output_target: str | None = None
    ) -> None:
        """Set a track's mixer-strip binding + output target via an undoable edit.

        Pass ``None`` (or ``""``) for either field to clear it.
        """
        _check(
            _get_lib().sonare_project_set_track_route(
                self._require_handle(),
                int(track_id),
                channel_strip_ref.encode("utf-8") if channel_strip_ref is not None else None,
                output_target.encode("utf-8") if output_target is not None else None,
            )
        )

    def add_automation_lane(
        self,
        track_id: int,
        target_param_id: int,
        points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None = None,
    ) -> int:
        """Append an automation lane to a track; return its index within the track.

        ``points`` is an optional list of ``(ppq, value, curve)`` breakpoints
        (``curve`` is an :class:`AutomationCurve` ordinal / name; default linear).
        """
        desc, _backing = _automation_lane_desc(target_param_id, points)
        out_index = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_add_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.byref(desc),
                ctypes.byref(out_index),
            )
        )
        del _backing
        return int(out_index.value)

    def edit_automation_lane(
        self,
        track_id: int,
        lane_index: int,
        target_param_id: int,
        points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None = None,
    ) -> None:
        """Replace an existing automation lane in place via an undoable edit."""
        desc, _backing = _automation_lane_desc(target_param_id, points)
        _check(
            _get_lib().sonare_project_edit_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.c_size_t(int(lane_index)),
                ctypes.byref(desc),
            )
        )
        del _backing

    def remove_automation_lane(self, track_id: int, lane_index: int) -> None:
        """Remove an automation lane from a track via an undoable edit command."""
        _check(
            _get_lib().sonare_project_remove_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.c_size_t(int(lane_index)),
            )
        )

    def undo(self) -> None:
        """Undo the most recent edit (raises when the undo stack is empty)."""
        _check(_get_lib().sonare_project_undo(self._require_handle()))

    def redo(self) -> None:
        """Redo the most recently undone edit (raises when the redo stack is empty)."""
        _check(_get_lib().sonare_project_redo(self._require_handle()))

    # -- MIDI ---------------------------------------------------------------

    def set_midi_events(
        self,
        clip_id: int,
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]] | np.ndarray,
    ) -> None:
        """Replace a MIDI clip's entire event list.

        Each event is ``(ppq, data0, data1)`` (the first two UMP-1.0 words of a
        channel-voice message; stored opaquely). Pass an empty sequence to clear.
        """
        rows = list(events)
        count = len(rows)
        c_events = (SonareMidiEventPod * count)()
        for i, ev in enumerate(rows):
            if len(ev) < 3:
                raise ValueError(f"events[{i}] must contain (ppq, data0, data1)")
            ppq, data0, data1 = ev[0], ev[1], ev[2]
            c_events[i].ppq = _validate_midi_event_ppq(ppq, f"events[{i}].ppq")
            c_events[i].data0 = _validate_midi_event_word(data0, f"events[{i}].data0")
            c_events[i].data1 = _validate_midi_event_word(data1, f"events[{i}].data1")
        _check(
            _get_lib().sonare_project_set_midi_events(
                self._require_handle(),
                int(clip_id),
                c_events if count else None,
                ctypes.c_size_t(count),
            )
        )

    def import_smf(self, data: bytes) -> int:
        """Import an in-memory SMF buffer; return the first added clip id.

        Raises ``ValueError`` (via the error code) on malformed input, never
        crashing.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_smf(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def export_smf(self) -> bytes:
        """Export the project's tempo map + MIDI clips to an SMF byte buffer."""
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_smf(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out or out_len.value == 0:
                return b""
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_bytes(out)

    def import_clip_file(self, data: bytes) -> int:
        """Import an in-memory MIDI 2.0 Clip File (``SMF2CLIP``); return the
        first added clip id.

        Unlike :meth:`import_smf`, the UMP-based container preserves MIDI 2.0
        channel-voice messages (16-bit velocity, 32-bit CC, per-note /
        registered controllers, bank-valid Program Change) without loss. Raises
        ``ValueError`` on malformed input, never crashing.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_clip_file(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def export_clip_file(self) -> bytes:
        """Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
        (``SMF2CLIP``) byte buffer.

        MIDI 2.0-only events are written WITHOUT loss — prefer this over
        :meth:`export_smf` when MIDI 2.0 fidelity matters.
        """
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_clip_file(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out or out_len.value == 0:
                return b""
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_bytes(out)

    def set_program(self, clip_id: int, program: int, bank: int = -1) -> None:
        """Set a MIDI clip's channel-0 program / bank at source PPQ 0.

        ``bank`` defaults to ``-1`` (no Bank Select emitted), matching
        :meth:`set_program_on_channel`; pass ``>= 0`` to emit a Bank Select.
        """
        _check(
            _get_lib().sonare_project_set_program(
                self._require_handle(), int(clip_id), int(program), int(bank)
            )
        )

    def set_program_on_channel(
        self, clip_id: int, group: int, channel: int, program: int, bank: int = -1
    ) -> None:
        """Set a MIDI clip's program / bank for one UMP group and channel."""
        _check(
            _get_lib().sonare_project_set_program_on_channel(
                self._require_handle(),
                int(clip_id),
                int(group),
                int(channel),
                int(program),
                int(bank),
            )
        )

    def set_midi_fx(self, clip_id: int, config_json: str) -> None:
        """Configure and apply a clip's MIDI-FX chain from JSON."""
        _check(
            _get_lib().sonare_project_set_midi_fx(
                self._require_handle(),
                int(clip_id),
                config_json.encode("utf-8"),
            )
        )

    def bake_midi_fx(self, clip_id: int, config_json: str) -> None:
        """Destructively bake a clip's MIDI-FX chain from JSON.

        Canonical name matching the Node / WASM ``bakeMidiFx`` surface; unlike
        :meth:`set_midi_fx` (a non-destructive insert) this rewrites the clip's
        stored events in place.
        """
        _check(
            _get_lib().sonare_project_bake_midi_fx(
                self._require_handle(),
                int(clip_id),
                config_json.encode("utf-8"),
            )
        )

    def validate_midi_notes(self, clip_id: int) -> NotePairValidation:
        """Check a MIDI clip for hanging / unmatched notes before bouncing."""
        result = SonareNotePairValidation()
        _check(
            _get_lib().sonare_project_validate_midi_notes(
                self._require_handle(),
                int(clip_id),
                ctypes.byref(result),
            )
        )
        return NotePairValidation(
            ok=bool(result.ok),
            unmatched_note_ons=int(result.unmatched_note_ons),
            unmatched_note_offs=int(result.unmatched_note_offs),
        )

    @staticmethod
    def midi_note_on(
        ppq: float, group: int, channel: int, note: int, velocity: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-on event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_on", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_note_off(
        ppq: float, group: int, channel: int, note: int, velocity: int = 0
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-off event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_off", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_cc(
        ppq: float, group: int, channel: int, controller: int, value: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 control-change event tuple."""
        return _midi_event_tuple("sonare_midi_cc", ppq, group, channel, controller, value)

    @staticmethod
    def midi_poly_pressure(
        ppq: float, group: int, channel: int, note: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 poly-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_poly_pressure", ppq, group, channel, note, pressure)

    @staticmethod
    def midi_program(ppq: float, group: int, channel: int, program: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 program-change event tuple."""
        return _midi_event_tuple("sonare_midi_program", ppq, group, channel, program)

    @staticmethod
    def midi_channel_pressure(
        ppq: float, group: int, channel: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 channel-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_channel_pressure", ppq, group, channel, pressure)

    @staticmethod
    def midi_pitch_bend(ppq: float, group: int, channel: int, bend: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 pitch-bend event tuple (`bend` is unsigned 14-bit)."""
        return _midi_event_tuple("sonare_midi_pitch_bend", ppq, group, channel, bend)

    # -- MIDI naming / GM tables (static-lifetime lookups) ------------------

    @staticmethod
    def gm_instrument_name(program: int) -> str | None:
        """GM Level 1 instrument name for ``program`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_gm_instrument_name(int(program))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_program_for_name(name: str | None) -> int:
        """Reverse GM instrument lookup; ``-1`` when unknown / ``None``."""
        return int(
            _get_lib().sonare_midi_gm_program_for_name(name.encode("utf-8") if name else None)
        )

    @staticmethod
    def gm_family_name(family: int) -> str | None:
        """GM family name for ``family`` [0,15], or ``None``."""
        r = _get_lib().sonare_midi_gm_family_name(int(family))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_family_first_program(family: int) -> int:
        """First GM program in ``family`` [0,15], or ``-1``."""
        return int(_get_lib().sonare_midi_gm_family_first_program(int(family)))

    @staticmethod
    def gm2_instrument_name(bank_lsb: int, program: int) -> str | None:
        """GM2 melodic instrument name for ``bank_lsb`` + ``program``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_instrument_name(int(bank_lsb), int(program))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_drum_name(note: int) -> str | None:
        """GM drum name for ``note`` [35,81], or ``None``."""
        r = _get_lib().sonare_midi_gm_drum_name(int(note))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_drum_note_for_name(name: str | None) -> int:
        """Reverse GM drum lookup; ``-1`` when unknown / ``None``."""
        return int(
            _get_lib().sonare_midi_gm_drum_note_for_name(name.encode("utf-8") if name else None)
        )

    @staticmethod
    def gm2_drum_set_name(bank_lsb: int) -> str | None:
        """GM2 drum-set name for ``bank_lsb``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_set_name(int(bank_lsb))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm2_drum_name(bank_lsb: int, note: int) -> str | None:
        """GM2 drum name for ``bank_lsb`` + ``note``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_name(int(bank_lsb), int(note))
        return r.decode("utf-8") if r else None

    @staticmethod
    def midi_cc_name(controller: int) -> str | None:
        """Standard MIDI CC name for ``controller`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_cc_name(int(controller))
        return r.decode("utf-8") if r else None

    @staticmethod
    def midi_cc_index_for_name(name: str | None) -> int:
        """Reverse standard MIDI CC lookup; ``-1`` when unknown / ``None``."""
        return int(_get_lib().sonare_midi_cc_index_for_name(name.encode("utf-8") if name else None))

    @staticmethod
    def per_note_controller_name(index: int) -> str | None:
        """MIDI 2.0 registered per-note controller name for ``index``, or ``None``."""
        r = _get_lib().sonare_midi_per_note_controller_name(int(index))
        return r.decode("utf-8") if r else None

    # -- MIDI pure conversion helpers ---------------------------------------

    @staticmethod
    def midi_bank_program(
        ppq: float,
        group: int,
        channel: int,
        bank_msb: int,
        bank_lsb: int,
        program: int,
    ) -> list[tuple[float, int, int]]:
        """Lower a bank/program selection to MIDI 1.0 bank-select + program events.

        Returns the emitted ``(ppq, data0, data1)`` events (Bank MSB CC, Bank LSB
        CC, then Program Change) at ``ppq``.
        """
        lib = _get_lib()
        events = (SonareMidiEventPod * 3)()
        out_count = ctypes.c_size_t()
        _check(
            lib.sonare_midi_bank_program(
                float(ppq),
                int(group),
                int(channel),
                int(bank_msb),
                int(bank_lsb),
                int(program),
                events,
                ctypes.c_size_t(3),
                ctypes.byref(out_count),
            )
        )
        return [
            (float(events[i].ppq), int(events[i].data0), int(events[i].data1))
            for i in range(int(out_count.value))
        ]

    @staticmethod
    def midi_route_events(
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]],
        config: Mapping[str, int] | None = None,
    ) -> MidiRouteResult:
        """Route events through the RT MidiRouter filter / remap / thru logic.

        ``config`` is an optional mapping with ``filter_group`` /
        ``filter_channel`` (``-1`` = any), ``remap_channel`` (``-1`` = no remap)
        and ``thru`` (1 = pass non-matching events through, default).
        """
        lib = _get_lib()
        rows = list(events)
        n = len(rows)
        c_in = (SonareMidiEventPod * n)()
        for i, ev in enumerate(rows):
            seq = tuple(ev)
            if len(seq) < 3:
                raise ValueError(f"events[{i}] must contain (ppq, data0, data1)")
            c_in[i].ppq = _validate_midi_event_ppq(seq[0], f"events[{i}].ppq")
            c_in[i].data0 = _validate_midi_event_word(seq[1], f"events[{i}].data0")
            c_in[i].data1 = _validate_midi_event_word(seq[2], f"events[{i}].data1")
        cfg_map: Mapping[str, int] = config or {}
        cfg = SonareMidiRouteConfig(
            filter_group=int(cfg_map.get("filter_group", -1)),
            filter_channel=int(cfg_map.get("filter_channel", -1)),
            remap_channel=int(cfg_map.get("remap_channel", -1)),
            thru=int(cfg_map.get("thru", 1)),
        )
        out = (SonareMidiEventPod * n)() if n else None
        out_count = ctypes.c_size_t()
        overflowed = ctypes.c_int()
        overflow_count = ctypes.c_uint32()
        _check(
            lib.sonare_midi_route_events(
                c_in if n else None,
                ctypes.c_size_t(n),
                ctypes.byref(cfg),
                out,
                ctypes.c_size_t(n),
                ctypes.byref(out_count),
                ctypes.byref(overflowed),
                ctypes.byref(overflow_count),
            )
        )
        routed = [
            (float(out[i].ppq), int(out[i].data0), int(out[i].data1))
            for i in range(int(out_count.value))
        ]
        return MidiRouteResult(
            events=routed,
            overflowed=bool(overflowed.value),
            overflow_count=int(overflow_count.value),
        )

    @staticmethod
    def midi_cc_learn(
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]],
        param_id: int,
        min_value: float = 0.0,
        max_value: float = 1.0,
        min_movement: int = 0,
    ) -> MidiCcBinding | None:
        """Run MIDI learn over ``events`` and return the learned binding.

        Returns ``None`` when no binding is learned (native
        ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(events)
        n = len(rows)
        c_in = (SonareMidiEventPod * n)()
        for i, ev in enumerate(rows):
            seq = tuple(ev)
            if len(seq) < 3:
                raise ValueError(f"events[{i}] must contain (ppq, data0, data1)")
            c_in[i].ppq = _validate_midi_event_ppq(seq[0], f"events[{i}].ppq")
            c_in[i].data0 = _validate_midi_event_word(seq[1], f"events[{i}].data0")
            c_in[i].data1 = _validate_midi_event_word(seq[2], f"events[{i}].data1")
        out_binding = SonareMidiCcBinding()
        rc = lib.sonare_midi_cc_learn(
            c_in if n else None,
            ctypes.c_size_t(n),
            ctypes.c_uint32(int(param_id) & 0xFFFFFFFF),
            ctypes.c_float(float(min_value)),
            ctypes.c_float(float(max_value)),
            ctypes.c_uint8(int(min_movement) & 0xFF),
            ctypes.byref(out_binding),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return _cc_binding_from_c(out_binding)

    @staticmethod
    def midi_cc_to_breakpoint(
        bindings: Sequence[MidiCcBinding | Mapping[str, object]],
        event: tuple[float, int, int] | Sequence[float],
    ) -> tuple[float, float, int] | None:
        """Convert one CC ``event`` to an automation breakpoint via a binding table.

        Returns ``(ppq, value, curve_to_next)`` or ``None`` when the event does
        not match any binding (native ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        seq = tuple(event)
        if len(seq) < 3:
            raise ValueError("event must contain (ppq, data0, data1)")
        ev = SonareMidiEventPod(
            ppq=_validate_midi_event_ppq(seq[0], "event.ppq"),
            data0=_validate_midi_event_word(seq[1], "event.data0"),
            data1=_validate_midi_event_word(seq[2], "event.data1"),
        )
        pt = SonareAutomationPoint()
        rc = lib.sonare_midi_cc_to_breakpoint(
            c_bindings if m else None,
            ctypes.c_size_t(m),
            ctypes.byref(ev),
            ctypes.byref(pt),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return (float(pt.ppq), float(pt.value), int(pt.curve_to_next))

    @staticmethod
    def midi_param_to_cc(
        bindings: Sequence[MidiCcBinding | Mapping[str, object]],
        param_id: int,
        unit_value: float,
        group: int,
        ppq: float = 0.0,
    ) -> tuple[float, int, int] | None:
        """Convert an automation parameter value back to a CC event.

        Returns ``(ppq, data0, data1)`` or ``None`` when ``param_id`` is not
        bound (native ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        out_event = SonareMidiEventPod()
        rc = lib.sonare_midi_param_to_cc(
            c_bindings if m else None,
            ctypes.c_size_t(m),
            ctypes.c_uint32(int(param_id) & 0xFFFFFFFF),
            ctypes.c_float(float(unit_value)),
            ctypes.c_uint8(int(group) & 0xFF),
            ctypes.c_double(float(ppq)),
            ctypes.byref(out_event),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return (float(out_event.ppq), int(out_event.data0), int(out_event.data1))

    # -- MIR ----------------------------------------------------------------

    def auto_tempo(self, audio: Sequence[float] | np.ndarray, sample_rate: int) -> float:
        """Detect tempo from a mono buffer and install it (undoable).

        Returns the primary BPM estimate.
        """
        c_array, length = _to_c_float_array(audio)
        out_bpm = ctypes.c_float()
        _check(
            _get_lib().sonare_project_auto_tempo(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
                ctypes.byref(out_bpm),
            )
        )
        return float(out_bpm.value)

    def snap_to_grid(self, ppq: float, strength: float = 1.0) -> float:
        """Snap a PPQ coordinate to the nearest beat of the project grid.

        ``strength`` in ``[0, 1]`` (0 = no snap, 1 = exact grid line).
        """
        out_ppq = ctypes.c_double()
        _check(
            _get_lib().sonare_project_snap_to_grid(
                self._require_handle(),
                float(ppq),
                float(strength),
                ctypes.byref(out_ppq),
            )
        )
        return float(out_ppq.value)

    def annotate_keys(
        self, keys: Sequence[tuple[float, float, int, int]] | Sequence[Sequence[float]]
    ) -> None:
        """Replace the project's key annotation stream via an undoable command.

        Each key is ``(start_ppq, end_ppq, tonic_pc, mode)`` where ``tonic_pc``
        is 0..11 (C=0) or 255 unknown and ``mode`` is the KeyMode ordinal
        (0 unknown, 1 major, 2 minor, 3 dorian, 4 phrygian, 5 lydian,
        6 mixolydian, 7 locrian). Pass an empty sequence to clear.
        """
        rows = list(keys)
        count = len(rows)
        c_keys = (SonareProjectKeySegment * count)() if count else None
        for i, k in enumerate(rows):
            seq = tuple(k)
            if len(seq) < 4:
                raise ValueError(f"keys[{i}] must contain (start_ppq, end_ppq, tonic_pc, mode)")
            c_keys[i].start_ppq = float(seq[0])
            c_keys[i].end_ppq = float(seq[1])
            c_keys[i].tonic_pc = int(seq[2])
            c_keys[i].mode = int(seq[3])
        _check(
            _get_lib().sonare_project_annotate_keys(
                self._require_handle(), c_keys, ctypes.c_size_t(count)
            )
        )

    def annotate_chords(self, chords: Sequence[dict[str, object]]) -> None:
        """Replace the project's chord-symbol annotation stream (undoable command).

        Each chord is a mapping with keys ``start_ppq``, ``end_ppq``, ``root_pc``
        (0..11 / 255), ``quality`` (ChordQuality ordinal), optional
        ``extensions`` (iterable of semitone ints, up to 8), ``slash_bass_pc``
        (default 255), ``roman_numeral`` (optional str) and ``modulation_boundary``
        (bool). Pass an empty sequence to clear.
        """
        rows = list(chords)
        count = len(rows)
        c_chords = (SonareProjectChordSymbol * count)() if count else None
        # Keep extension arrays and roman-numeral byte strings alive for the call.
        backing: list[object] = []
        for i, c in enumerate(rows):
            ext = list(c.get("extensions", []) or [])
            ext_count = len(ext)
            c_ext = (
                (ctypes.c_uint8 * ext_count)(*[int(e) & 0xFF for e in ext]) if ext_count else None
            )
            backing.append(c_ext)
            roman = c.get("roman_numeral")
            roman_bytes = roman.encode("utf-8") if isinstance(roman, str) and roman else None
            backing.append(roman_bytes)
            c_chords[i].start_ppq = float(c["start_ppq"])
            c_chords[i].end_ppq = float(c["end_ppq"])
            c_chords[i].root_pc = int(c.get("root_pc", 255))
            c_chords[i].quality = int(c.get("quality", 0))
            c_chords[i].extensions = c_ext
            c_chords[i].extension_count = ctypes.c_size_t(ext_count)
            c_chords[i].slash_bass_pc = int(c.get("slash_bass_pc", 255))
            c_chords[i].roman_numeral = roman_bytes
            c_chords[i].modulation_boundary = 1 if c.get("modulation_boundary") else 0
        _check(
            _get_lib().sonare_project_annotate_chords(
                self._require_handle(), c_chords, ctypes.c_size_t(count)
            )
        )
        del backing

    # -- assist sidecars ----------------------------------------------------

    def set_assist_sidecar(
        self,
        module_id: str,
        payload: bytes,
        *,
        schema_version: int = 0,
        target_track_id: int = 0,
        region_start_ppq: float = 0.0,
        region_end_ppq: float = 0.0,
    ) -> None:
        """Add or update an opaque assist sidecar (undoable command).

        Sidecars sharing ``module_id`` + ``target_track_id`` + region scope are
        replaced; otherwise a new one is appended. ``module_id`` must be
        non-empty and ``payload`` is copied opaque bytes.
        """
        if not module_id:
            raise ValueError("module_id must be a non-empty string")
        raw = bytes(payload)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        _check(
            _get_lib().sonare_project_set_assist_sidecar(
                self._require_handle(),
                module_id.encode("utf-8"),
                int(schema_version),
                int(target_track_id),
                float(region_start_ppq),
                float(region_end_ppq),
                buf,
                ctypes.c_size_t(len(raw)),
            )
        )

    def assist_sidecar_count(self) -> int:
        """Number of assist sidecars currently stored on the project."""
        return int(_get_lib().sonare_project_assist_sidecar_count(self._require_handle()))

    def get_assist_sidecar(self, index: int) -> AssistSidecar:
        """Read one assist sidecar by stable project order as an :class:`AssistSidecar`."""
        lib = _get_lib()
        raw = SonareProjectAssistSidecar()
        _check(
            lib.sonare_project_get_assist_sidecar(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        try:
            module_id = raw.module_id.decode("utf-8") if raw.module_id else ""
            payload_len = int(raw.payload_len)
            payload = (
                ctypes.string_at(raw.payload, payload_len) if raw.payload and payload_len else b""
            )
            return AssistSidecar(
                module_id=module_id,
                schema_version=int(raw.schema_version),
                target_track_id=int(raw.target_track_id),
                region_start_ppq=float(raw.region_start_ppq),
                region_end_ppq=float(raw.region_end_ppq),
                payload=payload,
            )
        finally:
            lib.sonare_project_free_assist_sidecar(ctypes.byref(raw))

    def assist_sidecars(self) -> list[AssistSidecar]:
        """Return all stored assist sidecars as a list of :class:`AssistSidecar`."""
        return [self.get_assist_sidecar(i) for i in range(self.assist_sidecar_count())]

    # -- project getters / setters ------------------------------------------

    def get_sample_rate(self) -> float:
        """Return the project sample rate in Hz."""
        out = ctypes.c_double()
        _check(_get_lib().sonare_project_get_sample_rate(self._require_handle(), ctypes.byref(out)))
        return float(out.value)

    def get_overlap_policy(self) -> int:
        """Return the project's clip-overlap policy ordinal."""
        out = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_get_overlap_policy(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def set_overlap_policy(self, policy: int) -> None:
        """Set the project's clip-overlap policy ordinal."""
        _check(_get_lib().sonare_project_set_overlap_policy(self._require_handle(), int(policy)))

    def set_marker(self, marker_id: int, ppq: float, name: str) -> int:
        """Add or replace a marker; return its (possibly newly allocated) id.

        Pass ``marker_id == 0`` to allocate a new marker id.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker(
                self._require_handle(),
                int(marker_id),
                float(ppq),
                name.encode("utf-8") if name is not None else None,
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def set_marker_ex(self, marker: ProjectMarker) -> int:
        """Add or replace a marker from a full :class:`ProjectMarker`.

        Carries the marker ``kind`` and (for the key-signature kind) the
        structured key. Pass ``marker.id == 0`` to allocate a new marker id;
        the affected id is returned.
        """
        raw = SonareProjectMarker()
        raw.id = int(marker.id)
        raw.kind = int(marker.kind) & 0xFF
        raw.key_fifths = int(marker.key_fifths)
        raw.key_minor = 1 if marker.key_minor else 0
        raw.ppq = float(marker.ppq)
        raw.name = _marker_name_bytes(marker.name)
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker_ex(
                self._require_handle(), ctypes.byref(raw), ctypes.byref(out_id)
            )
        )
        return int(out_id.value)

    def marker_by_index(self, index: int) -> ProjectMarker:
        """Read a marker by 0-based stored index, including its kind / key."""
        raw = SonareProjectMarker()
        _check(
            _get_lib().sonare_project_marker_by_index(
                self._require_handle(), ctypes.c_size_t(int(index)), ctypes.byref(raw)
            )
        )
        return ProjectMarker(
            id=int(raw.id),
            ppq=float(raw.ppq),
            name=bytes(raw.name).split(b"\x00", 1)[0].decode("utf-8", "replace"),
            kind=int(raw.kind),
            key_fifths=int(raw.key_fifths),
            key_minor=bool(raw.key_minor),
        )

    def set_mixer_scene_json(self, scene_json: str) -> None:
        """Replace the project's mixer scene from scene JSON."""
        _check(
            _get_lib().sonare_project_set_mixer_scene_json(
                self._require_handle(),
                scene_json.encode("utf-8"),
            )
        )

    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Replace the project's tempo map.

        Each segment is a mapping (``start_ppq`` / ``bpm`` / optional
        ``start_sample`` / ``end_bpm``) or a tuple
        ``(start_ppq, bpm, start_sample=ignored, end_bpm=0.0)``. ``start_sample``
        is accepted for ABI/source compatibility but ignored; sample positions
        are derived during normalization. ``end_bpm`` 0 means a constant-tempo
        segment. Pass an empty sequence to clear.
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
            _get_lib().sonare_project_set_tempo_segments(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
            )
        )

    def set_time_signatures(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Replace the project's time-signature map.

        Each segment is a mapping (``start_ppq`` / ``numerator`` /
        ``denominator``) or a tuple ``(start_ppq, numerator, denominator)``.
        Pass an empty sequence to clear.
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
            _get_lib().sonare_project_set_time_signatures(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
            )
        )

    def _count(self, fn_name: str) -> int:
        out = ctypes.c_size_t()
        _check(getattr(_get_lib(), fn_name)(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def marker_count(self) -> int:
        """Number of timeline markers in the project."""
        return self._count("sonare_project_marker_count")

    def source_count(self) -> int:
        """Number of registered audio sources in the project."""
        return self._count("sonare_project_source_count")

    def tempo_segment_count(self) -> int:
        """Number of tempo-map segments in the project."""
        return self._count("sonare_project_tempo_segment_count")

    def time_signature_count(self) -> int:
        """Number of time-signature segments in the project."""
        return self._count("sonare_project_time_signature_count")

    def track_count(self) -> int:
        """Number of tracks in the project."""
        return self._count("sonare_project_track_count")

    # -- compile / render ---------------------------------------------------

    def last_bounce_compile_result(self) -> ProjectCompileResult:
        """Return the compile result captured by the most recent bounce.

        Mirrors :meth:`compile`'s structured :class:`ProjectCompileResult` but
        reads the timeline + diagnostics recorded during the last
        ``bounce*`` call instead of recompiling.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(
            lib.sonare_project_last_bounce_compile_result(
                self._require_handle(), ctypes.byref(result)
            )
        )
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            message_lines = messages.splitlines()
            diagnostics = tuple(
                ProjectDiagnostic(
                    code=int(result.diagnostics[i].code),
                    severity=int(result.diagnostics[i].severity),
                    target_id=int(result.diagnostics[i].target_id),
                    message=message_lines[i] if i < len(message_lines) else "",
                )
                for i in range(int(result.diagnostic_count))
            )
            return ProjectCompileResult(has_timeline, messages, diagnostics)
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))

    def compile(self) -> ProjectCompileResult:
        """Compile the project into an RT-readable timeline.

        Returns a :class:`ProjectCompileResult` with ``has_timeline``,
        ``messages``, and structured ``diagnostics``. The result remains
        iterable as ``(has_timeline, messages)`` for legacy callers. Never
        throws on bad project content; it surfaces error diagnostics.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(lib.sonare_project_compile(self._require_handle(), ctypes.byref(result)))
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            message_lines = messages.splitlines()
            diagnostics = tuple(
                ProjectDiagnostic(
                    code=int(result.diagnostics[i].code),
                    severity=int(result.diagnostics[i].severity),
                    target_id=int(result.diagnostics[i].target_id),
                    message=message_lines[i] if i < len(message_lines) else "",
                )
                for i in range(int(result.diagnostic_count))
            )
            return ProjectCompileResult(has_timeline, messages, diagnostics)
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))

    def bounce(
        self,
        *,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project offline to interleaved float audio.

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic: the
        same project + options yields a bit-identical array within one build.
        Zero-valued options take native defaults (project sample rate, 2
        channels, block 128). Omitting ``total_frames`` (or passing <= 0) makes
        the native layer auto-derive the render length from the arrangement.

        Note: MIDI tracks render to silence here because no instrument is bound;
        use :meth:`bounce_with_builtin_instrument` to render MIDI through the
        built-in synth.
        """
        lib = _get_lib()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_bounce(
                self._require_handle(),
                ctypes.byref(options),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            # The C ABI returns a non-null buffer even when out_len == 0 (a
            # sentinel `new float[1]`); free it unconditionally to avoid leaking
            # on empty bounces. _from_c_float_array already copied the samples.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)

    def bounce_with_builtin_instrument(
        self,
        instrument: BuiltinSynthConfig | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, BuiltinSynthConfig]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the built-in synth.

        Unlike :meth:`bounce`, MIDI tracks routed to a bound destination render
        through the built-in polyphonic oscillator synth, so a MIDI-only
        arrangement produces audible (non-silent) output without the caller
        supplying its own instrument callbacks.

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass
                ``None`` for the default sine patch. Ignored when ``instruments``
                is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames: Render length in frames; <= 0 auto-derives the length
                from the arrangement (musical end + the synth's release tail).
            block_size / num_channels / sample_rate / instrument_latency_samples:
                As :meth:`bounce` (0 takes native defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + patch.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_builtin_instruments"):
            raise RuntimeError("libsonare was built without the built-in instrument bounce ABI")
        if instruments is None:
            patch = instrument if instrument is not None else BuiltinSynthConfig()
            bindings = [(int(destination_id), patch)]
        else:
            bindings = [(int(dst), patch) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareBuiltinInstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].config = patch._to_c()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_bounce_with_builtin_instruments(
                self._require_handle(),
                ctypes.byref(options),
                c_bindings if count else None,
                ctypes.c_size_t(count),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            # Free unconditionally: the C ABI returns a non-null sentinel buffer
            # even when out_len == 0, so a `> 0` guard would leak it.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)

    def bounce_with_synth_instrument(
        self,
        instrument: SynthPatch | str | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, SynthPatch | str]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the NativeSynth.

        Like :meth:`bounce_with_builtin_instrument`, but each bound destination
        renders through the patch-driven NativeSynth — the full synthesizer
        (subtractive / FM / Karplus-Strong / modal / additive / percussion /
        extended-waveguide-piano engines plus the realism layer).

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass a
                :class:`SynthPatch`, a preset name string (``"saw-lead"`` or
                ``"va:saw-lead"``; see :func:`synth_preset_names`), or ``None``
                for the default subtractive patch. Ignored when ``instruments``
                is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0). Unlike the
                JS helpers, Python keeps this as a binding argument instead of a
                :class:`SynthPatch` field.
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames: Render length in frames; <= 0 auto-derives the length
                from the arrangement (musical end + the patch's release tail).
            block_size / num_channels / sample_rate / instrument_latency_samples:
                As :meth:`bounce` (0 takes native defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + patch. Raises :class:`SonareError` for an
        unknown preset name.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_synth_instruments"):
            raise RuntimeError("libsonare was built without the NativeSynth bounce ABI")
        if instruments is None:
            bindings = [(int(destination_id), _synth_patch_arg(instrument))]
        else:
            bindings = [(int(dst), _synth_patch_arg(patch)) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareSynthInstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].patch = patch._to_c()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_bounce_with_synth_instruments(
                self._require_handle(),
                ctypes.byref(options),
                c_bindings if count else None,
                ctypes.c_size_t(count),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            # Free unconditionally: the C ABI returns a non-null sentinel buffer
            # even when out_len == 0, so a `> 0` guard would leak it.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)

    def load_soundfont(self, data: bytes | bytearray | memoryview) -> None:
        """Load (parse) SoundFont 2 bytes into the project.

        The presets / instruments / sample headers and the sample PCM (decoded
        to a float pool) replace any previously loaded SoundFont; the input
        buffer is not referenced after the call. Raises :class:`SonareError`
        on malformed input (the previous SoundFont is kept).
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_load_soundfont"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        buf = bytes(data)
        if not buf:
            raise ValueError("SoundFont data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_project_load_soundfont(
                self._require_handle(), c_data, ctypes.c_size_t(len(buf))
            )
        )

    def clear_soundfont(self) -> None:
        """Release the project's loaded SoundFont (no-op when none is loaded)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_clear_soundfont"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        _check(lib.sonare_project_clear_soundfont(self._require_handle()))

    def soundfont_preset_count(self) -> int:
        """Number of presets in the loaded SoundFont (0 when none is loaded)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_soundfont_preset_count"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        out = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_preset_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def soundfont_manifest(self) -> list[Sf2ProgramStatus]:
        """Enumerate the programs the arrangement plays and their backends.

        Returns one :class:`Sf2ProgramStatus` per (channel, bank, program)
        combination a note actually plays through, in first-use order. Each
        entry reports whether it resolves in the loaded SoundFont
        (:data:`SOURCE_BACKEND_SF2`, GS variation/drum fallbacks included) or
        would fall back to the built-in synth (:data:`SOURCE_BACKEND_SYNTH`).
        Without a loaded SoundFont every entry is a synth fallback.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_soundfont_manifest"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        handle = self._require_handle()
        total = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_manifest(handle, None, 0, ctypes.byref(total)))
        count = int(total.value)
        if count == 0:
            return []
        entries = (SonareSf2ProgramStatus * count)()
        _check(
            lib.sonare_project_soundfont_manifest(
                handle, entries, ctypes.c_size_t(count), ctypes.byref(total)
            )
        )
        return [
            Sf2ProgramStatus(
                channel=int(e.channel),
                bank=int(e.bank),
                program=int(e.program),
                backend=int(e.backend),
                preset_name=e.preset_name.decode("utf-8", errors="replace"),
            )
            for e in entries[: min(count, int(total.value))]
        ]

    def bounce_with_sf2_instrument(
        self,
        instrument: Sf2InstrumentConfig | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, Sf2InstrumentConfig]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the SF2 player.

        Like :meth:`bounce_with_builtin_instrument`, but each bound destination
        renders through a GS-compatible SoundFont player fed by the project's
        loaded SoundFont (:meth:`load_soundfont`): 16 MIDI channels per player,
        channel 10 drums via bank 128, GS NRPN part edits and GS/GM SysEx
        resets honored. Programs the SoundFont does not cover — including
        bouncing with no SoundFont loaded at all — play through the built-in
        synthesizer GM fallback bank (the data-free floor; see
        :meth:`soundfont_manifest` for the per-program backend).

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass
                ``None`` for the default patch. Ignored when ``instruments`` is
                given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames / block_size / num_channels / sample_rate /
                instrument_latency_samples: As :meth:`bounce` (0 takes native
                defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + SoundFont + patch.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_sf2_instruments"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        if instruments is None:
            patch = instrument if instrument is not None else Sf2InstrumentConfig()
            bindings = [(int(destination_id), patch)]
        else:
            bindings = [(int(dst), patch) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareSf2InstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].config = patch._to_c()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_bounce_with_sf2_instruments(
                self._require_handle(),
                ctypes.byref(options),
                c_bindings if count else None,
                ctypes.c_size_t(count),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            # Free unconditionally: the C ABI returns a non-null sentinel buffer
            # even when out_len == 0, so a `> 0` guard would leak it.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)

    def bounce_with_instruments(
        self,
        instrument: ExternalInstrument | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, ExternalInstrument]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through host instruments.

        Unlike :meth:`bounce_with_builtin_instrument`, each bound destination is
        rendered by a caller-supplied :class:`ExternalInstrument` (a real
        sampler/synth), letting MIDI tracks route through your own sound source.
        The instrument's callbacks run synchronously on the calling thread for
        the duration of this method, so no thread-safety machinery is required.

        Args:
            instrument: Instrument bound to ``destination_id`` (default 0).
                Ignored when ``instruments`` is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, instrument)``
                bindings, overriding ``instrument`` / ``destination_id``.
                    total_frames / block_size / num_channels / sample_rate /
                        instrument_latency_samples: As :meth:`bounce` (0 takes native
                        defaults; an instrument's own ``latency_samples`` attribute adds
                        per-instrument PDC; ``tail_samples`` extends auto-length
                        bounces).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + instrument behaviour. Raises the first
        exception raised inside any instrument callback.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_instruments"):
            raise RuntimeError("libsonare was built without the external-instrument bounce ABI")
        if instruments is None:
            if instrument is None:
                raise ValueError("bounce_with_instruments requires `instrument` or `instruments`")
            bindings = [(int(destination_id), instrument)]
        else:
            bindings = [(int(dst), inst) for dst, inst in instruments]
        count = len(bindings)

        errors: list[BaseException] = []
        keepalive: list[object] = []
        c_bindings = (SonareInstrumentBinding * count)()
        for i, (dst, inst) in enumerate(bindings):
            if not callable(getattr(inst, "render", None)):
                raise TypeError(
                    "each instrument must provide a render(channels, num_frames) method"
                )
            c_bindings[i].destination_id = dst
            c_bindings[i].callbacks = _make_instrument_callbacks(inst, errors, keepalive)

        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        rc = lib.sonare_project_bounce_with_instruments(
            self._require_handle(),
            ctypes.byref(options),
            c_bindings if count else None,
            ctypes.c_size_t(count),
            ctypes.byref(out),
            ctypes.byref(out_len),
        )
        # `keepalive` pins the ctypes trampolines until the bounce returns.
        keepalive.clear()
        try:
            _check(rc)
            if errors:
                raise errors[0]
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)
