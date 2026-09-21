"""Audio-repair result shapes.

Each defect family carries a detection describing what was found, a report
describing what the mono repair did, and a stereo result.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class ClickDetection:
    """What one channel's click detector found, independent of repair.

    ``count`` is runs meeting the repair criteria; ``rejected`` is runs the
    criteria excluded as outliers. A large ``rejected`` says the configured
    run length or neighbor ratio is too tight for this material, not that the
    material is clean.
    """

    count: int
    rejected: int
    longest_run_samples: int
    per_second: float


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
    repaired_runs: int
    repaired_samples: int
    linked_runs: int
    lpc_model_used: bool


@dataclass(frozen=True, slots=True)
class DeclickStereoResult:
    """A declicked stereo pair and what each channel's pass did.

    A run either channel's detector selects is repaired in both, so a
    common-mode click never moves the stereo image; only the selection is
    shared, and each channel's fill comes from its own samples and its own
    model, which is why the two reports can differ.
    """

    left: list[float]
    right: list[float]
    length: int
    left_report: DeclickReport
    right_report: DeclickReport


@dataclass(frozen=True, slots=True)
class ClipDetection:
    """What one channel's clip detector found, independent of repair.

    ``sample_fraction`` is ``sample_count`` divided by the input length.
    ``longest_run_samples`` past the 512-sample cap takes the interpolation
    fallback instead of the LPC solver.

    The flat-top fields answer a different question: ``sample_count`` and
    ``longest_run_samples`` are read against ``clip_threshold``, so they count
    the apex of any waveform that reaches it and miss material clipped before
    it was attenuated. The flat-top fields instead count runs of at least
    three consecutive bit-identical samples within 1 dB of the signal's peak,
    wherever that peak sits, so they still fire on a clipped tone that was
    attenuated afterward. A genuinely flat-topped waveform -- a square or
    pulse train, or a fully limited master -- counts as clipped here too and
    cannot be told apart from it in the time domain. ``flat_level`` is the
    magnitude the counted runs sit at, 0 when there are none.

    The reverse also holds, and matters more: anything that moves samples
    independently erases a real flat top, so a zero here is not proof the
    material was never clipped. Resampling, lossy coding, and a stereo
    downmix all do this -- a downmix in particular, since the two channels
    are rarely bit-identical, so averaging them moves each sample by a
    different amount and a plateau stops being exactly level. Detect each
    channel before mixing them down, not after.
    """

    sample_count: int
    sample_fraction: float
    run_count: int
    longest_run_samples: int
    flat_run_count: int
    longest_flat_run_samples: int
    flat_sample_count: int
    flat_level: float


@dataclass(frozen=True, slots=True)
class DeclipReport:
    """What one channel's declip pass found and did to it.

    ``lpc_reconstructed_runs`` is runs the solver filled;
    ``interpolated_runs`` is runs past the 512-sample cap filled by cubic /
    linear interpolation instead, for which ``lpc_order``, ``iterations`` and
    ``lpc_blend`` had no effect. ``linked_runs`` is the part of the repaired
    runs reaching past this channel's own clipped samples because the other
    channel's run was wider -- always 0 from the mono declip entry point.
    """

    detected: ClipDetection
    lpc_reconstructed_runs: int
    interpolated_runs: int
    repaired_samples: int
    linked_runs: int


@dataclass(frozen=True, slots=True)
class DeclipStereoResult:
    """A declipped stereo pair and what each channel's pass did.

    Declip repairs the union of both channels' clipped runs: each channel
    reconstructs the whole of every union run it has at least one clipped
    sample in, and is left untouched where it has none, so a run clipped in
    only one channel produces no linking at all.
    """

    left: list[float]
    right: list[float]
    length: int
    left_report: DeclipReport
    right_report: DeclipReport


@dataclass(frozen=True, slots=True)
class CrackleDetection:
    """What one channel's crackle detector found, independent of repair.

    Measured by the median criterion regardless of the configured mode --
    wavelet shrinkage removes crackle without ever deciding a sample is
    crackle, so this is the only definition of the defect either mode reports
    against.
    """

    sample_count: int
    sample_fraction: float
    per_second: float


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
    replaced_samples: int
    detail_coefficients: int
    shrunk_coefficients: int
    noise_sigma: float


@dataclass(frozen=True, slots=True)
class DecrackleStereoResult:
    """A decrackled stereo pair and what each channel's pass did.

    Crackle is surface damage landing at different instants in each channel,
    so there is no common event for the two channels to agree about: each
    channel is decrackled independently, unlike the linked-run repairs of
    declick and declip.
    """

    left: list[float]
    right: list[float]
    length: int
    left_report: DecrackleReport
    right_report: DecrackleReport


@dataclass(frozen=True, slots=True)
class HumDetection:
    """What one channel's hum detector found, independent of repair.

    Always measured from that channel's own input, whatever ``adaptive`` in
    the config says -- the fixed path notches the configured frequency
    without ever looking for hum, so a detector following the flag would
    hand back its own input. ``harmonic_dbfs`` is measured for every
    harmonic the sample rate carries, not only the notched ones; an entry
    past Nyquist reads the dB floor because nothing is there to measure.
    """

    fundamental_hz: float
    fundamental_prominence: float
    harmonics: int
    harmonic_dbfs: list[float]


@dataclass(frozen=True, slots=True)
class DehumReport:
    """What one channel's dehum pass found and did to it.

    ``fundamental_drift_hz`` is exactly zero without adaptive tracking --
    the measurement rather than an unset field.
    """

    detected: HumDetection
    notched_harmonics: int
    applied_fundamental_hz: float
    fundamental_drift_hz: float


@dataclass(frozen=True, slots=True)
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

    left: list[float]
    right: list[float]
    length: int
    left_report: DehumReport
    right_report: DehumReport


@dataclass(frozen=True, slots=True)
class NoiseDetection:
    """The noise floor a denoise pass estimated, before the mask.

    Absolute levels, which makes these the one part of a stereo denoise
    report that depends on how many channels were passed: the estimator runs
    on the channel-summed power, so two identical channels read about 3 dB
    above the same material through :func:`mastering_repair_denoise_classical`.
    Compare a stereo floor against another stereo floor, never against a
    mono one. ``band_floor_dbfs`` has 32 entries on a geometric grid from
    20 Hz to Nyquist, low to high.
    """

    floor_dbfs: float
    band_floor_dbfs: list[float]


@dataclass(frozen=True, slots=True)
class DenoiseReport:
    """What a denoise pass found and what it removed.

    ``floor_limited_fraction`` is always 0 in ``"spectralSubtraction"``
    mode, which floors on ``spectral_floor`` instead -- 0 from that mode is
    the mode and not a measurement. ``mean_reduction_db`` of 0 reads the
    same whether the mask was transparent or no mask ran at all.
    """

    detected: NoiseDetection
    mean_reduction_db: float
    max_reduction_db: float
    floor_limited_fraction: float


@dataclass(frozen=True, slots=True)
class DenoiseStereoResult:
    """A denoised stereo pair and the one mask that produced it.

    The mask is built from the channel-summed power and applied unchanged to
    both channels, so the pass cannot move an interchannel level or phase
    difference. That is also why there is one ``report`` rather than one per
    channel: a pair would be two copies of one measurement and would read as
    though the two could differ.
    """

    left: list[float]
    right: list[float]
    length: int
    report: DenoiseReport


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
    report: DenoiseReport


@dataclass(frozen=True, slots=True)
class ReverbDetection:
    """What a dereverb pass measured while deciding how much to subtract.

    NOT an ISO 3382 reverberation time: no Schroeder integration, no
    noise-floor truncation, STFT bins rather than octave bands, and music is
    not a free decay. Use :func:`detect_acoustic` for a graded RT60.

    ``late_decay_ratio_db`` less negative means the material sustains across
    the module's own late lag, which a late tail does and a dry offset does
    not, so a reverberant input reads HIGHER here than the same material
    dry. ``late_predictability`` is exactly zero whenever ``wpe_enabled`` is
    clear -- the default -- which is the measurement rather than an unset
    field.
    """

    late_decay_ratio_db: float
    late_predictability: float


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
    mean_reduction_db: float
    suppressed_fraction: float
    wpe_predictor_norm: float


@dataclass(frozen=True, slots=True)
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

    left: list[float]
    right: list[float]
    length: int
    report: DereverbReport


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
    report: DereverbReport
