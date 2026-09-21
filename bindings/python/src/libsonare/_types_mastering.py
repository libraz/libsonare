"""Mastering result shapes: stage gain reduction, loudness summaries, chain output.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True, slots=True)
class MasteringResult:
    """Mastering loudness/true-peak processing result."""

    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int = 0
    loudness_target_limited: bool = False
    #: Samples the named processor replaced with a finite in-domain one,
    #: keeping the output finite and in range.
    #:
    #: A non-finite sample supplied by the caller is rejected before the
    #: processor runs, so a replacement is always of a value the processor
    #: itself produced.
    #:
    #: Whether this can be non-zero depends on which processor was named:
    #: one that does not substitute reports zero because it has nothing to
    #: replace with, not because nothing needed replacing.
    non_finite_substitution_count: int = 0


@dataclass(frozen=True, slots=True)
class MasteringStereoResult:
    """Stereo mastering processing result."""

    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int = 0
    loudness_target_limited: bool = False
    #: See :class:`MasteringResult`. Aggregated over both channels.
    non_finite_substitution_count: int = 0


@dataclass(frozen=True, slots=True)
class LoudnessMatch:
    """What gain-matching one take to another's loudness took, and produced.

    ``applied_gain_db`` is ``reference_lufs - source_lufs`` and carries no
    upper bound, so ``matched_true_peak_dbtp`` reports where the gain left the
    peak instead of the match being capped to keep it. A silent or below-gate
    take reads non-finite on both loudness values, and ``applied_gain_db`` is
    then 0.
    """

    reference_lufs: float
    source_lufs: float
    applied_gain_db: float
    matched_true_peak_dbtp: float


@dataclass(frozen=True, slots=True)
class StageGainReduction:
    """Gain reduction reported by a single dynamics/maximizer chain stage.

    ``gain_reduction_db`` is the most recent (typically last-block) gain
    reduction in dB (negative or zero); for multiband stages it is the
    most-reduced band.
    """

    stage: str
    gain_reduction_db: float


@dataclass(frozen=True, slots=True)
class MasteringLoudnessSummary:
    """Existing EBU R128 measurements captured before or after mastering."""

    integrated_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    true_peak_dbtp: float
    loudness_range: float


@dataclass(frozen=True, slots=True)
class MasteringReport:
    """Compact explanation of how an offline mastering chain changed a program."""

    before: MasteringLoudnessSummary
    after: MasteringLoudnessSummary
    applied_gain_db: float
    max_gain_reduction_db: float
    loudness_target_limited: bool
    band_energy_delta_db: list[float] = field(default_factory=list)


@dataclass(frozen=True, slots=True)
class MasteringChainResult:
    """Result of running a configurable mastering chain on mono audio."""

    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    #: ITU-R BS.1770-4 true peak of the output (dBTP). The oversample factor
    #: follows the peak-limiting stage the chain actually applied, so it is not
    #: fixed: the loudness stage's true-peak oversample (default 4x) when
    #: loudness is enabled, and the maximizer true-peak limiter's own factor
    #: when loudness is disabled but that stage ran. The two disagree by
    #: roughly 0.02 dB between 4x and 8x.
    output_true_peak_dbtp: float = 0.0
    #: EBU Tech 3342 Loudness Range of the output (LU).
    output_lra: float = 0.0
    #: True when peak headroom prevented the requested LUFS target.
    loudness_target_limited: bool = False
    #: Per-stage gain reductions for the dynamics/maximizer stages (a subset of
    #: :attr:`stages`).
    stage_gain_reductions: list[StageGainReduction] = field(default_factory=list)
    report: MasteringReport | None = None
    #: Samples a stage replaced with a finite in-domain one, keeping the
    #: output finite and in range.
    #:
    #: A non-finite sample supplied by the caller is rejected before any
    #: stage runs, so a replacement is always of a value a stage itself
    #: produced.
    #:
    #: Only the true-peak limiters replace anything, so with the maximizer's
    #: limiter and the loudness stage both disabled a zero here means no
    #: stage was able to replace anything rather than that nothing needed
    #: replacing.
    non_finite_substitution_count: int = 0


@dataclass(frozen=True, slots=True)
class MasteringChainStereoResult:
    """Result of running a configurable mastering chain on stereo audio."""

    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    #: See :class:`MasteringChainResult` for field semantics.
    output_true_peak_dbtp: float = 0.0
    output_lra: float = 0.0
    loudness_target_limited: bool = False
    stage_gain_reductions: list[StageGainReduction] = field(default_factory=list)
    report: MasteringReport | None = None
    #: See :class:`MasteringChainResult` for field semantics. Aggregated over
    #: both channels.
    non_finite_substitution_count: int = 0
