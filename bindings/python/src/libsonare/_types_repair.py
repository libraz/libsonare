"""Audio-repair result shapes.

Each defect family carries a detection describing what was found, a report
describing what the mono repair did, and a stereo result. The repair session
adds the six detections gathered into one record, one stage record per repair
stage, and the analysis that recommends them.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Literal, TypeAlias

if TYPE_CHECKING:
    import numpy as np
    import numpy.typing as npt
    from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class ClickDetection:
    """What one channel's click detector found, independent of repair.

    ``count`` is runs meeting the repair criteria; ``rejected`` is runs the
    criteria excluded as outliers. A large ``rejected`` says the configured
    run length or neighbor ratio is too tight for this material, not that the
    material is clean. A run inside a broadband transient such as a drum hit
    is not counted; the repair still acts on it.
    """

    count: int
    """Number of click runs meeting the repair criteria; a run is counted once, not per sample."""
    rejected: int
    """Number of runs the criteria excluded as outliers."""
    longest_run_samples: int
    """Length of the longest counted run, in samples."""
    per_second: float
    """Counted runs per second of input."""


@dataclass(frozen=True, slots=True)
class DeclickReport:
    """What one channel's declick pass found and did to it.

    ``linked_runs`` is the part of ``repaired_runs`` this channel's own
    detection did not produce -- always 0 from the mono declick entry point,
    normally non-zero from the stereo one. ``lpc_model_used`` is False when
    the input was too short for ``lpc_order``, which reduces every fill to
    linear interpolation.
    """

    detected: ClickDetection
    """This channel's own click detection on the input, before repair."""
    repaired_runs: int
    """Number of runs interpolated, including runs inside a transient that ``detected.count``
    leaves out."""
    repaired_samples: int
    """Number of samples overwritten by interpolation."""
    linked_runs: int
    """Repaired runs this channel's own detection did not produce; always 0 from the mono entry
    point."""
    lpc_model_used: bool
    """False when the input was too short for ``lpc_order`` and every fill used linear
    interpolation."""


@dataclass(frozen=True, slots=True, eq=False)
class DeclickResult:
    """A declicked mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The declicked mono buffer, same length as the input."""
    report: DeclickReport
    """What the declick pass found and did."""


@dataclass(frozen=True, slots=True, eq=False)
class DeclickStereoResult:
    """A declicked stereo pair and what each channel's pass did.

    A run either channel's detector selects is repaired in both, so a
    common-mode click never moves the stereo image; only the selection is
    shared, and each channel's fill comes from its own samples and its own
    model, which is why the two reports can differ.
    """

    left: npt.NDArray[np.float32]
    """The declicked left channel."""
    right: npt.NDArray[np.float32]
    """The declicked right channel."""
    length: int
    """Length of each output channel, in samples."""
    left_report: DeclickReport
    """What the declick pass found and did on the left channel."""
    right_report: DeclickReport
    """What the declick pass found and did on the right channel."""


@dataclass(frozen=True, slots=True)
class ClipDetection:
    """What one channel's clip detector found, independent of repair.

    ``clipped_ratio`` is ``clipped_samples`` divided by the input length.
    ``longest_run_samples`` past the 512-sample cap takes the interpolation
    fallback instead of the LPC solver.

    The flat-top fields answer a different question: ``clipped_samples`` and
    ``longest_run_samples`` are read against ``clip_threshold_db``, so they count
    the apex of any waveform that reaches it and miss material clipped before
    it was attenuated. The flat-top fields instead count runs of at least
    three consecutive bit-identical samples, at least -40 dBFS, within 1 dB of the
    channel's flat level, so they still fire on a clipped tone that was
    attenuated afterward and a loud transient elsewhere does not hide them. A genuinely
    flat-topped waveform -- a square or
    pulse train, or a fully limited master -- counts as clipped here too and
    cannot be told apart from it in the time domain. ``flat_level`` is the
    magnitude the counted runs sit at -- the largest run level once the two
    highest runs are set aside -- ``None`` when there are none.

    The reverse also holds, and matters more: anything that moves samples
    independently erases a real flat top, so a zero here is not proof the
    material was never clipped. Resampling, lossy coding, and a stereo
    downmix all do this -- a downmix in particular, since the two channels
    are rarely bit-identical, so averaging them moves each sample by a
    different amount and a plateau stops being exactly level. Detect each
    channel before mixing them down, not after.
    """

    clipped_samples: int
    """Number of samples at or past ``clip_threshold_db``."""
    clipped_ratio: float
    """``clipped_samples`` divided by the input length, in 0..1."""
    run_count: int
    """Number of runs of consecutive clipped samples."""
    longest_run_samples: int
    """Length of the longest run of clipped samples, in samples."""
    flat_run_count: int
    """Number of flat-top runs (consecutive bit-identical samples)."""
    longest_flat_run_samples: int
    """Length of the longest flat-top run, in samples."""
    flat_sample_count: int
    """Number of samples inside the flat-top runs."""
    flat_level: float | None
    """Magnitude the counted flat-top runs sit at (linear); None when there are none."""


