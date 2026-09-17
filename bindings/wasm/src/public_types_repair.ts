/**
 * Defect detection and repair report types: declick, declip, decrackle,
 * denoise, dehum, dereverb and silence trim.
 */

/**
 * What a declick analysis found in one channel of a
 * {@link MasteringRepairDeclickStereoResult}. Runs, not samples.
 */
export interface ClickDetection {
  /** Runs meeting the repair criteria. */
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
 * {@link MasteringRepairDeclickStereoResult} and what it did to it.
 */
export interface DeclickReport {
  /** This channel's own analysis of the input. */
  detected: ClickDetection;
  /**
   * Runs interpolated. Larger than `detected.count` only under linked stereo
   * detection.
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

/**
 * A declicked stereo pair and what each channel's pass found and did.
 *
 * A run either channel's detector selects is repaired in BOTH channels — a
 * common-mode click repaired on one side only would move the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own AR model, which is why `leftReport` and `rightReport`
 * genuinely differ. `linkedRuns` is the part of `repairedRuns` this channel's
 * own detection did not produce, so it is normally non-zero here. Merged runs
 * can exceed `maxClickSamples`: that cap governs what may be selected, not
 * how far a selection reaches once both channels agree a click is there.
 */
export interface MasteringRepairDeclickStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclickReport;
  rightReport: DeclickReport;
}

/**
 * What a declip analysis found in one channel of a
 * {@link MasteringRepairDeclipStereoResult}.
 */
export interface ClipDetection {
  /** Samples at or past `clipThreshold`. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  /** Runs of consecutive clipped samples. */
  runCount: number;
  /** A run past the 512-sample cap takes the interpolation fallback instead of the solver. */
  longestRunSamples: number;
  /**
   * Runs of at least 3 bit-identical samples sitting within 1 dB of the signal's peak. Reads
   * clipping that survived a later gain change and so no longer reaches `clipThreshold` — the
   * fields above see none of it. A genuinely flat-topped waveform (a square or pulse train, a
   * fully limited master) counts here too and cannot be told apart from clipping in the time
   * domain.
   */
  flatRunCount: number;
  /** Over the counted flat-top runs. */
  longestFlatRunSamples: number;
  /** Samples belonging to a counted flat-top run. */
  flatSampleCount: number;
  /** The magnitude the counted runs sit at; 0 when there are none. */
  flatLevel: number;
}

/**
 * What a declip pass found in one channel of a
 * {@link MasteringRepairDeclipStereoResult} and what it did to it.
 */
export interface DeclipReport {
  /** This channel's own analysis of the input. */
  detected: ClipDetection;
  /** Runs the Janssen solver filled. */
  lpcReconstructedRuns: number;
  /**
   * Runs past the LPC gap cap, filled by interpolation instead: for these,
   * `lpcOrder`, `iterations` and `lpcBlend` had no effect.
   */
  interpolatedRuns: number;
  /** Samples overwritten by either fill. */
  repairedSamples: number;
  /**
   * Of the repaired runs, those reaching past this channel's own clipped
   * samples because the other channel's run was wider. Always 0 from the
   * mono `masteringRepairDeclip`.
   */
  linkedRuns: number;
}

/**
 * A declipped stereo pair and what each channel's pass found and did.
 *
 * Declip takes the union of both channels' clipped runs. Each channel
 * reconstructs the whole of every union run it has at least one clipped
 * sample in; a channel with none is left untouched there. `linkedRuns` is
 * therefore 0 for a plateau clipped in only one channel, and non-zero only
 * where both channels are clipped in the same region with different
 * extents — the narrower channel is what reaches past its own clipped
 * samples.
 */
export interface MasteringRepairDeclipStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclipReport;
  rightReport: DeclipReport;
}

/**
 * What a decrackle analysis found in one channel of a
 * {@link MasteringRepairDecrackleStereoResult}.
 *
 * Crackle is measured by the median criterion regardless of the configured `DecrackleMode` --
 * wavelet shrinkage is a removal method, not a detection method, so this is the module's only
 * definition of the defect.
 */
