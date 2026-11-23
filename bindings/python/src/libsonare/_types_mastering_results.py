"""Result records of the explainable-mastering helpers.

The audio profile, assistant suggestion and streaming preview come back as
frozen records with snake_case attributes. A level of silence is ``-inf`` (an
anti-phase width ``inf``), and a measurement the library did not take is ``None``
where the field says so.
The pair and mono-compatibility analyses return one record per analysis name.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, TypedDict

import numpy as np
import numpy.typing as npt

from ._types_repair import AudioDefects


@dataclass(frozen=True, slots=True)
class MasteringAudioProfileLoudness:
    """Loudness group of :class:`MasteringAudioProfile`."""

    integrated_lufs: float
    """Integrated loudness in LUFS; -inf for a silent or below-gate take."""
    loudness_range_lu: float | None
    """EBU Tech 3342 loudness range, in LU; None for a silent take."""
    true_peak_dbtp: float
    """True peak, in dBTP; -inf for digital silence."""
    crest_factor_db: float | None
    """Crest factor (peak over RMS level) in dB; None for a silent take."""


@dataclass(frozen=True, slots=True)
class MasteringAudioProfileSpectral:
    """Spectral group of :class:`MasteringAudioProfile`.

    Band levels (``*_rms_db``) are dBFS, mean square: a full-scale sine reads
    -3.01 dBFS in its band, a band with no energy reads -inf, and a band lying wholly
    above the Nyquist frequency is not measured and reads None.
    """

    sub_rms_db: float | None
    """Sub band level in dBFS."""
    low_rms_db: float | None
    """Low band level in dBFS."""
    low_mid_rms_db: float | None
    """Low-mid band level in dBFS."""
    mid_rms_db: float | None
    """Mid band level in dBFS."""
    high_mid_rms_db: float | None
    """High-mid band level in dBFS."""
    high_rms_db: float | None
    """High band level in dBFS."""
    air_rms_db: float | None
    """Air band level in dBFS."""
    centroid_hz: float
    """Spectral centroid in Hz."""
    flatness: float
    """Spectral flatness of the input."""
    rolloff_hz: float
    """Spectral rolloff frequency in Hz."""


@dataclass(frozen=True, slots=True)
class MasteringAudioProfileDynamics:
    """Dynamics group of :class:`MasteringAudioProfile`."""

    short_term_lufs_std: float
    """Standard deviation of the short-term loudness, in LU."""
    attack_per_second: float
    """Onsets per second of the input."""
    sustain_ratio: float
    """Sustain ratio of the input."""


@dataclass(frozen=True, slots=True)
class MasteringAudioProfile:
    """What :func:`mastering_audio_profile` and its stereo form return.

    ``defects`` is None unless the detectors ran: ``detectDefects`` was asked
    for and the input was long enough to measure. The profile never runs the
    WPE stage, so its reverb reading carries no ``late_predictability``.
    """

    duration_sec: float
    """Input duration in seconds."""
    bpm: float
    """Estimated tempo in BPM."""
    bpm_confidence: float
    """Confidence of the tempo estimate."""
    loudness: MasteringAudioProfileLoudness
    """Loudness measurements."""
    spectral: MasteringAudioProfileSpectral
    """Spectral measurements."""
    dynamics: MasteringAudioProfileDynamics
    """Dynamics measurements."""
    defects: AudioDefects | None
    """Repair detector readings; None unless the detectors ran."""


MasteringChainConfig = dict[str, Any]
"""The ``params`` of a version 2 mastering chain document as the core wrote it, camelCase keys.

The form the preset, chain-config and assistant calls return. A boolean toggles an ``enabled``
flag, an enum leaf is its token, and a list (``crossover.cutoffsHz``, ``bands``) is a list. Every
call taking a chain config takes it as :data:`MasteringChainConfigInput`, which this form is one
spelling of.
"""

MasteringChainConfigInput = Mapping[str, Any]
"""A mastering chain config as a caller gives it: :data:`MasteringChainConfig`, flat or mixed.

A key is a dotted path (``"loudness.targetLufs"``) and a nested object is shorthand for its
prefix (``{"loudness": {"targetLufs": -14}}``); giving one path both ways is refused. The core
reads it on the way in, so a content refusal is ``SonareError`` (``InvalidFormat``) naming the
path. The form :func:`libsonare.master_audio` takes as ``overrides`` and the streaming
mastering calls take as ``config``.
"""

EqBandInput = dict[str, Any]
"""One equalizer band with camelCase keys; omitted keys take the processor defaults.

