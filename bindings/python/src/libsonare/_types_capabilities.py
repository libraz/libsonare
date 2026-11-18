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
"""How a mastering processor runs: as a realtime insert, whole-file only, or on a pair of inputs."""
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
"""How the mixer wraps a processor on a bus with more than two channels."""
MasteringPresetKind = Literal["mastering", "restoration"]
"""Which preset family a mastering preset belongs to."""
MasteringInsertParamUnit = Literal[
    "dB",
    "dBFS",
    "dBTP",
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
"""Unit of a mastering insert parameter value."""
MasteringInsertParamScale = Literal["linear", "log"]
"""Axis a control draws a mastering insert parameter on."""
MasteringInsertParamRelation = Literal["lt", "le", "gt", "ge"]
"""Ordering of a parameter against a sibling: less than, at most, greater than or at least."""


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
    """Display label in lowerCamelCase; not accepted as input."""
    value: int
    """Number construction accepts for this choice."""


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
    """Key prefix without the trailing dot, such as ``"midBand3"``."""
    parent: str | None
    """Name of the enclosing slot, which must exist for this one to; None for a top-level slot."""
    activation: Literal["anyKey", "always"]
    """``"anyKey"``: exists once any of its keys is supplied; ``"always"``: exists without."""
    min_crossover_cutoffs: int
    """Crossover ``cutoff<i>Hz`` keys needed in effect for the slot to exist; 0 for none."""


@dataclass(frozen=True)
class MasteringInsertParamDependency:
    """A sibling key whose value bounds this parameter's.

    ``relation`` reads from this parameter's side against ``factor`` times
    the sibling's value: ``"le"`` with ``factor`` 0.5 means this parameter's
    value must be at most half the sibling's.
    """

    key: str
    """Key of the sibling parameter that bounds this one."""
    relation: MasteringInsertParamRelation
    """How this value is ordered against ``factor`` times the sibling: lt, le, gt or ge."""
    factor: float
    """Positive multiplier on the sibling's value; 1 for a plain ordering."""


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
    """Name of the field the condition reads."""
    scope: Literal["engine", "patch"]
    """``"engine"`` for a key of the same engine table, ``"patch"`` for a ``SynthPatch`` field."""
    relation: Literal["eq", "ne", "gt", "ge", "lt", "le"]
    """Comparison of the field against ``value``: eq, ne, gt, ge, lt or le."""
    value: float | bool | str
    """Value compared against: a number, a bool for a switch, or a vocabulary name."""


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
    """Key ``SynthPatch.engine_params`` takes, or the ``SynthPatch`` field name."""
    type: Literal["number", "boolean"]
    """``"boolean"`` is a switch given as 0 or 1; ``"number"`` is any other value."""
    default: float | bool
    """Engine base preset's value for an engine field, the keep-base default for a patch field."""
    unit: MasteringInsertParamUnit
    """Unit of the value, a ``MasteringInsertParamUnit`` token."""
    integer: bool = False
    """True for a field taking whole numbers only."""
    min: float | None = None
    """Smallest accepted value; None for a field with no closed range."""
    max: float | None = None
    """Largest accepted value; None for a field with no closed range."""
    active_when: tuple[tuple[SynthParamCondition, ...], ...] | None = None
    """Alternatives of conditions under which the field reaches the voice; None if unconditional."""


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
    """JSON-key parameter name, as used in scene insert params."""
    id: int | None
    """Integer id for realtime automation lanes and MIDI-CC binding; None for a construction-only
    key."""
    rt_safe: bool
    """Whether it can be changed live from the audio thread; false when ``id`` is None."""
    type: Literal["boolean", "number", "enum", "string", "array"]
    """Type construction reads the key as; ``"enum"`` is sent as the number in ``choices``."""
    min: float | None
    """Smallest value construction accepts; None when no limit is stated or ``choices`` is set."""
    max: float | None
    """Largest value construction accepts; None when no limit is stated or ``choices`` is set."""
    min_exclusive: bool
    """Whether ``min`` itself is rejected; false when ``min`` is None."""
    max_exclusive: bool
    """Whether ``max`` itself is rejected; false when ``max`` is None."""
    max_relative_to: Literal["nyquist"] | None
    """``"nyquist"`` when the ceiling follows the processing rate; None otherwise."""
    default: float | bool | None
    """Value the processor uses when the key is absent; None when there is no fallback."""
    unit: MasteringInsertParamUnit | None
    """Declared unit of a numeric key, a ``MasteringInsertParamUnit`` token; None otherwise."""
    ui_min: float | None
    """Lowest value a control should draw, inside [``min``, ``max``]; None means ``min``."""
    ui_max: float | None
    """Highest value a control should draw, inside [``min``, ``max``]; None means ``max``."""
    scale: MasteringInsertParamScale
    """Axis a control draws the value on, ``"linear"`` or ``"log"``."""
    depends_on: tuple[MasteringInsertParamDependency, ...]
    """Siblings whose live values bound this key; empty for an independent key."""
    coupled_with: tuple[str, ...]
    """Other siblings that move this key's accepted range in a way ``depends_on`` cannot state."""
    choices: tuple[MasteringInsertParamChoice, ...] | None
    """Closed set of accepted values in value order; None when the accepted values are not a closed
    set."""
    slot: str | None
    """Name of the :class:`MasteringInsertSlot` this key belongs to; None for a key that always
    exists."""


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
"""Grouping of a mastering processor for a picker."""


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
    """Processor id, the name used for scene inserts and named processors."""
    kind: MasteringProcessorKind
    """``"realtime"`` builds as a scene insert, ``"offline"`` is whole-file only, ``"pair"`` takes
    two inputs."""
    realtime_insertable: bool
    """True exactly for ids that always succeed as a realtime scene insert."""
    stereo_only: bool
    """True for a processor with no mono implementation."""
    latency_samples: int
    """Latency in samples for the default 48 kHz / 512-sample probe; 0 for offline processors."""
    tail_samples: int
    """Audible decay length in samples for the same probe; 0 for offline or no-tail processors."""
    realtime_cost: Literal["low", "moderate", "high"] | None
    """Coarse realtime work estimate; None when the processor is not an insert."""
    channel_policy: MasteringChannelPolicy
    """How the mixer wraps the processor on a bus with more than two channels."""
    category: MasteringProcessorCategory
    """Grouping for a processor picker, derived from the id's prefix."""
    causal: bool
    """Whether the output at a sample depends only on input up to it, plus the reported latency."""
    params: tuple[MasteringInsertParamInfo, ...]
    """Construction parameters of the processor; empty for an offline entry with none."""
    slots: tuple[MasteringInsertSlot, ...]
    """Conditional key groups of an insert in declaration order; empty if not an insert."""


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
    """Stable rig index used by the amp-sim ``presetIndex`` parameter."""
    name: str
    """Canonical preset identifier accepted by the amp-sim insert."""
    params: dict[str, float | int | bool]
    """Resolved starting values under the insert's camelCase keys; enum keys as integer ordinals."""


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

    ``target_lufs``, ``true_peak_ceiling_dbtp`` and
    ``max_limiter_gain_reduction_db`` are ``None`` for a ``"restoration"``
    preset, which carries no loudness stage.
    """

    name: str
    """Preset name."""
    kind: MasteringPresetKind
    """Preset family, ``"mastering"`` or ``"restoration"``."""
    target_lufs: float | None
    """Integrated loudness target in LUFS; None for a restoration preset."""
    true_peak_ceiling_dbtp: float | None
    """True-peak ceiling in dBTP; None for a restoration preset."""
    max_limiter_gain_reduction_db: float | None
    """Largest gain reduction the limiter may apply, in dB; None for a restoration preset."""


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