export interface CrackleDetection {
  /** Samples deviating from the local median by more than `threshold`. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  /** `sampleCount` divided by the input duration. */
  perSecond: number;
}

/**
 * What a decrackle pass found in one channel of a
 * {@link MasteringRepairDecrackleStereoResult} and what it did to it.
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

/**
 * A decrackled stereo pair and what each channel's pass found and did.
 *
 * Crackle is surface damage: the two channels carry different scratches at different instants,
 * so there is no common event for a shared decision to agree about. Both modes are memoryless
 * across channels, so the pair is processed independently and there is no `linkedRuns` field,
 * unlike {@link MasteringRepairDeclickStereoResult} and {@link MasteringRepairDeclipStereoResult}.
 */
export interface MasteringRepairDecrackleStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DecrackleReport;
  rightReport: DecrackleReport;
}

/**
 * What a dehum analysis found in one channel of a
 * {@link MasteringRepairDehumStereoResult}.
 *
 * Always measured through the estimation path, whatever `DehumOptions.adaptive` says: the
 * fixed path notches the configured frequency without ever looking for hum, so a detector
 * following the flag would hand back its own input.
 */
export interface HumDetection {
  /** Tracked fundamental; the configured value when adaptive tracking is off. */
  fundamentalHz: number;
  /** Winning candidate's projected energy over the median candidate; 1.0 means no peak
   * was found at all. Not a lock flag. */
  fundamentalProminence: number;
  /** Harmonics found above the floor, not necessarily a contiguous run from the first. */
  harmonics: number;
  /** Input level at each k*f0, k ascending, length 16. Measured for every k the sample
   * rate carries, not only the notched ones; a k*f0 at or past Nyquist reads the dB floor
   * because nothing is there to measure. */
  harmonicDbfs: Float32Array;
}

/**
 * What a dehum pass found in one channel of a
 * {@link MasteringRepairDehumStereoResult} and what it did to it.
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
export interface MasteringRepairDehumStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DehumReport;
  rightReport: DehumReport;
}

/**
 * What a denoise analysis found in a
 * {@link MasteringRepairDenoiseClassicalStereoResult}.
 *
 * A pair-level measurement, and the only absolute one in that result: the estimator runs on
 * the channel-summed power, so two identical channels read `10*log10(2)` -- about
 * 3.01 dB -- above the same material through `masteringRepairDenoiseClassical`. Compare a stereo
 * floor only against another stereo floor.
 */
export interface NoiseDetection {
  /** Broadband estimated noise floor, in dBFS. */
  floorDbfs: number;
  /** The floor's shape, low band to high, length 32. A geometric grid from 20 Hz to Nyquist --
   * the same axis the mastering report's `bandEnergyDeltaDb` uses, so a noise floor and a
   * tonal-balance change can be read together. */
  bandFloorDbfs: Float32Array;
}

/**
 * What a denoise pass found in a
 * {@link MasteringRepairDenoiseClassicalStereoResult} and what it removed.
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

/**
 * A denoised stereo pair and the one mask that produced it.
 *
 * One `report` rather than a per-channel pair: the gain mask is built from the channel-summed
 * power and applied unchanged to both channels, so the pass cannot move an interchannel level
 * or phase difference, and a pair would be two copies of one measurement.
 *
 * Needs at least `nFft` samples and REJECTS a shorter input, the opposite of
 * {@link MasteringRepairDereverbClassicalStereoResult}'s entry point, which pads one.
 */
export interface MasteringRepairDenoiseClassicalStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DenoiseReport;
}

