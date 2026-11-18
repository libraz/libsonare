"""Result records of the explainable-mastering helpers.

The audio profile, assistant suggestion and streaming preview come back as
frozen records with snake_case attributes. A measurement that is not finite (a
silent take has no integrated loudness) is ``None`` where the field says so.
The pair and mono-compatibility analyses return one record per analysis name.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, TypedDict

import numpy as np
import numpy.typing as npt

from ._types_repair import AudioDefects


@dataclass(frozen=True, slots=True)
class MasteringAudioProfileLoudness:
    """Loudness group of :class:`MasteringAudioProfile`."""

    integrated_lufs: float | None
    """Integrated loudness in LUFS; None for a silent or below-gate take."""
    loudness_range_lu: float
    """EBU Tech 3342 loudness range, in LU."""
    true_peak_dbtp: float
    """True peak, in dBTP."""
    crest_factor_db: float
    """Crest factor (peak over RMS level) in dB."""


@dataclass(frozen=True, slots=True)
class MasteringAudioProfileSpectral:
    """Spectral group of :class:`MasteringAudioProfile`.

    Band levels (``*_rms_db``) are dBFS, mean square: a full-scale sine reads
    -3.01 dBFS in its band.
    """

    sub_rms_db: float
    """Sub band level in dBFS."""
    low_rms_db: float
    """Low band level in dBFS."""
    low_mid_rms_db: float
    """Low-mid band level in dBFS."""
    mid_rms_db: float
    """Mid band level in dBFS."""
    high_mid_rms_db: float
    """High-mid band level in dBFS."""
    high_rms_db: float
    """High band level in dBFS."""
    air_rms_db: float
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
"""The ``params`` of a version 2 mastering chain document, with camelCase keys.

A key is a dotted path (``"loudness.targetLufs"``) and a nested object is shorthand for its
prefix (``{"loudness": {"targetLufs": -14}}``); giving one path both ways is refused. A
boolean toggles an ``enabled`` flag, an enum leaf is its token, and a list
(``crossover.cutoffsHz``, ``bands``) is a list. The core reads it on the way in, so a
content refusal is ``SonareError`` (``InvalidFormat``) naming the path. The form
:func:`libsonare.master_audio` takes as ``overrides`` and the preset, chain-config and
assistant calls return, as the core wrote it.
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
    integrated_lufs: float | None
    """Integrated loudness in LUFS; None for a silent or below-gate take."""
    loudness_range_lu: float
    """EBU Tech 3342 loudness range, in LU."""
    true_peak_dbtp: float
    """True peak, in dBTP."""
    crest_factor_db: float
    """Crest factor (peak over RMS level) in dB."""
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
    integrated_lufs: float | None
    """None for a silent or below-gate take."""
    true_peak_dbtp: float
    """True peak of the take, in dBTP."""
    normalization_gain_db: float
    """Gain that lands the take on the platform's target; 0 when the loudness is None."""
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

    A loudness a silent take cannot give, and the gain derived from it, is None.
    """

    source_lufs: float | None
    """Integrated loudness of the source in LUFS; None for a silent take."""
    reference_lufs: float | None
    """Integrated loudness of the reference in LUFS; None for a silent take."""
    gain_to_match_db: float | None
    """Gain that brings the source to the reference loudness."""


@dataclass(frozen=True, slots=True)
class MatchTonalBalanceBand:
    """One band of the ``match.tonalBalance`` and ``match.tonalBalanceLogBands`` analyses."""

    low_hz: float
    """Lower band edge in Hz."""
    high_hz: float
    """Upper band edge in Hz."""
    source_db: float
    """Source level in the band, in dB."""
    reference_db: float
    """Reference level in the band, in dB."""
    deviation_db: float
    """Source level minus reference level."""


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
    """float32 gain in dB at each frequency."""


@dataclass(frozen=True, slots=True)
class MatchEstimateReferenceDelaySamplesResult:
    """Result of the ``match.estimateReferenceDelaySamples`` pair analysis."""

    delay_samples: int
    """Delay of the reference relative to the source, in whole source-rate samples."""


@dataclass(frozen=True, slots=True)
class StereoMonoCompatCheckResult:
    """Result of the ``stereo.monoCompatCheck`` analysis."""

    correlation: float
    """Inter-channel correlation in ``[-1, 1]``."""
    width: float | None
    """Stereo width; None for a fully out-of-phase pair."""
    mono_peak: float
    """Peak of the mono fold-down."""
    side_rms: float
    """RMS level of the side signal."""
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
    """RMS level of the side signal."""


@dataclass(frozen=True, slots=True)
class StereoMonoCompatCheckLogBandsResult:
    """Result of the ``stereo.monoCompatCheckLogBands`` analysis."""

    bands: list[StereoMonoCompatBand]
    """Per-band readings."""