@dataclass(frozen=True, slots=True)
class DeclipReport:
    """What one channel's declip pass found and did to it.

    ``lpc_reconstructed_runs`` is runs the solver filled;
    ``unreconstructed_runs`` is runs left clipped: past the 512-sample gap cap,
    without a usable solve, or reconstructed more than 20 dB above the
    threshold. ``linked_runs`` is the part of the repaired
    runs reaching past this channel's own clipped samples because the other
    channel's run was wider -- always 0 unless ``link_channels`` was set on a
    multichannel entry point.
    """

    detected: ClipDetection
    """This channel's own clip detection on the input, before repair."""
    lpc_reconstructed_runs: int
    """Number of runs the LPC solver filled."""
    unreconstructed_runs: int
    """Number of runs left clipped rather than reconstructed."""
    repaired_samples: int
    """Number of samples in the reconstructed runs."""
    linked_runs: int
    """Repaired runs reaching past this channel's own clipped samples; always 0 unless
    ``link_channels`` was set on a multichannel entry point."""


@dataclass(frozen=True, slots=True, eq=False)
class DeclipResult:
    """A declipped mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The declipped mono buffer, same length as the input."""
    report: DeclipReport
    """What the declip pass found and did."""


@dataclass(frozen=True, slots=True, eq=False)
class DeclipStereoResult:
    """A declipped stereo pair and what each channel's pass did.

    Each channel repairs its own clipped runs unless ``link_channels`` was
    set, in which case each reconstructs every run either channel clipped
    that it has at least one clipped sample in; ``linked_runs`` in each report
    is always 0 unless ``link_channels`` was set.
    """

    left: npt.NDArray[np.float32]
    """The declipped left channel."""
    right: npt.NDArray[np.float32]
    """The declipped right channel."""
    length: int
    """Length of each output channel, in samples."""
    left_report: DeclipReport
    """What the declip pass found and did on the left channel."""
    right_report: DeclipReport
    """What the declip pass found and did on the right channel."""


@dataclass(frozen=True, slots=True)
class CrackleDetection:
    """What one channel's crackle detector found, independent of repair.

    Measured by the median criterion regardless of the configured mode --
    wavelet shrinkage removes crackle without ever deciding a sample is
    crackle, so this is the only definition of the defect either mode reports
    against. A sample inside a broadband transient such as a drum hit is not
    counted; the repair still acts on it.
    """

    sample_count: int
    """Number of samples deviating from the local median by more than the threshold."""
    sample_fraction: float
    """``sample_count`` divided by the input length, in 0..1."""
    per_second: float
    """Crackle samples counted per second of input."""


@dataclass(frozen=True, slots=True)
class DecrackleReport:
    """What one channel's decrackle pass found and did to it.

    The two modes remove crackle by different means and report through
    different fields: ``replaced_samples`` is median-mode only,
    ``detail_coefficients`` / ``shrunk_coefficients`` / ``noise_sigma`` are
    wavelet-mode only. A field belonging to the other mode reads zero because
    that mode did not run, not because it went unmeasured. The wavelet fields
    describe the unshifted pass, not every pass the mode averages.
    """

    detected: CrackleDetection
    """This channel's own crackle detection on the input, before repair."""
    replaced_samples: int
    """Median mode only: samples the filter overwrote; 0 in wavelet mode."""
    detail_coefficients: int
    """Wavelet mode only: detail coefficients examined by the unshifted pass; 0 in median mode."""
    shrunk_coefficients: int
    """Wavelet mode only: of the examined coefficients, those driven to zero; 0 in median mode."""
    noise_sigma: float
    """Wavelet mode only: the MAD noise estimate that set every level's threshold; 0 in median
    mode."""


