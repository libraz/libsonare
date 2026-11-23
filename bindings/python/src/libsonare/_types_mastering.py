"""Mastering result shapes: stage gain reduction, loudness summaries, chain output.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal, TypeAlias

import numpy as np
import numpy.typing as npt

from ._types_analysis import _empty_float32

MasteringParamValue: TypeAlias = float | int | bool
"""A single mastering parameter value."""
MasteringParams: TypeAlias = dict[str, MasteringParamValue]
"""Mastering parameters keyed by their catalog name."""
MasteringSoloParams: TypeAlias = dict[str, MasteringParamValue | str]
"""A solo processor's params; an enum-valued key also takes its choices name."""
MasteringAssistantParams: TypeAlias = dict[str, MasteringParamValue | str]
"""Assistant params; ``targetPlatform`` and ``preset`` additionally take names."""

MasteringPreset: TypeAlias = Literal[
    "pop",
    "edm",
    "acoustic",
    "hipHop",
    "aiMusic",
    "speech",
    "streaming",
    "youtube",
    "broadcast",
    "podcast",
    "audiobook",
    "cinema",
    "jpop",
    "ambient",
    "lofi",
    "classical",
    "drumAndBass",
    "techno",
    "metal",
    "trap",
    "rnb",
    "jazz",
    "kpop",
    "trance",
    "gameOst",
    "vinyl",
    "tapeHiss",
    "fieldRecording",
    "voiceMemo",
    "shellac78",
]
"""A mastering preset name, as :func:`mastering_preset_names` lists them."""

