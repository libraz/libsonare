/**
 * Defect detection and repair report types: declick, declip, decrackle,
 * denoise, dehum, dereverb and silence trim.
 */

/**
 * What a declick analysis found in one channel of a
 * {@link MasteringRepairDeclickResult}. Runs, not samples.
 */
export interface ClickDetection {
  /** Runs meeting the repair criteria. A run inside a broadband transient such as a drum hit is not counted; the repair still acts on it. */
  count: number;
  /**
   * Outlier runs the criteria excluded. A large value says `maxClickSamples`
   * or `neighborRatio` is too tight for this material, not that the material
   * is clean.
   */
  rejected: number;
  /** Over the counted runs. */
  longestRunSamples: number;
  /** `count` divided by the input duration. */
  perSecond: number;
}

/**
 * What a declick pass found in one channel of a
 * {@link MasteringRepairDeclickResult}, or of the mono `masteringRepairDeclick` call, and what it did to it.
 */
export interface DeclickReport {
  /** This channel's own analysis of the input. */
  detected: ClickDetection;
  /**
   * Runs interpolated. Larger than `detected.count` under linked stereo
   * detection, or where the repair acts on a run inside a transient the detector does not count.
   */
  repairedRuns: number;
  /** Samples overwritten by interpolation. */
  repairedSamples: number;
  /**
   * Of `repairedRuns`, those whose extent this channel's own detection did
   * not produce. Always 0 from the mono `masteringRepairDeclick`.
   */
  linkedRuns: number;
  /**
   * False when the input was too short for `lpcOrder`: every fill then
   * reduces to linear interpolation.
   */
  lpcModelUsed: boolean;
}

/** What `masteringRepairDeclick` returns: the repaired audio and what the pass did. */
export interface DeclickResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DeclickReport;
}

/**
 * A declicked stereo pair and what each channel's pass found and did.
 *
 * A run either channel's detector selects is repaired in BOTH channels — a
 * common-mode click repaired on one side only would move the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own AR model, which is why `reports[0]` and `reports[1]`
 * genuinely differ. `linkedRuns` is the part of `repairedRuns` this channel's
 * own detection did not produce, so it is normally non-zero here. Merged runs
 * can exceed `maxClickSamples`: that cap governs what may be selected, not
 * how far a selection reaches once both channels agree a click is there.
 */
export interface MasteringRepairDeclickResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** One report per channel: `[left, right]`. */
  reports: DeclickReport[];
}

/**
 * What a declip analysis found in one channel of a
 * {@link MasteringRepairDeclipResult}.
 */
export interface ClipDetection {
  /**
   * Samples at or past `clipThresholdDb`. The metering family's region report,
   * `ClippingReport`, counts the same two quantities under the same names.
   */
  clippedSamples: number;
  /** `clippedSamples` divided by the input length. */
  clippedRatio: number;
  /** Runs of consecutive clipped samples. */
  runCount: number;
  /** A run past the 512-sample cap takes the interpolation fallback instead of the solver. */
  longestRunSamples: number;
  /**
   * Runs of at least 3 bit-identical samples (at least -40 dBFS) sitting within 1 dB of the
   * channel's flat level, which a louder unclipped passage does not move. Reads
   * clipping that survived a later gain change and so no longer reaches `clipThresholdDb` — the
   * fields above see none of it. A genuinely flat-topped waveform (a square or pulse train, a
   * fully limited master) counts here too and cannot be told apart from clipping in the time
   * domain.
   */
  flatRunCount: number;
  /** Over the counted flat-top runs. */
  longestFlatRunSamples: number;
  /** Samples belonging to a counted flat-top run. */
  flatSampleCount: number;
  /**
   * The magnitude the counted runs sit at: the largest run level once the two highest runs
   * are set aside. Absent when no flat run was measured.
   */
  flatLevel?: number;
}

/**
 * What a declip pass found in one channel of a
 * {@link MasteringRepairDeclipResult}, or of the mono `masteringRepairDeclip` call, and what it did to it.
 */
export interface DeclipReport {
  /** This channel's own analysis of the input. */
  detected: ClipDetection;
  /** Runs the Janssen solver filled. */
  lpcReconstructedRuns: number;
  /**
   * Runs left clipped: past the 512-sample cap, with no usable solve, or
   * reconstructed more than 20 dB past the clip level.
   */
  unreconstructedRuns: number;
  /** Samples overwritten by the reconstructed runs. */
  repairedSamples: number;
  /**
   * Of the repaired runs, those reaching past this channel's own clipped
   * samples because the other channel's run was wider. Always 0 without
   * `linkChannels`, and from the mono `masteringRepairDeclip`.
   */
  linkedRuns: number;
}

