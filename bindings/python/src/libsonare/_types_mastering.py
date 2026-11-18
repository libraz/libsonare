"""Mastering result shapes: stage gain reduction, loudness summaries, chain output.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from ._types_analysis import _empty_float32


@dataclass(frozen=True, slots=True, eq=False)
class MasteringResult:
    """Mastering loudness/true-peak processing result."""

    samples: np.ndarray
    """Mono output samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS."""
    applied_gain_db: float
    """Gain applied to the input, in dB."""
    latency_samples: int = 0
    """Processor latency in samples; the output is already compensated for it."""
    loudness_target_limited: bool = False
    """True when the true-peak ceiling prevented reaching the target loudness."""
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
    """Number of samples the processor replaced with a finite value."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringStereoResult:
    """Stereo mastering processing result."""

    left: np.ndarray
    """Left output channel samples."""
    right: np.ndarray
    """Right output channel samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS."""
    applied_gain_db: float
    """Gain applied to the input, in dB."""
    latency_samples: int = 0
    """Processor latency in samples; the output is already compensated for it."""
    loudness_target_limited: bool = False
    """True when the true-peak ceiling prevented reaching the target loudness."""
    #: See :class:`MasteringResult`. Aggregated over both channels.
    non_finite_substitution_count: int = 0
    """Number of samples the processor replaced with a finite value."""


@dataclass(frozen=True, slots=True)
class LoudnessMatch:
    """What gain-matching one take to another's loudness took, and produced.

    ``applied_gain_db`` is the gain that lands the remeasured source on
    ``reference_lufs``; it equals ``reference_lufs - source_lufs`` unless the
    gain moves blocks across the absolute gate. It carries no upper bound, so
    ``matched_true_peak_dbtp`` reports where the gain left the peak instead of
    the match being capped to keep it. A silent or below-gate take has no
    loudness: its ``reference_lufs`` or ``source_lufs`` is ``None``, and
    ``applied_gain_db`` is then 0.
    """

    reference_lufs: float | None
    """Integrated loudness of the reference in LUFS; None for a silent or below-gate take."""
    source_lufs: float | None
    """Integrated loudness of the source in LUFS; None for a silent or below-gate take."""
    applied_gain_db: float
    """Gain applied to the source, in dB; 0 when either loudness is None."""
    matched_true_peak_dbtp: float
    """True peak of the matched take after the gain, in dBTP."""


@dataclass(frozen=True, slots=True)
class StreamingLoudnessGain:
    """The loudness numbers a :class:`StreamingMasteringChain` is built with.

    Measured at the loudness stage's input, after every earlier enabled stage,
    which is where the offline chain measures, so ``loudness_static_gain_db``
    equals the gain :func:`mastering_chain` applies, ceiling clamp included.
    Pass ``loudness_static_gain_db`` and ``true_peak_dbtp`` as the constructor's
    ``loudness_static_gain_db`` and ``loudness_static_gain_peak_db``. A silent or
    below-gate stage input has no ``integrated_lufs`` (``None``) and a gain of 0;
    digital silence has no ``true_peak_dbtp`` (``None``), which the constructor
    reads as no measured peak.
    """

    loudness_static_gain_db: float
    """Static gain of the loudness stage in dB, ceiling clamp included."""
    true_peak_dbtp: float | None
    """True peak at the stage input in dBTP; None for digital silence."""
    integrated_lufs: float | None
    """Integrated loudness at the stage input in LUFS; None for a silent or below-gate input."""


@dataclass(frozen=True, slots=True)
class StageGainReduction:
    """Gain reduction reported by a single dynamics/maximizer chain stage.

    ``gain_reduction_db`` is the most recent (typically last-block) gain
    reduction in dB (negative or zero); for multiband stages it is the
    most-reduced band.
    """

    stage: str
    """Identifier of the chain stage."""
    gain_reduction_db: float
    """Latest gain reduction in dB (<= 0); the most-reduced band for multiband stages."""


