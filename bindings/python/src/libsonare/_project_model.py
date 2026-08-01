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
from typing import TYPE_CHECKING, Any, Protocol, SupportsFloat, cast

import numpy as np

if TYPE_CHECKING:
    from ._project import Project

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


def _fade_curve_value(curve: str | int) -> int:
    return _resolve_enum(curve, _FADE_CURVE_NAMES, "fade curve")


def _loop_mode_value(mode: str | int) -> int:
    return _resolve_enum(mode, _LOOP_MODE_NAMES, "loop mode")


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