/** What `masteringRepairDeclip` returns: the repaired audio and what the pass did. */
export interface DeclipResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DeclipReport;
}

/**
 * A declipped stereo pair and what each channel's pass found and did.
 *
 * By default each channel repairs its own clipped runs. With `linkChannels`,
 * each channel reconstructs the whole of every union run it has at least one
 * clipped sample in, and a channel with none is left untouched there.
 * `linkedRuns` is non-zero only then, where both channels are clipped in the
 * same region with different extents — the narrower channel is what reaches
 * past its own clipped samples.
 */
export interface MasteringRepairDeclipResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** One report per channel: `[left, right]`. */
  reports: DeclipReport[];
}

/**
 * What a decrackle analysis found in one channel of a
 * {@link MasteringRepairDecrackleResult}.
 *
 * Crackle is measured by the median criterion regardless of the configured `DecrackleMode` --
 * wavelet shrinkage is a removal method, not a detection method, so this is the module's only
 * definition of the defect.
 */
export interface CrackleDetection {
  /** Samples deviating from the local median by more than `threshold`. A sample inside a broadband transient such as a drum hit is not counted; the repair still acts on it. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  /** `sampleCount` divided by the input duration. */
  perSecond: number;
}

/**
 * What a decrackle pass found in one channel of a
 * {@link MasteringRepairDecrackleResult}, or of the mono `masteringRepairDecrackle` call, and what it did to it.
 *
 * The two modes remove crackle by different means and report through different fields. A field
 * belonging to the other mode reads zero because that mode did not run -- the caller knows this
 * from the config it passed, so it is not an unfilled value.
 */
export interface DecrackleReport {
  /** This channel's own analysis of the input. */
  detected: CrackleDetection;
  /** Median mode: samples the filter overwrote. Equal to `detected.sampleCount`. */
  replacedSamples: number;
  /** Wavelet mode: detail coefficients examined by the unshifted pass, not by every pass the mode averages. */
  detailCoefficients: number;
  /** Wavelet mode: of those, driven to zero. */
  shrunkCoefficients: number;
  /** Wavelet mode: the MAD noise estimate that set every level's threshold. The configured
   * `threshold` is only a cap on it. */
  noiseSigma: number;
}

/** What `masteringRepairDecrackle` returns: the repaired audio and what the pass did. */
export interface DecrackleResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DecrackleReport;
}

/**
 * A decrackled stereo pair and what each channel's pass found and did.
 *
 * Crackle is surface damage: the two channels carry different scratches at different instants,
 * so there is no common event for a shared decision to agree about. Both modes are memoryless
 * across channels, so the pair is processed independently and there is no `linkedRuns` field,
 * unlike {@link MasteringRepairDeclickResult} and {@link MasteringRepairDeclipResult}.
 */
export interface MasteringRepairDecrackleResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** One report per channel: `[left, right]`. */
  reports: DecrackleReport[];
}

/**
 * What a dehum analysis found in one channel of a
 * {@link MasteringRepairDehumResult}.
 *
 * Always measured through the estimation path, whatever `DehumOptions.adaptive` says: the
 * fixed path notches the configured frequency without ever looking for hum, so a detector
 * following the flag would hand back its own input.
 */
export interface HumDetection {
  /** Tracked fundamental; the configured value when adaptive tracking is off. */
  fundamentalHz: number;
  /** Winning candidate's projected energy over the median candidate, a linear ratio (not dB);
   * 1.0 means no peak: none was found, or the winner was not a stationary line through most analysis frames (a repeating kick reads 1.0). Not a lock flag. */
  fundamentalProminence: number;
  /** Harmonics found above the floor, not necessarily a contiguous run from the first. */
  harmonics: number;
  /**
   * Input level at each k*f0, k ascending, at most 16 entries. Measured for
   * every k the sample rate carries, not only the notched ones, and ending
   * before the first k*f0 at or past Nyquist, where nothing is there to
   * measure. A harmonic with no energy reads `-Infinity`.
   */
  harmonicDbfs: Float32Array;
}

/**
 * What a dehum pass found in one channel of a
 * {@link MasteringRepairDehumResult}, or of the mono `masteringRepairDehum` call, and what it did to it.
 */