``{"type": "peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True}``.
Further keys: ``coeffMode``, ``slopeDbPerOct``, ``phase``, ``placement``. The
form :meth:`libsonare.StreamingEqualizer.set_band` takes.
"""


@dataclass(frozen=True, slots=True)
class MasteringAssistantProfile:
    """Measurements the assistant summarises its suggestion from (flat)."""

    duration_sec: float
    """Input duration in seconds."""
    bpm: float
    """Estimated tempo in BPM."""
    bpm_confidence: float
    """Confidence of the tempo estimate."""
    integrated_lufs: float
    """Integrated loudness in LUFS; -inf for a silent or below-gate take."""
    loudness_range_lu: float | None
    """EBU Tech 3342 loudness range, in LU; None for a silent take."""
    true_peak_dbtp: float
    """True peak, in dBTP; -inf for digital silence."""
    crest_factor_db: float | None
    """Crest factor (peak over RMS level) in dB; None for a silent take."""
    spectral_centroid_hz: float
    """Spectral centroid in Hz."""
    spectral_flatness: float
    """Spectral flatness of the input."""
    attack_per_second: float
    """Onsets per second of the input."""
    sustain_ratio: float
    """Sustain ratio of the input."""


@dataclass(frozen=True, slots=True)
class MasteringAssistantResult:
    """What :func:`mastering_assistant_suggest` and its stereo form return."""

    chain_config: MasteringChainConfig
    """The suggested chain; pass it as ``overrides`` to :func:`master_audio` unchanged."""
    explanation: list[str]
    """One line per decision the assistant made, in the order it made them."""
    profile: MasteringAssistantProfile
    """Measurements the suggestion was derived from."""


class StreamingPlatform(TypedDict):
    """One delivery target a streaming preview or the assistant measures against."""

    name: str
    """Platform name, echoed into the result."""
    target_lufs: float
    """Integrated-loudness target in LUFS, e.g. -14."""
    ceiling_dbtp: float
    """True-peak ceiling in dBTP, e.g. -1."""


@dataclass(frozen=True, slots=True)
class MasteringStreamingPreviewPlatform:
    """One delivery target in a :class:`MasteringStreamingPreviewResult`."""

    name: str
    """Platform name, echoed from the requested target."""
    integrated_lufs: float
    """Integrated loudness in LUFS; -inf for a silent or below-gate take."""
    true_peak_dbtp: float
    """True peak of the take, in dBTP; -inf for digital silence."""
    normalization_gain_db: float | None
    """Gain that lands the take on the platform's target; None for a silent take."""
    ceiling_risk: bool
    """True when that gain would push the true peak above the platform's ceiling."""


@dataclass(frozen=True, slots=True)
class MasteringStreamingPreviewResult:
    """What :func:`mastering_streaming_preview` and its stereo form return."""

    platforms: list[MasteringStreamingPreviewPlatform]
    """One entry per requested delivery target."""


@dataclass(frozen=True, slots=True)
class MatchReferenceLoudnessResult:
    """Result of the ``match.referenceLoudness`` pair analysis.

    A silent take's loudness is -inf, and the gain derived from it is None.
    """

    source_lufs: float
    """Integrated loudness of the source in LUFS; -inf for a silent take."""
    reference_lufs: float
    """Integrated loudness of the reference in LUFS; -inf for a silent take."""
    gain_to_match_db: float | None
    """Gain that brings the source to the reference loudness; None when either take is silent."""


@dataclass(frozen=True, slots=True)
class MatchTonalBalanceBand:
    """One band of the ``match.tonalBalance`` and ``match.tonalBalanceLogBands`` analyses."""

    low_hz: float
    """Lower band edge in Hz."""
    high_hz: float
    """Upper band edge in Hz."""
    source_db: float | None
    """Source level in the band, in dB; -inf for a band with no energy, None for a band no
    spectrum bin falls in."""
    reference_db: float | None
    """Reference level in the band, in dB; -inf for a band with no energy, None for a band no
    spectrum bin falls in."""
    deviation_db: float | None
    """Source level minus reference level; -inf when only the source band is empty, None when
    the reference band is or when no spectrum bin falls in the band."""


@dataclass(frozen=True, slots=True)
class MatchTonalBalanceResult:
    """Result of the ``match.tonalBalance`` and ``match.tonalBalanceLogBands`` analyses."""

    bands: list[MatchTonalBalanceBand]
    """Per-band readings."""


@dataclass(frozen=True, slots=True, eq=False)
class MatchEqCurveResult:
    """Result of the ``match.matchEqCurve`` pair analysis: ``gain_db[i]`` at ``frequencies[i]``."""

    frequencies: npt.NDArray[np.float32]
    """float32 frequencies in Hz."""
    gain_db: npt.NDArray[np.float32]
    """float32 gain in dB at each frequency. A band where the source has no energy gets 0 dB, one
    where only the reference has none gets ``-maxGainDb``, and the decision precedes smoothing."""


@dataclass(frozen=True, slots=True)
class MatchEstimateReferenceDelaySamplesResult:
    """Result of the ``match.estimateReferenceDelaySamples`` pair analysis."""

    delay_samples: int
    """Delay of the reference relative to the source, in whole source-rate samples."""


@dataclass(frozen=True, slots=True)
class StereoMonoCompatCheckResult:
    """Result of the ``stereo.monoCompatCheck`` analysis."""

    correlation: float | None
    """Inter-channel correlation in ``[-1, 1]``; None when both channels are silent."""
    width: float | None
    """Stereo width; inf for a fully out-of-phase pair, None when both channels are silent."""
    mono_peak: float
    """Peak of the mono fold-down 0.5 * (L + R), as a linear full-scale amplitude."""
    side_rms: float
    """RMS of the side signal 0.5 * (L - R), as a linear full-scale amplitude."""
    likely_mono_compatible: bool
    """True when the pair is likely to survive a mono fold-down."""


@dataclass(frozen=True, slots=True)
class StereoMonoCompatBand:
    """One band of the ``stereo.monoCompatCheckLogBands`` analysis."""

    low_hz: float
    """Lower band edge in Hz."""
    high_hz: float
    """Upper band edge in Hz."""
    correlation: float
    """Inter-channel correlation in ``[-1, 1]``."""
    side_rms: float
    """RMS of the band's share of side signal 0.5 * (L - R), as a linear full-scale amplitude."""


@dataclass(frozen=True, slots=True)
class StereoMonoCompatCheckLogBandsResult:
    """Result of the ``stereo.monoCompatCheckLogBands`` analysis."""

    bands: list[StereoMonoCompatBand]
    """Per-band readings."""