@dataclass(frozen=True, slots=True, eq=False)
class DecrackleResult:
    """A decrackled mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The decrackled mono buffer, same length as the input."""
    report: DecrackleReport
    """What the decrackle pass found and did."""


@dataclass(frozen=True, slots=True, eq=False)
class DecrackleStereoResult:
    """A decrackled stereo pair and what each channel's pass did.

    Crackle is surface damage landing at different instants in each channel,
    so there is no common event for the two channels to agree about: each
    channel is decrackled independently, unlike the linked-run repairs of
    declick and declip.
    """

    left: npt.NDArray[np.float32]
    """The decrackled left channel."""
    right: npt.NDArray[np.float32]
    """The decrackled right channel."""
    length: int
    """Length of each output channel, in samples."""
    left_report: DecrackleReport
    """What the decrackle pass found and did on the left channel."""
    right_report: DecrackleReport
    """What the decrackle pass found and did on the right channel."""


@dataclass(frozen=True, slots=True, eq=False)
class HumDetection:
    """What one channel's hum detector found, independent of repair.

    Always measured from that channel's own input, whatever ``adaptive`` in
    the config says -- the fixed path notches the configured frequency
    without ever looking for hum, so a detector following the flag would
    hand back its own input. ``harmonic_dbfs`` is measured for every
    harmonic the sample rate carries, not only the notched ones, and ends before the
    first harmonic at or past Nyquist, where nothing is there to measure; a harmonic
    with no energy reads ``-inf``.
    ``fundamental_prominence`` is a linear energy ratio (not dB); 1.0 means no
    peak was found.
    """

    fundamental_hz: float
    """Tracked fundamental in Hz; the configured value when adaptive tracking is off."""
    fundamental_prominence: float
    """Linear energy ratio (not dB) of the winning candidate over the median candidate; 1.0 means
    no peak."""
    harmonics: int
    """Number of harmonics above the floor; 0 when ``fundamental_prominence`` is 1.0."""
    harmonic_dbfs: npt.NDArray[np.float32]
    """Input level in dBFS at each k times the fundamental, k ascending; at most 16 entries."""


@dataclass(frozen=True, slots=True)
class DehumReport:
    """What one channel's dehum pass found and did to it.

    ``fundamental_drift_hz`` is exactly zero without adaptive tracking --
    the measurement rather than an unset field.
    """

    detected: HumDetection
    """This channel's own hum detection on the input, before filtering."""
    notched_harmonics: int
    """Number of harmonics the filter cascade reached; fewer than ``harmonics`` once a harmonic
    hits Nyquist."""
    applied_fundamental_hz: float
    """Frequency in Hz the last notch refresh used."""
    fundamental_drift_hz: float
    """Largest excursion in Hz of the tracked frequency from the configured one; 0 without adaptive
    tracking."""


@dataclass(frozen=True, slots=True, eq=False)
class DehumResult:
    """A dehummed mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The dehummed mono buffer, same length as the input."""
    report: DehumReport
    """What the dehum pass found and did."""


@dataclass(frozen=True, slots=True, eq=False)
class DehumStereoResult:
    """A dehummed stereo pair and what each channel's pass did.

    Mains hum is one physical source, so with ``adaptive`` set the tracker
    reads the channel mean and both channels' cascades follow the one
    frequency it finds: ``applied_fundamental_hz`` and
    ``fundamental_drift_hz`` come out identical in both reports, even when
    the two channels' own ``detected`` measurements differ. Only the
    frequency is shared -- each channel keeps its own filter state. With
    ``adaptive`` clear, which is the default, nothing is shared and the two
    channels are filtered independently at the configured frequency.
    """

    left: npt.NDArray[np.float32]
    """The dehummed left channel."""
    right: npt.NDArray[np.float32]
    """The dehummed right channel."""
    length: int
    """Length of each output channel, in samples."""
    left_report: DehumReport
    """What the dehum pass found and did on the left channel."""
    right_report: DehumReport
    """What the dehum pass found and did on the right channel."""