export interface DehumReport {
  /** This channel's own analysis, before filtering. */
  detected: HumDetection;
  /** Harmonics the cascade reached; fewer than the configured `harmonics` once k*f0
   * hits Nyquist. */
  notchedHarmonics: number;
  /** Frequency the last notch refresh used. */
  appliedFundamentalHz: number;
  /** Largest excursion of the tracked frequency from the configured one. Zero without
   * adaptive tracking, which is the measurement rather than an unset field. */
  fundamentalDriftHz: number;
}

/** What `masteringRepairDehum` returns: the repaired audio and what the pass did. */
export interface DehumResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DehumReport;
}

/**
 * A dehummed stereo pair and what each channel's pass found and did.
 *
 * Mains hum is one physical source, so with `DehumOptions.adaptive` set the tracker reads
 * the channel mean and both cascades follow the one frequency it finds: `appliedFundamentalHz`
 * and `fundamentalDriftHz` are therefore identical in both reports by construction, while each
 * report's `detected` still measures that channel's own input and each channel keeps its own
 * filter state. With `adaptive` clear, which is the default, nothing is shared and the two
 * channels are filtered independently at the configured frequency.
 */
export interface MasteringRepairDehumResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** One report per channel: `[left, right]`. */
  reports: DehumReport[];
}

/**
 * What a denoise analysis found in a
 * {@link MasteringRepairDenoiseClassicalResult}.
 *
 * A pair-level measurement, and the only absolute one in that result: the estimator runs on
 * the channel-summed power, so two identical channels read `10*log10(2)` -- about
 * 3.01 dB -- above the same material through `masteringRepairDenoiseClassical`. Compare a stereo
 * floor only against another stereo floor.
 */
export interface NoiseDetection {
  /**
   * Broadband estimated noise floor, in dBFS; `-Infinity` for digital silence.
   * Absent when there is nothing to measure from.
   */
  floorDbfs?: number;
  /**
   * The noise floor's shape, low band to high, on a 32-band geometric grid
   * from 20 Hz to Nyquist -- the same axis the mastering report's
   * `bandEnergyDeltaDb` uses, so a noise floor and a tonal-balance change can
   * be read together. Only bands holding an analysis bin carry a level, so the
   * array can be shorter than 32; `bandFloorBand` names each entry's band. A
   * band of zero power reads `-Infinity`.
   */
  bandFloorDbfs: Float32Array;
  /**
   * Band index, in [0, 32), of each `bandFloorDbfs` entry, ascending. A band
   * narrower than the bin spacing holds no bin and is not listed.
   */
  bandFloorBand: Int32Array;
}

/**
 * What a denoise pass found in a
 * {@link MasteringRepairDenoiseClassicalResult} or in the mono result of `masteringRepairDenoiseClassical` and what it removed.
 */
export interface DenoiseReport {
  /** The pair's analysis, before the mask. */
  detected: NoiseDetection;
  /** Mean attenuation the gain mask applied. Zero reads the same whether the mask was
   * transparent or no mask ran at all. */
  meanReductionDb: number;
  /** Deepest attenuation any cell applied; at `reductionDb` the floor set the depth rather
   * than the estimate. */
  maxReductionDb: number;
  /** Fraction of cells sitting on that floor. Always 0 in `spectralSubtraction` mode, which
   * floors on `spectralFloor` instead, so 0 from that mode is the mode and not a
   * measurement. */
  floorLimitedFraction: number;
}

/** What `masteringRepairDenoiseClassical` returns: the repaired audio and what the pass did. */
export interface DenoiseResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DenoiseReport;
}

/**
 * A denoised stereo pair and the one mask that produced it.
 *
 * One report (`reports` holds only it) rather than a per-channel pair: the gain mask is built from the channel-summed
 * power and applied unchanged to both channels, so the pass cannot move an interchannel level
 * or phase difference, and a pair would be two copies of one measurement.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back to the input
 * length, as {@link MasteringRepairDereverbClassicalResult}'s entry point does.
 */
export interface MasteringRepairDenoiseClassicalResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** The one shared report: `[report]`. */
  reports: DenoiseReport[];
}

/**
 * What a dereverb analysis found in a
 * {@link MasteringRepairDereverbClassicalResult}.
 *
 * NOT an ISO 3382 reverberation time: no Schroeder integration, no noise-floor truncation,
 * STFT bins rather than octave bands, and music is not a free decay. Use `estimateRoom` for a
 * graded RT60; this reports what the module itself measured while deciding how much to
 * subtract.
 */
