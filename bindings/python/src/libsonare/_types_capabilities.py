"""Capability and catalogue shapes the library reports about itself.

These describe what the loaded build can do -- ABI versions, compiled-in
features, decoders and the mastering processor catalogue -- rather than the
result of analysing any audio. Every shape is a frozen dataclass with
snake_case fields; :func:`_from_wire` builds one from the library's JSON
document, whose keys are camelCase, and ignores a key no field names.
"""

from __future__ import annotations

import types
from collections.abc import Hashable
from dataclasses import dataclass, fields, is_dataclass
from functools import cache
from typing import Any, Literal, TypeVar, Union, cast, get_args, get_origin, get_type_hints

MasteringProcessorKind = Literal["realtime", "offline", "pair"]
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
MasteringPresetKind = Literal["mastering", "restoration"]
MasteringInsertParamUnit = Literal[
    "dB",
    "dBFS",
    "LUFS",
    "Hz",
    "ms",
    "s",
    "samples",
    "m",
    "cm",
    "mm",
    "deg",
    "percent",
    "degC",
    "V",
    "inPerSec",
    "dBPerOct",
    "semitones",
    "cents",
    "ratio",
    "bits",
    "count",
    "N",
    "midiNote",
    "none",
]
MasteringInsertParamScale = Literal["linear", "log"]
MasteringInsertParamRelation = Literal["lt", "le", "gt", "ge"]


@dataclass(frozen=True)
class CapabilitiesAbi:
    """ABI versions reported by :func:`libsonare.capabilities`."""

    project: int
    """Project document ABI version."""
    engine: int
    """Realtime engine ABI version."""


@dataclass(frozen=True)
class CapabilitiesFeatures:
    """Build options reported by :func:`libsonare.capabilities`.

    Every compile-time feature option has a field, ``False`` when the option
    was off. An entry point of a feature that was not built stays exported
    and raises ``NotSupported``, so probing for a symbol tells a host nothing;
    these fields are the probe.
    """

    mastering: bool
    """Mastering chain, processors and presets."""
    mixing: bool
    """Mixer, scenes and buses."""
    mixing_assistant: bool
    """Mixing assistant, which can be dropped apart from ``mixing``."""
    graph: bool
    """Processing graph engine."""
    fx: bool
    """Creative insert effects."""
    ffmpeg: bool
    """FFmpeg-backed decoders."""
    instrument_param_automation: bool
    """Automation of instrument parameters."""
    arrangement: bool
    """Arrangement editing."""
    project: bool
    """Project documents."""
    acoustic_sim: bool
    """Room acoustics simulation."""
    playback: bool
    """Device playback."""
    pitch_editor: bool
    """Pitch editor."""
    vocal_edit: bool
    """Vocal editing."""
    voice_changer: bool
    """Realtime voice changer."""
    assist: bool
    """Assist modules."""


@dataclass(frozen=True)
class CapabilitiesDecode:
    """Built-in and FFmpeg-backed decoder lists for the loaded library."""

    builtin: tuple[str, ...]
    """File extensions the built-in decoders read."""
    ffmpeg: tuple[str, ...]
    """File extensions read through FFmpeg; empty without FFmpeg."""


@dataclass(frozen=True)
class Capabilities:
    """Build and runtime descriptor returned by :func:`libsonare.capabilities`."""

    version: str
    """Library version string."""
    abi: CapabilitiesAbi
    """ABI versions."""
    platform: str
    """Target platform, such as ``"darwin-arm64"``."""
    features: CapabilitiesFeatures
    """Compiled-in build options."""
    decode: CapabilitiesDecode
    """Decodable file extensions."""
    simd: str
    """SIMD instruction set the build uses."""
    hardware_concurrency: int
    """Hardware threads the host reports."""


@dataclass(frozen=True)
class MasteringInsertParamChoice:
    """One accepted value of a closed-set insert parameter.

    ``name`` is a lowerCamel display label (the enumerator's name, or an
    integer's decimal text for a non-contiguous whole-number parameter);
    ``value`` is the number construction accepts.
    """

    name: str
    value: int