/**
 * A denoised channel set and the one mask that produced it.
 *
 * The N-channel form of {@link MasteringRepairDenoiseClassicalStereoResult}: one mask over the
 * channel-summed power, applied unchanged to every channel, so no interchannel level or phase
 * difference moves however many channels there are. One channel reproduces
 * `masteringRepairDenoiseClassical` bit for bit; two reproduce the stereo entry plane for plane.
 *
 * `report.detected` is the SET's and absolute: N identical channels read `10*log10(N)` above one
 * of them — about 3.01 dB for a pair and 4.77 dB for three. Every other field of the report is a
 * fraction and does not move with the channel count.
 */
export interface MasteringRepairDenoiseClassicalLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DenoiseReport;
}

/**
 * What a dereverb analysis found in a
 * {@link MasteringRepairDereverbClassicalStereoResult}.
 *
 * NOT an ISO 3382 reverberation time: no Schroeder integration, no noise-floor truncation,
 * STFT bins rather than octave bands, and music is not a free decay. Use `estimateRoom` for a
 * graded RT60; this reports what the module itself measured while deciding how much to
 * subtract.
 */
export interface ReverbDetection {
  /** Decay across the module's own late lag, in dB. Less negative means the material sustains
   * across that lag, which a late tail does and a dry offset does not -- so a reverberant
   * input reads HIGHER here than the same material dry. */
  lateDecayRatioDb: number;
  /** Mean WPE predictor norm, before the clamp. Exactly 0 whenever the WPE stage did not run,
   * which is the case unless `wpeEnabled` is set -- and it is clear by default, so a
   * default-config pass reports 0 here as its measurement. */
  latePredictability: number;
}

/**
 * What a dereverb pass found in a
 * {@link MasteringRepairDereverbClassicalStereoResult} and what it removed.
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

/**
 * A dereverberated stereo pair and the one mask that produced it.
 *
 * One `report` rather than a per-channel pair: the mask is built from the channel-summed power
 * and the WPE stage accumulates over both channels and applies one predictor set to each, so
 * neither stage can move an interchannel level or phase difference.
 *
 * Every field of that report is a ratio or a fraction, so unlike {@link NoiseDetection}
 * nothing here shifts with the channel count and a stereo figure is comparable against a mono
 * one. An input shorter than `nFft` is PADDED for analysis rather than rejected, which is the
 * opposite of {@link MasteringRepairDenoiseClassicalStereoResult}'s entry point.
 */
export interface MasteringRepairDereverbClassicalStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DereverbReport;
}

/**
 * A dereverberated channel set and the one mask that produced it.
 *
 * The N-channel form of {@link MasteringRepairDereverbClassicalStereoResult}: one mask over the
 * channel-summed power, and one WPE predictor set fitted over every channel's statistics, so
 * neither stage can move an interchannel level or phase difference. One channel reproduces
 * `masteringRepairDereverbClassical` bit for bit; two reproduce the stereo entry plane for plane.
 *
 * Every field of the report is a ratio or a fraction, so unlike
 * {@link MasteringRepairDenoiseClassicalLinkedResult} nothing here shifts with the channel count.
 * An input shorter than `nFft` is PADDED for analysis rather than rejected, again the opposite of
 * that entry.
 */
export interface MasteringRepairDereverbClassicalLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DereverbReport;
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
 * A trimmed stereo pair, the range both channels were cut to, and the two per-channel scans
 * that range is the union of.
 *
 * The only repair stereo result whose arrays are SHORTER than the input, so `left.length` is
 * the output length and the input's says nothing about it. Both channels come back the same
 * length, because one range cuts both.
 *
 * A pair in which NEITHER channel carries signal comes back as two empty arrays and a success,
 * not an error.
 *
 * One `report` plus two ranges, which is neither of the earlier repair stereo shapes:
 * `report.range` is the union that was applied to both channels, while `leftRange` and
 * `rightRange` are the per-channel scans it was formed from, so a caller can see which channel
 * decided each edge. A channel carrying nothing reports an empty range and contributes nothing
 * to the union.
 */
export interface MasteringRepairTrimSilenceStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: TrimReport;
  leftRange: TrimRange;
  rightRange: TrimRange;
}