@dataclass(frozen=True, slots=True, eq=False)
class NoiseDetection:
    """The noise floor a denoise pass estimated, before the mask.

    Absolute levels, which makes these the one part of a stereo denoise
    report that depends on how many channels were passed: the estimator runs
    on the channel-summed power, so two identical channels read about 3 dB
    above the same material through :func:`mastering_repair_denoise_classical`.
    Compare a stereo floor against another stereo floor, never against a
    mono one.
    """

    floor_dbfs: float | None
    """Broadband estimated noise floor in dBFS; -inf for digital silence, None when there is
    nothing to measure from."""
    band_floor_dbfs: npt.NDArray[np.float32]
    """Noise floor in dBFS of each band holding an analysis bin, low to high, on a 32-band
    geometric grid from 20 Hz to Nyquist; ``band_floor_band`` names each entry's band, and a band
    of zero power reads -inf."""
    band_floor_band: npt.NDArray[np.int32]
    """Band index, in ``[0, 32)``, of each ``band_floor_dbfs`` entry, ascending."""


@dataclass(frozen=True, slots=True)
class DenoiseReport:
    """What a denoise pass found and what it removed.

    ``floor_limited_fraction`` is always 0 in ``"spectralSubtraction"``
    mode, which floors on ``spectral_floor`` instead -- 0 from that mode is
    the mode and not a measurement. ``mean_reduction_db`` of 0 reads the
    same whether the mask was transparent or no mask ran at all.
    """

    detected: NoiseDetection
    """Noise-floor estimate of the input, before the mask."""
    mean_reduction_db: float
    """Mean attenuation in dB the gain mask applied; 0 also when no mask ran."""
    max_reduction_db: float
    """Deepest attenuation in dB any cell received."""
    floor_limited_fraction: float
    """Fraction of cells sitting on the ``reduction_db`` floor; always 0 in
    ``"spectralSubtraction"`` mode."""


@dataclass(frozen=True, slots=True, eq=False)
class DenoiseResult:
    """A denoised mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The denoised mono buffer, same length as the input."""
    report: DenoiseReport
    """What the denoise pass found and removed."""


@dataclass(frozen=True, slots=True, eq=False)
class DenoiseStereoResult:
    """A denoised stereo pair and the one mask that produced it.

    The mask is built from the channel-summed power and applied unchanged to
    both channels, so the pass cannot move an interchannel level or phase
    difference. That is also why there is one ``report`` rather than one per
    channel: a pair would be two copies of one measurement and would read as
    though the two could differ.
    """

    left: npt.NDArray[np.float32]
    """The denoised left channel."""
    right: npt.NDArray[np.float32]
    """The denoised right channel."""
    length: int
    """Length of each output channel, in samples."""
    report: DenoiseReport
    """What the single shared denoise mask found and removed."""


@dataclass(frozen=True, slots=True)
class DenoiseLinkedResult:
    """Any number of denoised channels and the one mask that produced them.

    ``channels`` holds one output buffer per input channel, in input order.
    The N-channel form of :class:`DenoiseStereoResult`, carrying the same
    guarantee for the whole set: one mask over the channel-summed power,
    applied unchanged to every channel, so no interchannel level or phase
    difference moves however many channels there are.

    ``report.detected`` is the one part that moves with the channel count --
    :class:`NoiseDetection` carries absolute levels, and they are the SET's,
    so N identical channels read ``10*log10(N)`` dB above one of them. Every
    attenuation figure on :class:`DenoiseReport` is a fraction and stays put.
    """

    channels: list[NDArray[np.float32]]
    """One denoised output buffer per input channel, in input order."""
    report: DenoiseReport
    """What the single shared denoise mask found and removed."""