@dataclass(frozen=True)
class MasteringInsertSlot:
    """A group of an insert's keys that exists only under a condition.

    An EQ band, a multiband crossover band, or a dynamic sub-band inside one.
    ``name`` is the key prefix without the trailing dot (``"midBand3"``,
    ``"band1.dyn2"``); ``parent`` names the enclosing slot, always listed
    earlier, or is ``None``. ``activation`` is ``"anyKey"`` when the slot
    exists once any one of its keys is supplied and ``"always"`` when it exists
    without them; ``min_crossover_cutoffs`` is how many ``cutoff<i>Hz`` keys
    the crossover in effect needs for the slot to exist (0 for none, and with
    no cutoff key supplied the cutoffs that publish a default are in effect).
    """

    name: str
    parent: str | None
    activation: Literal["anyKey", "always"]
    min_crossover_cutoffs: int


@dataclass(frozen=True)
class MasteringInsertParamDependency:
    """A sibling key whose value bounds this parameter's.

    ``relation`` reads from this parameter's side against ``factor`` times
    the sibling's value: ``"le"`` with ``factor`` 0.5 means this parameter's
    value must be at most half the sibling's.
    """

    key: str
    relation: MasteringInsertParamRelation
    factor: float


@dataclass(frozen=True)
class SynthParamCondition:
    """One condition another patch field must meet, as one of the conditions
    of an alternative in :attr:`SynthParamInfo.active_when`.

    ``scope`` is ``"engine"`` for a key of the same
    :func:`synth_engine_param_info` table and ``"patch"`` for a ``SynthPatch``
    field. ``value`` is a number, a bool against a switch, or a vocabulary name
    against a wrapper enum (``body`` ``"violin"``).
    """

    key: str
    scope: Literal["engine", "patch"]
    relation: Literal["eq", "ne", "gt", "ge", "lt", "le"]
    value: float | bool | str


@dataclass(frozen=True)
class SynthParamInfo:
    """One synth parameter as :func:`synth_engine_param_info` and
    :func:`synth_patch_param_info` describe it.

    ``name`` is the key ``SynthPatch.engine_params`` (or the patch field) is
    spelled with; ``"boolean"`` is a switch given as 0 or 1. ``integer`` is
    true for a field taking whole numbers only. ``min`` / ``max`` are ``None``
    for a field with no closed range. ``default`` is the engine's base
    preset's value for an engine field and the "keep base" default for a
    patch field, a bool for a switch. ``active_when`` is not ``None`` only for
    a field that reaches the voice under conditions on other patch fields: a
    tuple of alternatives, each a tuple of conditions, and the field reaches
    the voice when every condition of at least one alternative holds, so a
    field reached through several layers lists one alternative per layer.
    What a field further needs from a performance (velocity, a controller,
    overlapping notes), or a value past which it saturates, is not stated::

        live = field.active_when is None or any(
            all(holds(c) for c in alternative) for alternative in field.active_when
        )
    """

    name: str
    type: Literal["number", "boolean"]
    default: float | bool
    unit: MasteringInsertParamUnit
    integer: bool = False
    min: float | None = None
    max: float | None = None
    active_when: tuple[tuple[SynthParamCondition, ...], ...] | None = None


