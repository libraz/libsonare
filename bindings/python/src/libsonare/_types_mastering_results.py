"""Shapes of the JSON documents the explainable-mastering helpers return.

Each helper returns a JSON string; parse it with :func:`json.loads` and treat the
value as the matching ``TypedDict`` here. Keys keep the JSON's camelCase spelling.
A measurement that is not finite (a silent take has no integrated loudness) is
``None`` where the field says so.
"""

from __future__ import annotations

from typing import Any, TypedDict


class MasteringAudioProfileLoudness(TypedDict):
    """Loudness block of :class:`MasteringAudioProfile`."""

    integratedLufs: float
    lraLu: float
    truePeakDb: float
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
    attackDensity: float
    sustainRatio: float


class MasteringAudioProfileDefects(TypedDict):
    """What the repair detectors measured.

    ``measured`` is false when nothing ran, either because ``detectDefects`` was
    not asked for or the input was too short; every other field is then at its
    default rather than a reading.
    """

    measured: bool
    clickCount: int
    clickRejected: int
    clickLongestRunSamples: int
    clickPerSecond: float
    crackleSampleCount: int
    crackleSampleFraction: float
    cracklePerSecond: float
    clipSampleCount: int
    clipRunCount: int
    clipLongestRunSamples: int
    clipSampleFraction: float
    clipFlatRunCount: int
    clipFlatSampleCount: int
    clipLongestFlatRunSamples: int
    clipFlatLevel: float
    noiseFloorDbfs: float
    noiseBandPeakDbfs: float
    noiseBandPeakIndex: int
    humFundamentalHz: float
    humFundamentalProminence: float
    humHarmonics: int
    humFundamentalDbfs: float
    humPeakHarmonicDbfs: float
    lateDecayRatioDb: float


class MasteringAudioProfile(TypedDict):
    """The document :func:`mastering_audio_profile` returns, parsed."""

    durationSec: float
    bpm: float
    bpmConfidence: float
    loudness: MasteringAudioProfileLoudness
    spectral: MasteringAudioProfileSpectral
    dynamics: MasteringAudioProfileDynamics
    defects: MasteringAudioProfileDefects


class MasteringRepairAnalysis(TypedDict):
    """Result of :func:`mastering_repair_analyze`.

    ``recommended`` lists the stages the measurement supports, in application
    order, each a ``{"stage": ..., <setting>: ...}`` dict carrying every setting,
    ready for :func:`mastering_repair_apply`. Dereverb is never recommended.
    """

    defects: MasteringAudioProfileDefects
    channels: list[MasteringAudioProfileDefects]
    declipThresholdSafe: bool
    integratedLufs: float
    recommended: list[dict[str, Any]]
    explanation: list[str]


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
    lraLu: float
    truePeakDb: float
    crestFactorDb: float
    spectralCentroidHz: float
    spectralFlatness: float
    attackDensity: float
    sustainRatio: float


class MasteringAssistantResult(TypedDict):
    """The document :func:`mastering_assistant_suggest` returns, parsed."""

    chainConfig: MasteringChainConfigDocument
    explanation: list[str]
    profile: MasteringAssistantProfile


class MasteringStreamingPreviewPlatform(TypedDict):
    """One delivery target in a :class:`MasteringStreamingPreviewResult`."""

    name: str
    integratedLufs: float | None
    truePeakDb: float
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


class MatchTonalBalanceLogBandsResult(TypedDict):
    """Result of the ``match.tonalBalanceLogBands`` pair analysis."""

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