@dataclass(frozen=True, slots=True)
class ReverbDetection:
    """What a dereverb pass measured while deciding how much to subtract.

    NOT an ISO 3382 reverberation time: no Schroeder integration, no
    noise-floor truncation, STFT bins rather than octave bands, and music is
    not a free decay. Use :func:`detect_acoustic` for a graded RT60.

    ``late_decay_ratio_db`` less negative means the material sustains across
    the module's own late lag, which a late tail does and a dry offset does
    not, so a reverberant input reads HIGHER here than the same material
    dry. ``late_predictability`` is None whenever ``wpe_enabled`` is clear --
    the default -- because the WPE stage that measures it did not run.
    """

    late_decay_ratio_db: float
    """Decay in dB across the module's own late lag; higher (less negative) means more sustained
    reverberation."""
    late_predictability: float | None
    """Mean WPE predictor norm before the clamp; None when ``wpe_enabled`` is clear."""


@dataclass(frozen=True, slots=True)
class DereverbReport:
    """What a dereverb pass found and what it removed.

    ``suppressed_fraction`` is the only observation of the ``threshold``
    knob: 0 alongside a nonzero ``mean_reduction_db`` says the gate admitted
    nothing. ``wpe_predictor_norm`` below ``detected.late_predictability``
    says the clamp acted, an otherwise silent branch; it is zero when the
    WPE stage did not run.
    """

    detected: ReverbDetection
    """Reverberation measurements of the input."""
    mean_reduction_db: float
    """Mean attenuation in dB the subtraction applied."""
    suppressed_fraction: float
    """Fraction of cells the ``threshold`` gate admitted as late reverberation."""
    wpe_predictor_norm: float
    """Mean WPE predictor norm actually applied, after the clamp; 0 when WPE did not run."""


@dataclass(frozen=True, slots=True, eq=False)
class DereverbResult:
    """A dereverberated mono buffer and what the pass did to it."""

    samples: NDArray[np.float32]
    """The dereverberated mono buffer, same length as the input."""
    report: DereverbReport
    """What the dereverb pass found and removed."""


@dataclass(frozen=True, slots=True, eq=False)
class DereverbStereoResult:
    """A dereverberated stereo pair and the one mask that produced it.

    The mask is built from the channel-summed power, and the WPE stage
    accumulates over both channels and applies one predictor set to each, so
    neither stage can move an interchannel level or phase difference. That
    is also why there is one ``report`` rather than one per channel.

    Every field of that report is a ratio or a fraction, so unlike
    :class:`NoiseDetection` nothing here shifts with the channel count and a
    stereo figure is comparable against a mono one.
    """

    left: npt.NDArray[np.float32]
    """The dereverberated left channel."""
    right: npt.NDArray[np.float32]
    """The dereverberated right channel."""
    length: int
    """Length of each output channel, in samples."""
    report: DereverbReport
    """What the single shared dereverb mask found and removed."""


@dataclass(frozen=True, slots=True)
class DereverbLinkedResult:
    """Any number of dereverberated channels and the one mask behind them.

    ``channels`` holds one output buffer per input channel, in input order.
    The N-channel form of :class:`DereverbStereoResult`: one mask over the
    channel-summed power and one WPE predictor set fitted over every
    channel's statistics, so neither stage can move an interchannel level or
    phase difference however many channels there are.

    Every field of ``report`` is a ratio or a fraction, so unlike
    :class:`DenoiseLinkedResult` nothing here shifts with the channel count
    and a figure measured over a set is comparable against a mono one.
    """

    channels: list[NDArray[np.float32]]
    """One dereverberated output buffer per input channel, in input order."""
    report: DereverbReport
    """What the single shared dereverb mask found and removed."""


@dataclass(frozen=True, slots=True)
class RepairChannelChange:
    """How much one repair stage changed one channel."""

    changed_samples: int
    """Samples whose value the stage changed."""
    change_level_db: float
    """Level in dBFS of the difference the stage made, as an RMS over the channel; ``-inf`` when
    nothing changed."""


@dataclass(frozen=True, slots=True)
class RepairStageReports:
    """One stage a repair application ran and what it reported.

    ``scope`` is ``"channel"`` with one report per channel, in input order, or
    ``"linked"`` with exactly one report for the whole set: denoise and dereverb
    apply one mask to every channel.
    """

    stage: str
    """Name of the stage that ran; a :data:`RepairStageName` token."""
    scope: str
    """``"channel"`` for one report per channel, ``"linked"`` for one report for the whole set."""
    reports: list[
        DeclipReport
        | DeclickReport
        | DecrackleReport
        | DehumReport
        | DenoiseReport
        | DereverbReport
    ]
    """Per-channel reports in input order for ``"channel"`` scope, or exactly one report for
    ``"linked"`` scope."""
    changes: list[RepairChannelChange]
    """One entry per input channel, in input order, whatever the ``scope``."""