@dataclass(frozen=True, slots=True)
class MasteringLoudnessSummary:
    """Existing EBU R128 measurements captured before or after mastering."""

    integrated_lufs: float
    """Integrated loudness in LUFS."""
    max_momentary_lufs: float
    """Maximum momentary loudness in LUFS."""
    max_short_term_lufs: float
    """Maximum short-term loudness in LUFS."""
    true_peak_dbtp: float
    """True peak, in dBTP."""
    loudness_range_lu: float
    """EBU Tech 3342 loudness range, in LU."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringReport:
    """Compact explanation of how an offline mastering chain changed a program."""

    before: MasteringLoudnessSummary
    """Measurements of the input."""
    after: MasteringLoudnessSummary
    """Measurements of the output."""
    applied_gain_db: float
    """Gain applied by the chain, in dB."""
    max_gain_reduction_db: float
    """Most-negative final dynamics/limiter gain reduction in dB; zero when none ran."""
    loudness_target_limited: bool
    """True when peak headroom prevented the requested LUFS target."""
    band_energy_delta_db: np.ndarray = field(default_factory=_empty_float32)
    """32 log-spaced after-minus-before spectral energy deltas in dB, 20 Hz to Nyquist."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringChainResult:
    """Result of running a configurable mastering chain on mono audio."""

    samples: np.ndarray
    """Mono output samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS; the achieved value, not the target."""
    applied_gain_db: float
    """Gain applied by the chain, in dB."""
    stages: list[str]
    """Identifiers of the stages the chain applied."""
    #: ITU-R BS.1770-4 true peak of the output (dBTP). The oversample factor
    #: follows the peak-limiting stage the chain actually applied, so it is not
    #: fixed: the loudness stage's true-peak oversample (default 4x) when
    #: loudness is enabled, and the maximizer true-peak limiter's own factor
    #: when loudness is disabled but that stage ran. The two disagree by
    #: roughly 0.02 dB between 4x and 8x.
    output_true_peak_dbtp: float = 0.0
    """True peak of the output, in dBTP."""
    #: EBU Tech 3342 Loudness Range of the output (LU).
    output_loudness_range_lu: float = 0.0
    """EBU Tech 3342 loudness range of the output, in LU."""
    #: True when peak headroom prevented the requested LUFS target.
    loudness_target_limited: bool = False
    """True when peak headroom prevented the requested LUFS target."""
    #: Per-stage gain reductions for the dynamics/maximizer stages (a subset of
    #: :attr:`stages`).
    stage_gain_reductions: list[StageGainReduction] = field(default_factory=list)
    """Gain reduction of each dynamics/maximizer stage."""
    report: MasteringReport | None = None
    """Before/after measurements of the run; None when the chain produced none."""
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
    """Number of samples a stage replaced with a finite value."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringChainStereoResult:
    """Result of running a configurable mastering chain on stereo audio."""

    left: np.ndarray
    """Left output channel samples."""
    right: np.ndarray
    """Right output channel samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS; the achieved value, not the target."""
    applied_gain_db: float
    """Gain applied by the chain, in dB."""
    stages: list[str]
    """Identifiers of the stages the chain applied."""
    #: See :class:`MasteringChainResult` for field semantics.
    output_true_peak_dbtp: float = 0.0
    """True peak of the output, in dBTP."""
    output_loudness_range_lu: float = 0.0
    """EBU Tech 3342 loudness range of the output, in LU."""
    loudness_target_limited: bool = False
    """True when peak headroom prevented the requested LUFS target."""
    stage_gain_reductions: list[StageGainReduction] = field(default_factory=list)
    """Gain reduction of each dynamics/maximizer stage."""
    report: MasteringReport | None = None
    """Before/after measurements of the run; None when the chain produced none."""
    #: See :class:`MasteringChainResult` for field semantics. Aggregated over
    #: both channels.
    non_finite_substitution_count: int = 0
    """Number of samples a stage replaced with a finite value."""