@dataclass(frozen=True)
class MasteringInsertParamInfo:
    """Metadata for one key an insert processor's construction or automation reads.

    Entries come in two runs: first the processor's realtime automation
    targets in id order, then -- sorted by name -- the keys construction reads
    that are not automation targets, with ``id`` ``None`` and ``rt_safe``
    false.

    ``min_exclusive`` / ``max_exclusive`` say whether the published bound
    itself is rejected (false when the bound is ``None``). ``max_relative_to``
    is ``"nyquist"`` when the ceiling follows the processing rate: the
    effective ceiling is then the lower of ``max`` and the host's Nyquist
    frequency, both exclusive.

    ``unit`` is declared for every numeric parameter (``"none"`` for a
    fraction, a selector index or a seed) and ``None`` otherwise. ``scale`` is
    the axis a control draws the value on. ``ui_min`` / ``ui_max``, when not
    ``None``, are a display range inside ``[min, max]``; ``None`` means the
    accepted range is also the display range. ``depends_on`` lists the
    siblings whose values bound this one; a bound that only restates that
    dependency at the sibling's default is ``None``, so ``min`` / ``max`` are
    limits of the key's own. ``coupled_with`` names the other siblings that
    move this key's accepted range in a way ``depends_on`` cannot state;
    ``min`` / ``max`` are measured with them at their defaults.
    """

    name: str
    id: int | None
    rt_safe: bool
    type: Literal["boolean", "number", "enum", "string", "array"]
    min: float | None
    max: float | None
    min_exclusive: bool
    max_exclusive: bool
    max_relative_to: Literal["nyquist"] | None
    default: float | bool | None
    unit: MasteringInsertParamUnit | None
    ui_min: float | None
    ui_max: float | None
    scale: MasteringInsertParamScale
    depends_on: tuple[MasteringInsertParamDependency, ...]
    coupled_with: tuple[str, ...]
    choices: tuple[MasteringInsertParamChoice, ...] | None
    slot: str | None


MasteringProcessorCategory = Literal[
    "dynamics",
    "effects",
    "eq",
    "final",
    "maximizer",
    "multiband",
    "other",
    "reference",
    "repair",
    "saturation",
    "spectral",
    "stereo",
    "utility",
    "voice",
]


@dataclass(frozen=True)
class MasteringInsertTiming:
    """Latency and tail of one insert built from given params, at a given rate.

    Returned by :func:`mastering_insert_timing`.
    """

    latency_samples: int
    """Processing latency in samples."""
    tail_samples: int
    """Audible decay after the input stops, in samples."""


@dataclass(frozen=True)
class MasteringProcessorCatalogEntry:
    """Capabilities exposed by :func:`mastering_processor_catalog`."""

    id: str
    kind: MasteringProcessorKind
    realtime_insertable: bool
    stereo_only: bool
    latency_samples: int
    tail_samples: int
    realtime_cost: Literal["low", "moderate", "high"] | None
    channel_policy: MasteringChannelPolicy
    category: MasteringProcessorCategory
    causal: bool
    params: tuple[MasteringInsertParamInfo, ...]
    slots: tuple[MasteringInsertSlot, ...]


@dataclass(frozen=True)
class MasteringAmpPresetCatalogEntry:
    """One built-in amp-sim rig, as returned by :func:`mastering_amp_preset_catalog`.

    ``params`` holds the rig's resolved starting values under the insert's
    camelCase parameter keys (``drive``, ``bassDb``, ``ampModel``, ...), with
    enum-valued keys as integer ordinals. Read-only metadata for an editor
    positioning inherited controls -- the DSP is still driven by the rig
    index plus sparse overrides.
    """

    index: int
    name: str
    params: dict[str, float | int | bool]


@dataclass(frozen=True)
class CapabilityCatalogPresets:
    """Built-in preset identifiers grouped by public feature family."""

    mastering: tuple[str, ...]
    """Mastering chain presets."""
    synth: tuple[str, ...]
    """NativeSynth presets."""
    mixing_scene: tuple[str, ...]
    """Mixing scene presets."""
    voice_changer: tuple[str, ...]
    """Voice changer presets."""
    playback_room: tuple[str, ...]
    """Playback room presets."""


@dataclass(frozen=True)
class MasteringPresetCatalogEntry:
    """One mastering preset's kind and loudness targets.

    ``target_lufs``, ``true_peak_ceiling_db`` and
    ``max_limiter_gain_reduction_db`` are ``None`` for a ``"restoration"``
    preset, which carries no loudness stage.
    """

    name: str
    kind: MasteringPresetKind
    target_lufs: float | None
    true_peak_ceiling_db: float | None
    max_limiter_gain_reduction_db: float | None