# BEGIN GENERATED SoloProcessor (make processor-types)
# Every processor name mastering_processor_names() can return, and therefore
# every name mastering_apply_processor() / mastering_apply_processor_stereo() accept.
SoloProcessor: TypeAlias = Literal[
    "dynamics.brickwallLimiter",
    "dynamics.compressor",
    "dynamics.deesser",
    "dynamics.duckingProcessor",
    "dynamics.expander",
    "dynamics.gate",
    "dynamics.limiter",
    "dynamics.parallelComp",
    "dynamics.sidechainRouter",
    "dynamics.transientShaper",
    "dynamics.upwardCompressor",
    "dynamics.upwardExpander",
    "dynamics.vocalRider",
    "effects.acoustic.roomMorph",
    "effects.delay.stereo",
    "effects.filter.vowel",
    "effects.gsEfx",
    "effects.modulation.autoWah",
    "effects.modulation.chorus",
    "effects.modulation.ensemble",
    "effects.modulation.flanger",
    "effects.modulation.phaser",
    "effects.modulation.pitchShifter",
    "effects.modulation.ringModulator",
    "effects.modulation.rotary",
    "effects.modulation.wah",
    "effects.reverb.convolution",
    "effects.reverb.fdn",
    "effects.reverb.plate",
    "effects.reverb.room",
    "effects.reverb.velvet",
    "eq.bandPass",
    "eq.cutFilter",
    "eq.dynamic",
    "eq.equalizer",
    "eq.graphic",
    "eq.linearPhase",
    "eq.midSide",
    "eq.minimumPhase",
    "eq.parametric",
    "eq.passiveTube",
    "eq.shelving",
    "eq.steppedProportionalQ",
    "eq.tilt",
    "final.bitDepth",
    "final.dither",
    "final.outputChain",
    "maximizer.adaptiveRelease",
    "maximizer.loudnessOptimize",
    "maximizer.maximizer",
    "maximizer.softKneeMax",
    "maximizer.truePeakLimiter",
    "multiband.compressor",
    "multiband.dynamicEq",
    "multiband.expander",
    "multiband.imager",
    "multiband.limiter",
    "multiband.saturation",
    "repair.declick",
    "repair.declip",
    "repair.decrackle",
    "repair.dehum",
    "repair.denoiseClassical",
    "repair.dereverbClassical",
    "repair.trimSilence",
    "saturation.ampSim",
    "saturation.bitcrusher",
    "saturation.distortion",
    "saturation.exciter",
    "saturation.hardClipper",
    "saturation.multibandExciter",
    "saturation.overdrive",
    "saturation.softClipper",
    "saturation.tape",
    "saturation.transformer",
    "saturation.tube",
    "saturation.waveshaper",
    "spectral.airBand",
    "spectral.lowEndFocus",
    "spectral.presenceEnhancer",
    "spectral.spectralShaper",
    "stereo.autoPan",
    "stereo.binaural",
    "stereo.haasEnhancer",
    "stereo.imager",
    "stereo.monoMaker",
    "stereo.phaseAlign",
    "stereo.stereoBalance",
    "utility.gain",
    "voice.changer",
]
# END GENERATED SoloProcessor
"""A processor name accepted by ``mastering_apply_processor()`` and its stereo form."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringResult:
    """Mastering loudness/true-peak processing result."""

    samples: npt.NDArray[np.float32]
    """Mono output samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS."""
    applied_gain_db: float
    """Sum of the static gains the stages applied, in dB; limiter gain reduction is not included."""
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

    left: npt.NDArray[np.float32]
    """Left output channel samples."""
    right: npt.NDArray[np.float32]
    """Right output channel samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS."""
    applied_gain_db: float
    """Sum of the static gains the stages applied, in dB; limiter gain reduction is not included."""
    latency_samples: int = 0
    """Processor latency in samples; the output is already compensated for it."""
    loudness_target_limited: bool = False
    """True when the true-peak ceiling prevented reaching the target loudness."""
    #: See :class:`MasteringResult`. Aggregated over both channels.
    non_finite_substitution_count: int = 0
    """Number of samples the processor replaced with a finite value."""


@dataclass(frozen=True, slots=True, eq=False)
class LoudnessMatchResult:
    """Result of :func:`mastering_ab_match_loudness`: the matched take and the match.

    ``applied_gain_db`` is the gain that lands the remeasured source on
    ``reference_lufs``; it equals ``reference_lufs - source_lufs`` unless the
    gain moves blocks across the absolute gate. It carries no upper bound, so
    ``matched_true_peak_dbtp`` reports where the gain left the peak instead of
    the match being capped to keep it. A take with no measurable loudness
    (silent or below the gate) is refused with :class:`SonareValueError`.
    """

    samples: npt.NDArray[np.float32]
    """The matched take as float32."""
    sample_rate: int
    """Sample rate of ``samples`` in Hz."""
    reference_lufs: float
    """Integrated loudness of the reference in LUFS."""
    source_lufs: float
    """Integrated loudness of the source in LUFS."""
    applied_gain_db: float
    """Gain applied to the source, in dB."""
    matched_true_peak_dbtp: float
    """True peak of the matched take after the gain, in dBTP."""


@dataclass(frozen=True, slots=True, eq=False)
class LoudnessMatchStereoResult:
    """Result of :func:`mastering_ab_match_loudness_stereo`; see :class:`LoudnessMatchResult`."""

    left: npt.NDArray[np.float32]
    """The matched left channel as float32."""
    right: npt.NDArray[np.float32]
    """The matched right channel as float32."""
    sample_rate: int
    """Sample rate of both channels in Hz."""
    reference_lufs: float
    """Integrated loudness of the reference in LUFS."""
    source_lufs: float
    """Integrated loudness of the source in LUFS."""
    applied_gain_db: float
    """Shared gain applied to both channels, in dB."""
    matched_true_peak_dbtp: float
    """True peak of the matched take after the gain, in dBTP."""


@dataclass(frozen=True, slots=True, eq=False)
class DynamicsProcessorResult:
    """Result of a mastering dynamics processor."""

    samples: npt.NDArray[np.float32]
    """The processed audio as float32."""
    latency_samples: int
    """Processor latency in samples."""


@dataclass(frozen=True, slots=True)
class StreamingLoudnessGain:
    """The loudness numbers a :class:`StreamingMasteringChain` is built with.

    Measured at the loudness stage's input, after every earlier enabled stage,
    which is where the offline chain measures, so ``loudness_static_gain_db``
    equals the gain :func:`master_audio` applies, ceiling clamp included.
    Pass ``loudness_static_gain_db`` and ``true_peak_dbtp`` as the constructor's
    ``loudness_static_gain_db`` and ``loudness_static_gain_peak_dbtp``. A silent or
    below-gate stage input has no gain (``None``, which the constructor applies as
    none); its ``integrated_lufs`` is ``-inf``, and digital silence has a
    ``true_peak_dbtp`` of ``-inf``.
    """

    loudness_static_gain_db: float | None
    """Static gain of the loudness stage in dB, ceiling clamp included; None for a silent or
    below-gate input."""
    true_peak_dbtp: float
    """True peak at the stage input in dBTP; -inf for digital silence."""
    integrated_lufs: float
    """Integrated loudness at the stage input in LUFS; -inf for a silent or below-gate input."""


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
    """Integrated loudness in LUFS; -inf for silence or when nothing passes the gate."""
    max_momentary_lufs: float | None
    """Maximum momentary loudness in LUFS; -inf for silence, None when the signal is shorter than
    one window."""
    max_short_term_lufs: float | None
    """Maximum short-term loudness in LUFS; -inf for silence, None when the signal is shorter than
    one window."""
    true_peak_dbtp: float
    """True peak, in dBTP; -inf for silence."""
    loudness_range_lu: float | None
    """EBU Tech 3342 loudness range, in LU; None when not measured."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringReport:
    """Compact explanation of how an offline mastering chain changed a program."""

    before: MasteringLoudnessSummary
    """Measurements of the input."""
    after: MasteringLoudnessSummary
    """Measurements of the output."""
    applied_gain_db: float
    """Sum of the static gains the stages applied, in dB; limiter gain reduction is not included."""
    max_gain_reduction_db: float
    """Most-negative final dynamics/limiter gain reduction in dB; zero when none ran."""
    loudness_target_limited: bool
    """True when peak headroom prevented the requested LUFS target."""
    band_energy_delta_db: npt.NDArray[np.float32] = field(default_factory=_empty_float32)
    """32 log-spaced after-minus-before spectral energy deltas in dB, 20 Hz to Nyquist; NaN in a
    band where either spectrum has no level."""
    loudness_gain_db: float | None = None
    """Static gain the loudness stage applied, in dB; 0 when the input already sat at the target.

    None when not measured: the stage is disabled, or the input is too quiet for an integrated
    loudness.
    """


