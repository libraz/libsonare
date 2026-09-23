"""Capability and catalogue shapes the library reports about itself.

These describe what the loaded build can do -- ABI versions, compiled-in
features, decoders and the mastering processor catalogue -- rather than the
result of analysing any audio.
"""

from __future__ import annotations

from typing import Literal, TypedDict

MasteringProcessorKind = Literal["realtime", "offline", "pair"]
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
MasteringPresetKind = Literal["mastering", "restoration"]


class CapabilitiesAbi(TypedDict):
    """ABI versions reported by :func:`libsonare.capabilities`."""

    project: int
    engine: int


class CapabilitiesFeatures(TypedDict):
    """Feature-family switches reported by :func:`libsonare.capabilities`."""

    mastering: bool
    mixing: bool
    # Separate from ``mixing``: the assistant can be dropped on its own, and its
    # entry points stay exported either way -- they answer NOT_SUPPORTED rather
    # than disappearing, so probing the library for a symbol tells a host
    # nothing. Key stays camelCase for the same reason as the one below.
    mixingAssistant: bool
    fx: bool
    ffmpeg: bool
    # Key stays camelCase: capabilities() returns the C ABI JSON verbatim.
    instrumentParamAutomation: bool
    # The four below name the remaining build options that change which
    # commands and entry points a binary answers. Without them a caller can
    # observe that a capability is missing but not that it was never built.
    arrangement: bool
    acousticSim: bool
    pitchEditor: bool
    voiceChanger: bool


class CapabilitiesDecode(TypedDict):
    """Built-in and FFmpeg-backed decoder lists for the loaded library."""

    builtin: list[str]
    ffmpeg: list[str]


class Capabilities(TypedDict):
    """Build and runtime descriptor returned by :func:`libsonare.capabilities`."""

    version: str
    abi: CapabilitiesAbi
    platform: str
    features: CapabilitiesFeatures
    decode: CapabilitiesDecode
    simd: str
    hardwareConcurrency: int


class MasteringInsertParamChoice(TypedDict):
    """One accepted value of a closed-set insert parameter.

    ``name`` is a lowerCamel display label (the enumerator's name, or an
    integer's decimal text for a non-contiguous whole-number parameter);
    ``value`` is the number construction accepts.
    """

    name: str
    value: int


class MasteringInsertParamInfo(TypedDict):
    """Metadata for one key an insert processor's construction or automation reads.

    Entries come in two runs: first the processor's realtime automation
    targets in id order, then -- sorted by name -- the keys construction reads
    that are not automation targets, with ``id`` null and ``rtSafe`` false.
    """

    name: str
    id: int | None
    rtSafe: bool
    type: Literal["boolean", "number", "enum", "string", "array"]
    min: float | None
    max: float | None
    default: float | bool | None
    unit: str | None
    choices: list[MasteringInsertParamChoice] | None


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
]


class MasteringInsertTiming(TypedDict):
    """Latency and tail of one insert built from given params, at a given rate.

    Returned by :func:`mastering_insert_timing`.
    """

    latencySamples: int
    tailSamples: int


class MasteringProcessorCatalogEntry(TypedDict):
    """Capabilities exposed by :func:`mastering_processor_catalog`."""

    id: str
    kind: MasteringProcessorKind
    realtimeInsertable: bool
    stereoOnly: bool
    latencySamples: int
    tailSamples: int
    realtimeCost: Literal["low", "moderate", "high"] | None
    channelPolicy: MasteringChannelPolicy
    category: MasteringProcessorCategory
    params: list[MasteringInsertParamInfo]


class CapabilityCatalogPresets(TypedDict):
    """Built-in preset identifiers grouped by public feature family."""

    mastering: list[str]
    synth: list[str]
    mixingScene: list[str]
    voiceChanger: list[str]


class MasteringPresetCatalogEntry(TypedDict):
    """One mastering preset's kind and loudness targets.

    ``targetLufs``, ``truePeakCeilingDb`` and ``maxLimiterGainReductionDb`` are
    ``None`` for a ``"restoration"`` preset, which carries no loudness stage.
    """

    name: str
    kind: MasteringPresetKind
    targetLufs: float | None
    truePeakCeilingDb: float | None
    maxLimiterGainReductionDb: float | None


class CapabilityCatalog(TypedDict):
    """Machine-readable catalog returned by :func:`capability_catalog`."""

    version: str
    abi: CapabilitiesAbi
    processors: list[MasteringProcessorCatalogEntry]
    presets: CapabilityCatalogPresets
    masteringPresets: list[MasteringPresetCatalogEntry]