@dataclass(frozen=True)
class CapabilityCatalogChainStage:
    """One flat chain stage; ``<prefix>.<leaf>`` keys are ``processor``'s parameters.

    ``processor`` is ``None`` when every key of the stage is chain-only.
    """

    prefix: str
    """Key prefix of the stage."""
    processor: str | None
    """Processor id describing the stage's keys."""
    enabled_key: str
    """Chain key that switches the stage on."""


@dataclass(frozen=True)
class CapabilityCatalogChainList:
    """An indexed chain list.

    ``<key>.<i>`` is the chain parameter ``key`` when ``processor`` is ``None``;
    otherwise ``<key>.<i>.<field>`` is that processor's ``field``.
    """

    key: str
    """Chain key of the list."""
    processor: str | None
    """Processor id of each item, or ``None`` for a list of numbers."""
    min_items: int
    """Fewest items the list accepts."""
    max_items: int
    """Most items the list accepts."""


@dataclass(frozen=True)
class CapabilityCatalogChainAlias:
    """The other spellings of a canonical chain key; ``<i>`` stands for a list index."""

    key: str
    """Canonical chain key."""
    aliases: tuple[str, ...]
    """Other accepted spellings."""


@dataclass(frozen=True)
class CapabilityCatalogChain:
    """How each flat mastering chain key is described.

    ``params`` holds the keys no processor describes, with bounds measured
    through the chain's own validator.
    """

    stages: tuple[CapabilityCatalogChainStage, ...]
    """Flat chain stages."""
    lists: tuple[CapabilityCatalogChainList, ...]
    """Indexed chain lists."""
    params: tuple[MasteringInsertParamInfo, ...]
    """Chain-only keys."""
    aliases: tuple[CapabilityCatalogChainAlias, ...]
    """Alternative spellings of chain keys."""


@dataclass(frozen=True)
class CapabilityCatalog:
    """Machine-readable catalog returned by :func:`capability_catalog`."""

    version: str
    """Library version string."""
    abi: CapabilitiesAbi
    """ABI versions."""
    processors: tuple[MasteringProcessorCatalogEntry, ...]
    """Every mastering processor."""
    chain: CapabilityCatalogChain
    """Flat mastering chain keys."""
    presets: CapabilityCatalogPresets
    """Built-in preset identifiers."""
    mastering_presets: tuple[MasteringPresetCatalogEntry, ...]
    """Mastering presets with their loudness targets."""


_T = TypeVar("_T")


def _wire_key(name: str) -> str:
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


@cache
def _hints(cls: Hashable) -> dict[str, Any]:
    # The public module rebinds __module__, so resolve names against this file.
    return get_type_hints(cls, globalns=globals())


def _convert(hint: Any, value: Any) -> Any:
    origin = get_origin(hint)
    if origin in (Union, types.UnionType):
        if value is None:
            return None
        for arm in get_args(hint):
            if isinstance(arm, type) and is_dataclass(arm) and isinstance(value, dict):
                return _from_wire(arm, value)
            if get_origin(arm) is tuple and isinstance(value, list):
                return _convert(arm, value)
        return value
    if origin is tuple:
        item = get_args(hint)[0]
        return tuple(_convert(item, element) for element in value)
    if isinstance(hint, type) and is_dataclass(hint):
        return _from_wire(hint, value)
    return value


def _from_wire(cls: type[_T], document: dict[str, Any]) -> _T:
    """Build ``cls`` from a library JSON object keyed in camelCase."""
    hints = _hints(cast(Hashable, cls))
    values = {
        field.name: _convert(hints[field.name], document[_wire_key(field.name)])
        for field in fields(cls)  # type: ignore[arg-type]
        if _wire_key(field.name) in document
    }
    return cls(**values)