@dataclass(frozen=True, slots=True, eq=False)
class MasteringChainResult:
    """Result of running a configurable mastering chain on mono audio."""

    samples: npt.NDArray[np.float32]
    """Mono output samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS; the achieved value, not the target."""
    applied_gain_db: float
    """Sum of the static gains the stages applied, in dB; limiter gain reduction is not included."""
    stages: list[str]
    """Identifiers of the stages the chain applied."""
    #: ITU-R BS.1770-4 true peak of the output (dBTP). The oversample factor
    #: follows the peak-limiting stage the chain actually applied, so it is not
    #: fixed: the loudness stage's true-peak oversample (default 4x) when
    #: loudness is enabled, and the maximizer true-peak limiter's own factor
    #: when loudness is disabled but that stage ran. The two disagree by
    #: roughly 0.02 dB between 4x and 8x.
    output_true_peak_dbtp: float = 0.0
    """True peak of the output, in dBTP; -inf for silence."""
    #: EBU Tech 3342 Loudness Range of the output (LU).
    output_loudness_range_lu: float | None = None
    """EBU Tech 3342 loudness range of the output, in LU; None when not measured."""
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
    loudness_gain_db: float | None = None
    """Same value as ``report.loudness_gain_db``; None when not measured."""


@dataclass(frozen=True, slots=True, eq=False)
class MasteringChainStereoResult:
    """Result of running a configurable mastering chain on stereo audio."""

    left: npt.NDArray[np.float32]
    """Left output channel samples."""
    right: npt.NDArray[np.float32]
    """Right output channel samples."""
    sample_rate: int
    """Sample rate of the output in Hz."""
    input_lufs: float
    """Integrated loudness of the input in LUFS."""
    output_lufs: float
    """Integrated loudness of the output in LUFS; the achieved value, not the target."""
    applied_gain_db: float
    """Sum of the static gains the stages applied, in dB; limiter gain reduction is not included."""
    stages: list[str]
    """Identifiers of the stages the chain applied."""
    #: See :class:`MasteringChainResult` for field semantics.
    output_true_peak_dbtp: float = 0.0
    """True peak of the output, in dBTP; -inf for silence."""
    output_loudness_range_lu: float | None = None
    """EBU Tech 3342 loudness range of the output, in LU; None when not measured."""
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
    loudness_gain_db: float | None = None
    """Same value as ``report.loudness_gain_db``; None when not measured."""