@dataclass(frozen=True, slots=True)
class RepairApplyResult:
    """Repaired channels, in input order, and one entry per applied stage, in application order."""

    channels: list[NDArray[np.float32]]
    """The repaired channels, in input order."""
    reports: list[RepairStageReports]
    """One entry per applied stage, in application order."""


@dataclass(frozen=True, slots=True)
class AudioDefects:
    """What the six repair detectors measured, one detection per detector.

    The audio profile and the repair analysis both report it. ``reverb.late_predictability``
    is None there, since neither runs the WPE stage.
    """

    click: ClickDetection
    """Click detection for the channel or channel set."""
    clip: ClipDetection
    """Clipping detection, including the flat-top measurements."""
    crackle: CrackleDetection
    """Crackle detection, measured by the median criterion."""
    noise: NoiseDetection
    """Noise-floor estimate, in dBFS."""
    hum: HumDetection
    """Mains-hum detection."""
    reverb: ReverbDetection
    """Reverberation measurements taken while deciding how much a dereverb would subtract."""


RepairStageName: TypeAlias = Literal[
    "declip", "declick", "decrackle", "dehum", "denoise", "dereverb"
]
"""The repair stages, in the order :func:`mastering_repair_apply` runs them."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DeclipStage:
    """Declip settings as :func:`mastering_repair_declip` takes them; None is its default."""

    stage: Literal["declip"] = field(default="declip", init=False)
    """Stage discriminator; always ``"declip"``."""
    clip_threshold_db: float | None = None
    """Level in dBFS at or above which a sample counts as clipped."""
    lpc_order: int | None = None
    """LPC order used for prediction."""
    iterations: int | None = None
    """Number of LPC reconstruction iterations."""
    link_channels: bool | None = None
    """Reconstruct the union of every channel's clipped runs in each channel; by default (False)
    each channel repairs only its own."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DeclickStage:
    """Declick settings as :func:`mastering_repair_declick` takes them; None is its default."""

    stage: Literal["declick"] = field(default="declick", init=False)
    """Stage discriminator; always ``"declick"``."""
    threshold: float | None = None
    """Amplitude threshold against the LPC prediction."""
    neighbor_ratio: float | None = None
    """Click amplitude ratio against the neighbouring samples."""
    max_click_samples: int | None = None
    """Maximum click run length, in samples."""
    lpc_order: int | None = None
    """LPC order used for prediction."""
    residual_ratio: float | None = None
    """Residual-to-signal ratio threshold."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DecrackleStage:
    """Decrackle settings as :func:`mastering_repair_decrackle` takes them; None is its default."""

    stage: Literal["decrackle"] = field(default="decrackle", init=False)
    """Stage discriminator; always ``"decrackle"``."""
    threshold: float | None = None
    """Median mode: deviation threshold. Wavelet mode: a cap on the BayesShrink threshold."""
    mode: str | None = None
    """Algorithm: median filter or wavelet shrinkage, as :func:`mastering_repair_decrackle` takes
    it."""
    levels: int | None = None
    """Number of Haar wavelet decomposition levels (wavelet mode)."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DehumStage:
    """Dehum settings as :func:`mastering_repair_dehum` takes them; None is its default."""

    stage: Literal["dehum"] = field(default="dehum", init=False)
    """Stage discriminator; always ``"dehum"``."""
    fundamental_hz: float | None = None
    """Mains-hum fundamental in Hz."""
    harmonics: int | None = None
    """Number of notches, counting the fundamental."""
    q: float | None = None
    """Notch Q."""
    adaptive: bool | None = None
    """Whether adaptive frequency tracking is enabled."""
    search_range_hz: float | None = None
    """Tracking search range in Hz."""
    adaptation: float | None = None
    """Tracking step size."""
    frame_size: int | None = None
    """Analysis frame size in samples; at least 16."""
    pll_bandwidth: float | None = None
    """PLL bandwidth used by the tracker."""
    mode: str | None = None
    """Algorithm: ``"subtract"`` or ``"notch"``."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DenoiseStage:
    """Linked denoise settings as :func:`mastering_repair_denoise_classical` takes them.

    A setting left as None is that function's default.
    """

    stage: Literal["denoise"] = field(default="denoise", init=False)
    """Stage discriminator; always ``"denoise"``."""
    mode: str | None = None
    """Algorithm: ``"logMmse"``, ``"mmseStsa"`` or ``"spectralSubtraction"``."""
    noise_estimator: str | None = None
    """Noise estimator: ``"quantile"``, ``"mcra"``, ``"imcra"`` or ``"spp"``."""
    n_fft: int | None = None
    """STFT size in samples; a power of two."""
    hop_length: int | None = None
    """Hop in samples; positive and at most ``n_fft // 2``."""
    dd_alpha: float | None = None
    """Decision-directed a priori SNR smoothing."""
    reduction_db: float | None = None
    """Deepest attenuation the mask may apply, in dB; at least 0."""
    over_subtraction: float | None = None
    """Berouti alpha; read only by ``"spectralSubtraction"``."""
    spectral_floor: float | None = None
    """Berouti beta; read only by ``"spectralSubtraction"``."""
    noise_estimation_quantile: float | None = None
    """Fraction of frames assumed noise-only."""
    speech_presence_gain: bool | None = None
    """Whether speech-presence probability gating is applied."""
    gain_smoothing: bool | None = None
    """Whether gains are smoothed across time."""