export interface ReverbDetection {
  /** Decay across the module's own late lag, in dB. Less negative means the material sustains
   * across that lag, which a late tail does and a dry offset does not -- so a reverberant
   * input reads HIGHER here than the same material dry. Absent when nothing was measured:
   * silence, or an input shorter than the late lag. */
  lateDecayRatioDb?: number;
  /** Mean WPE predictor norm, before the clamp. Absent whenever the WPE stage did not run,
   * which is the case unless `wpeEnabled` is set -- and it is clear by default, so a
   * default-config pass omits it. */
  latePredictability?: number;
}

/**
 * What a dereverb pass found in a
 * {@link MasteringRepairDereverbClassicalResult} or in the mono result of `masteringRepairDereverbClassical` and what it removed.
 */
export interface DereverbReport {
  /** The pair's analysis. */
  detected: ReverbDetection;
  /** Mean attenuation the subtraction applied. */
  meanReductionDb: number;
  /** Fraction of cells the `threshold` gate admitted as late reverberation. The only
   * observation of that knob: 0 alongside a nonzero `meanReductionDb` says the gate admitted
   * nothing. */
  suppressedFraction: number;
  /** Mean predictor norm actually applied, after the clamp. Below
   * `detected.latePredictability` says the clamp acted, an otherwise silent branch. Zero when
   * the WPE stage did not run, so 0 by default. */
  wpePredictorNorm: number;
}

/** What `masteringRepairDereverbClassical` returns: the repaired audio and what the pass did. */
export interface DereverbResult {
  /** Repaired audio. */
  samples: Float32Array;
  /** What the pass found and did. */
  report: DereverbReport;
}

/**
 * A dereverberated stereo pair and the one mask that produced it.
 *
 * One report (`reports` holds only it) rather than a per-channel pair: the mask is built from the channel-summed power
 * and the WPE stage accumulates over both channels and applies one predictor set to each, so
 * neither stage can move an interchannel level or phase difference.
 *
 * Every field of that report is a ratio or a fraction, so unlike {@link NoiseDetection}
 * nothing here shifts with the channel count and a stereo figure is comparable against a mono
 * one. An input shorter than `nFft` is padded for analysis, as
 * {@link MasteringRepairDenoiseClassicalResult}'s entry point does.
 */
export interface MasteringRepairDereverbClassicalResult {
  /** Output planes in input order: `[left, right]` for a stereo pair. */
  channels: Float32Array[];
  /** The one shared report: `[report]`. */
  reports: DereverbReport[];
}

/** One half-open sample range, in INPUT-buffer coordinates. */
export interface TrimRange {
  /** First kept sample. */
  first: number;
  /** One past the last kept sample. An empty range has `first >= lastExclusive`. */
  lastExclusive: number;
}

/**
 * What a trim pass kept and what it dropped.
 *
 * A pass that kept nothing reports `range` as `(inputLength, inputLength)`, which counts the
 * whole buffer as removed head and leaves `removedTailSamples` at 0. The two still sum to the
 * input length, so a caller reporting how much went reads the right total; only the split
 * between the ends is arbitrary there.
 */
export interface TrimReport {
  /** The kept range, padding included. */
  range: TrimRange;
  /** Samples dropped before `range.first`. */
  removedHeadSamples: number;
  /** Samples dropped after `range.lastExclusive`. */
  removedTailSamples: number;
}

/**
 * What `masteringRepairTrimSilence` returns: the trimmed audio and what the pass did. A trim that
 * kept nothing gives an empty `samples`, not an error.
 */
export interface TrimSilenceResult {
  /** Kept audio; empty when nothing was kept. */
  samples: Float32Array;
  /** What the trim pass found and did. */
  report: TrimReport;
}

/**
 * A trimmed stereo pair, the range both channels were cut to, and the two per-channel scans
 * that range is the union of.
 *
 * The only repair stereo result whose arrays are SHORTER than the input, so `channels[0].length` is
 * the output length and the input's says nothing about it. Both channels come back the same
 * length, because one range cuts both.
 *
 * A pair in which NEITHER channel carries signal comes back as two empty arrays and a success,
 * not an error.
 *
 * One report plus two ranges:
 * `reports[0].range` is the union that was applied to both channels, while `ranges[0]` and
 * `ranges[1]` are the per-channel scans it was formed from, so a caller can see which channel
 * decided each edge. A channel carrying nothing reports an empty range and contributes nothing
 * to the union.
 */
export interface MasteringRepairTrimSilenceResult {
  /** `[left, right]`, both cut to the one shared range. */
  channels: Float32Array[];
  /** The one shared report: `[report]`. */
  reports: TrimReport[];
  /** The per-channel scans the shared range is the union of: `[leftRange, rightRange]`. */
  ranges: TrimRange[];
}
