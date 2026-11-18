"""Shapes of the JSON documents the explainable-mastering helpers return.

Each helper returns a JSON string; parse it with :func:`json.loads` and treat the
value as the matching ``TypedDict`` here. Keys keep the JSON's camelCase spelling.
A measurement that is not finite (a silent take has no integrated loudness) is
``None`` where the field says so.
"""

from __future__ import annotations

from typing import Any, NotRequired, TypedDict


class MasteringAudioProfileLoudness(TypedDict):
    """Loudness block of :class:`MasteringAudioProfile`."""

    integratedLufs: float
    loudnessRangeLu: float
    truePeakDbtp: float
    crestFactorDb: float


class MasteringAudioProfileSpectral(TypedDict):
    """Spectral block of :class:`MasteringAudioProfile`.

    The ``*RmsDb`` band levels are dBFS on the mean-square convention: a
    full-scale sine reads -3.01 dBFS in its band.
    """

    subRmsDb: float
    lowRmsDb: float
    lowMidRmsDb: float
    midRmsDb: float
    highMidRmsDb: float
    highRmsDb: float
    airRmsDb: float
    centroidHz: float
    flatness: float
    rolloffHz: float


class MasteringAudioProfileDynamics(TypedDict):
    """Dynamics block of :class:`MasteringAudioProfile`."""

    shortTermLufsStd: float
    attackPerSecond: float
    sustainRatio: float


class MasteringAudioProfileClick(TypedDict):
    """Click detector reading in a :class:`MasteringAudioProfileDefects` record."""

    count: int
    rejected: int
    longestRunSamples: int
    perSecond: float


class MasteringAudioProfileClip(TypedDict):
    """Clip detector reading in a :class:`MasteringAudioProfileDefects` record."""

    clippedSamples: int
    clippedRatio: float
    runCount: int
    longestRunSamples: int
    flatRunCount: int
    longestFlatRunSamples: int
    flatSampleCount: int
    flatLevel: float


class MasteringAudioProfileCrackle(TypedDict):
    """Crackle detector reading in a :class:`MasteringAudioProfileDefects` record."""

    sampleCount: int
    sampleFraction: float
    perSecond: float


class MasteringAudioProfileNoise(TypedDict):
    """Noise detector reading in a :class:`MasteringAudioProfileDefects` record."""

    floorDbfs: float
    bandFloorDbfs: list[float]


class MasteringAudioProfileHum(TypedDict):
    """Hum detector reading in a :class:`MasteringAudioProfileDefects` record.

    ``fundamentalProminence`` is a linear energy ratio over the median hum
    candidate (not dB); 1.0 means no peak.
    """

    fundamentalHz: float
    fundamentalProminence: float
    harmonics: int
    harmonicDbfs: list[float]


class MasteringAudioProfileReverb(TypedDict):
    """Reverb detector reading; ``latePredictability`` is present only when WPE ran."""

    lateDecayRatioDb: float
    latePredictability: NotRequired[float]


class MasteringAudioProfileDefects(TypedDict):
    """What the six repair detectors measured, one record per detector."""

    click: MasteringAudioProfileClick
    clip: MasteringAudioProfileClip
    crackle: MasteringAudioProfileCrackle
    noise: MasteringAudioProfileNoise
    hum: MasteringAudioProfileHum
    reverb: MasteringAudioProfileReverb


class MasteringAudioProfile(TypedDict):
    """The document :func:`mastering_audio_profile` returns, parsed.

    ``defects`` is present only when the detectors ran: ``detectDefects`` was
    asked for and the input was long enough to measure.
    """

    durationSec: float
    bpm: float
    bpmConfidence: float
    loudness: MasteringAudioProfileLoudness
    spectral: MasteringAudioProfileSpectral
    dynamics: MasteringAudioProfileDynamics
    defects: NotRequired[MasteringAudioProfileDefects]


