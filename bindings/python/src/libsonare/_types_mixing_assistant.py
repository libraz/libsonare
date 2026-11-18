"""Result records of :func:`mixing_assistant_suggest`.

The suggestion is parameters only: ``scene`` is a mixer scene document, kept
with its own keys so it goes to :meth:`Mixer.from_scene_json` unchanged, and
the records beside it are the measurements the decisions were made from.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal, TypeAlias

MixAnalysisBand: TypeAlias = Literal["sub", "low", "lowMid", "mid", "highMid", "high", "air"]
"""Analysis band the assistant measures occupancy, dominance and crowding in."""

MixSourceClass: TypeAlias = Literal[
    "unknown",
    "kick",
    "snare",
    "hiHat",
    "tom",
    "cymbal",
    "bass",
    "guitar",
    "keys",
    "strings",
    "lead",
    "vocal",
    "backing",
    "percussion",
    "fx",
    "drumKit",
]
"""Source class a track resolves to.

:func:`mixing_assistant_source_class_names` lists them at runtime.
"""


@dataclass(frozen=True, slots=True)
class MixAssistantTrackProfile:
    """Per-track measurements and the class the track was resolved to."""

    strip_id: str
    """Id of the strip the track is addressed by."""
    name: str | None
    """Display name the track was given; None when it was given none."""
    source: MixSourceClass
    source_confidence: float
    """Classifier confidence in ``[0, 1]``."""
    usable: bool
    """False when the track could not be measured; it then gets no suggestions at all."""
    exclusion_reason: str | None
    """Why the track was excluded; None when it is usable."""
    num_channels: int
    duration_sec: float
    integrated_lufs: float | None
    """BS.1770 integrated loudness in LUFS; None for a track with no gated block to measure."""
    true_peak_dbtp: float
    crest_factor_db: float
    spectral_centroid_hz: float
    spectral_flatness: float
    """Near 0 is tonal, near 1 is noise-like."""
    attack_per_second: float
    sustain_ratio: float
    band_occupancy: dict[MixAnalysisBand, float]
    """Share of the track's energy in each analysis band."""


@dataclass(frozen=True, slots=True)
class MixBandDominance:
    """One informative masker/maskee band pair."""

    masker: str
    maskee: str
    band: MixAnalysisBand
    ratio: float
    """Masker's mean energy share in ``[0, 1]``; 0.5 means equal band energy."""
    valid_frames: int
    """Frames where both tracks exceeded the band energy floor."""


@dataclass(frozen=True, slots=True)
class MixTrackAlignment:
    """A related track pair and the time/polarity relationship measured between them."""

    reference: str
    target: str
    lag_samples: int
    """Lag that aligns the target onto the reference; positive means the target arrives later."""
    correlation: float
    """Signed normalized correlation at ``lag_samples``, in ``[-1, 1]``."""
    polarity_opposed: bool


@dataclass(frozen=True, slots=True)
class MixCrowdedBand:
    """A band carrying more simultaneous sources than the image comfortably holds."""

    band: MixAnalysisBand
    crowding: float
    """In ``[0, 1]``: 1 when all of the band's energy sits in one pan position."""


@dataclass(frozen=True, slots=True)
class MixMonoRisk:
    """A track whose stereo image is likely to collapse in mono."""

    strip_id: str
    correlation: float
    """Inter-channel correlation in ``[-1, 1]``."""
    width: float
    """Side-to-mid level ratio, clamped finite so an anti-phase pair reads a large value."""
    wide_low_end: bool


@dataclass(frozen=True, slots=True)
class MixAssistantMixProfile:
    """Cross-track measurements the decisions were made from."""

    track_count: int
    band_dominance: list[MixBandDominance]
    alignment: list[MixTrackAlignment]
    crowded_bands: list[MixCrowdedBand]
    mono_risks: list[MixMonoRisk]


@dataclass(frozen=True, slots=True)
class MixAssistantResult:
    """What :func:`mixing_assistant_suggest` returns."""

    scene: dict[str, Any]
    """The suggested scene document; nothing has been applied."""
    tracks: list[MixAssistantTrackProfile]
    """One entry per input track, in input order."""
    mix: MixAssistantMixProfile
    explanation: list[str]
    """Reasons in the order the changes were applied; empty when nothing was suggested."""