@dataclass(frozen=True, slots=True, kw_only=True)
class DereverbStage:
    """Linked dereverb settings as :func:`mastering_repair_dereverb_classical` takes them.

    A setting left as None is that function's default.
    """

    stage: Literal["dereverb"] = field(default="dereverb", init=False)
    """Stage discriminator; always ``"dereverb"``."""
    threshold: float | None = None
    """Late-reverberation detection threshold; 0 applies no gate."""
    attenuation: float | None = None
    """Linear suppression amount; 1 is full."""
    n_fft: int | None = None
    """STFT size in samples; a power of two."""
    hop_length: int | None = None
    """Hop in samples; positive and at most ``n_fft // 2``."""
    t60_sec: float | None = None
    """Estimated reverberation time T60, in seconds."""
    late_delay_ms: float | None = None
    """Onset of the late reverberation relative to the direct sound, in ms."""
    over_subtraction: float | None = None
    """Berouti alpha."""
    spectral_floor: float | None = None
    """Berouti beta."""
    wpe_enabled: bool | None = None
    """Whether the WPE pre-stage runs."""
    wpe_iterations: int | None = None
    """Number of WPE EM iterations."""
    wpe_taps: int | None = None
    """Number of WPE filter taps."""
    wpe_strength: float | None = None
    """WPE blend weight."""


RepairStage: TypeAlias = (
    DeclipStage | DeclickStage | DecrackleStage | DehumStage | DenoiseStage | DereverbStage
)
"""One stage for :func:`mastering_repair_apply`, told apart by its ``stage`` attribute."""


@dataclass(frozen=True, slots=True)
class MasteringRepairAnalysis:
    """Result of :func:`mastering_repair_analyze`.

    ``defects`` aggregates every channel the way the stereo audio profile does and
    ``channels`` holds each channel's own, in input order; both are None when the
    input was too short for the detectors to run. ``recommended`` lists the stages
    the measurement supports, in application order and with every setting given,
    ready for :func:`mastering_repair_apply`. Dereverb is never recommended.
    """

    defects: AudioDefects | None
    """Defects aggregated over the channels; None when the input was too short for the detectors."""
    channels: list[AudioDefects] | None
    """Each channel's own defects, in input order; None exactly when ``defects`` is."""
    declip_threshold_safe: bool
    """Whether a declip at the flat level leaves louder audio alone; when False, declip is withheld
    from ``recommended``."""
    integrated_lufs: float
    """Integrated loudness in LUFS the noise floor is compared against; -inf for a silent or
    below-gate take."""
    recommended: list[RepairStage]
    """Stages the measurement supports, in application order, each with every setting given."""
    explanation: list[str]
    """Why each stage was or was not chosen."""