MasteringChainConfig = dict[str, Any]
"""A nested mastering chain config: module -> processor -> parameter.

``{"dynamics": {"compressor": {"thresholdDb": -24}}, "loudness": {"targetLufs":
-14}}``. A boolean toggles a module/processor's ``enabled`` flag, an enum leaf
is its token, and a list (``crossover.cutoffsHz``, ``bands``) is a list. The
form :func:`libsonare.master_audio` takes as ``overrides`` and
:func:`libsonare.mastering_assistant_suggest_chain` returns.
"""


class MasteringChainConfigDocument(TypedDict):
    """A chain document: a version and flat ``"module.param": value`` params.

    ``params`` keys depend on the suggested stages, so they are not enumerated; a
    version 2 document carries the multiband stage as an object under
    ``dynamics.multibandComp``.
    """

    version: int
    params: dict[str, Any]


class MasteringAssistantProfile(TypedDict):
    """Measurements the assistant summarises its suggestion from (flat)."""

    durationSec: float
    bpm: float
    bpmConfidence: float
    integratedLufs: float | None
    loudnessRangeLu: float
    truePeakDbtp: float
    crestFactorDb: float
    spectralCentroidHz: float
    spectralFlatness: float
    attackPerSecond: float
    sustainRatio: float


class MasteringAssistantResult(TypedDict):
    """The document :func:`mastering_assistant_suggest` returns, parsed."""

    chainConfig: MasteringChainConfigDocument
    explanation: list[str]
    profile: MasteringAssistantProfile


class StreamingPlatform(TypedDict):
    """One delivery target a streaming preview or the assistant measures against."""

    name: str
    """Platform name, echoed into the result."""
    target_lufs: float
    """Integrated-loudness target in LUFS, e.g. -14."""
    ceiling_dbtp: float
    """True-peak ceiling in dBTP, e.g. -1."""


class MasteringStreamingPreviewPlatform(TypedDict):
    """One delivery target in a :class:`MasteringStreamingPreviewResult`."""

    name: str
    integratedLufs: float | None
    truePeakDbtp: float
    normalizationGainDb: float
    ceilingRisk: bool


class MasteringStreamingPreviewResult(TypedDict):
    """The document :func:`mastering_streaming_preview` returns, parsed."""

    platforms: list[MasteringStreamingPreviewPlatform]


class MatchReferenceLoudnessResult(TypedDict):
    """Result of the ``match.referenceLoudness`` pair analysis."""

    sourceLufs: float | None
    referenceLufs: float | None
    gainToMatchDb: float | None


class MatchTonalBalanceBand(TypedDict):
    """One band of the ``match.tonalBalance`` and ``match.tonalBalanceLogBands`` analyses."""

    lowHz: float
    highHz: float
    sourceDb: float
    referenceDb: float
    deviationDb: float


class MatchTonalBalanceResult(TypedDict):
    """Result of the ``match.tonalBalance`` pair analysis."""

    bands: list[MatchTonalBalanceBand]


class MatchEqCurveResult(TypedDict):
    """Result of the ``match.matchEqCurve`` pair analysis: ``gainDb[i]`` at ``frequencies[i]``."""

    frequencies: list[float]
    gainDb: list[float]


class MatchEstimateReferenceDelaySamplesResult(TypedDict):
    """Result of the ``match.estimateReferenceDelaySamples`` pair analysis."""

    delaySamples: float


class StereoMonoCompatCheckResult(TypedDict):
    """Result of the ``stereo.monoCompatCheck`` analysis; ``width`` is ``None`` out of phase."""

    correlation: float
    width: float | None
    monoPeak: float
    sideRms: float
    likelyMonoCompatible: bool


class StereoMonoCompatBand(TypedDict):
    """One band of the ``stereo.monoCompatCheckLogBands`` analysis."""

    lowHz: float
    highHz: float
    correlation: float
    sideRms: float


class StereoMonoCompatCheckLogBandsResult(TypedDict):
    """Result of the ``stereo.monoCompatCheckLogBands`` analysis."""

    bands: list[StereoMonoCompatBand]
