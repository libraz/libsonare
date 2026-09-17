/**
 * Defect detection and repair report types: declick, declip, decrackle,
 * denoise, dehum, dereverb and silence trim.
 */

/** One channel's click detection, from {@link masteringRepairDeclickStereo}. Counts runs, not samples. */
export interface ClickDetection {
  /** Runs meeting the repair criteria. */
  count: number;
  /** Runs the criteria excluded as outliers. */
  rejected: number;
  /** Longest counted run, in samples. */
  longestRunSamples: number;
  /** `count` divided by the input duration. */
  perSecond: number;
}

/**
 * What one channel's declick pass found and what it did to it, from
 * {@link masteringRepairDeclickStereo}.
 *
 * A large `detected.rejected` says the configured run length or neighbour
 * ratio is too tight for this material, not that the material is clean.
 */
export interface DeclickReport {
  /** This channel's own analysis of the input. */
  detected: ClickDetection;
  /** Runs interpolated. */
  repairedRuns: number;
  /** Samples overwritten by interpolation. */
  repairedSamples: number;
  /**
   * Of `repairedRuns`, those this channel's own detection did not produce --
   * they were selected because the other channel's detector found them.
   */
  linkedRuns: number;
  /** False when the input was too short for `lpcOrder`, which reduces every fill to linear interpolation. */
  lpcModelUsed: boolean;
}

/**
 * A declicked stereo pair and what each channel's pass did, from
 * {@link masteringRepairDeclickStereo}.
 *
 * A run either channel's detector selects is repaired in both, so a
 * common-mode click never moves the stereo image; only the selection is
 * shared, and each channel's fill comes from its own samples and its own
 * model, which is why `leftReport` and `rightReport` can differ.
 */
export interface DeclickStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclickReport;
  rightReport: DeclickReport;
}

/**
 * One channel's clip detection, from {@link masteringRepairDeclipStereo}.
 *
 * The first four fields count runs and samples at or past `clipThreshold`, so
 * they count the apex of any waveform that reaches it (a full-scale sine
 * reports thousands of "clipped" samples having never been clipped) and they
 * miss material clipped in one tool and attenuated in the next, which leaves
 * nothing at the threshold. The `flat*` fields answer a different question --
 * they find runs of at least 3 consecutive bit-identical samples within 1 dB
 * of the signal's peak, so they survive a gain change and do not fire on a
 * sine, but a genuinely flat-topped waveform (a square or pulse train, a
 * fully limited master) counts as clipped here too and cannot be told apart
 * from real clipping in the time domain.
 */
export interface ClipDetection {
  /** Samples at or past the clip threshold. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  /** Runs of consecutive clipped samples. */
  runCount: number;
  /** A run past the 512-sample cap takes the interpolation fallback instead of the solver. */
  longestRunSamples: number;
  /** Flat-top runs found: at least 3 consecutive bit-identical samples within 1 dB of the peak. */
  flatRunCount: number;
  /** Longest flat-top run, in samples. */
  longestFlatRunSamples: number;
  /** Samples belonging to any counted flat-top run. */
  flatSampleCount: number;
  /** Magnitude the counted flat-top runs sit at; 0 when there are none. */
  flatLevel: number;
}

/**
 * What one channel's declip pass found and what it did to it, from
 * {@link masteringRepairDeclipStereo}.
 */
export interface DeclipReport {
  /** This channel's own analysis of the input. */
  detected: ClipDetection;
  /** Runs the LPC solver filled. */
  lpcReconstructedRuns: number;
  /**
   * Runs past the 512-sample cap, filled by interpolation instead: for these,
   * `lpcOrder`, `iterations` and `lpcBlend` had no effect.
   */
  interpolatedRuns: number;
  /** Samples overwritten by either fill. */
  repairedSamples: number;
  /**
   * Of the repaired runs, those reaching past this channel's own clipped
   * samples because the other channel's run was wider.
   */
  linkedRuns: number;
}

/**
 * A declipped stereo pair and what each channel's pass did, from
 * {@link masteringRepairDeclipStereo}.
 *
 * Each channel reconstructs the whole of every union run it has at least one
 * clipped sample in; a channel with no clipped sample in a run is left
 * untouched there, so `leftReport` and `rightReport` can genuinely differ.
 */
export interface DeclipStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclipReport;
  rightReport: DeclipReport;
}

/**
 * One channel's crackle detection, from {@link masteringRepairDecrackleStereo}.
 *
 * Measured by the median criterion whatever `mode` is configured: wavelet
 * shrinkage removes crackle without ever deciding a sample is crackle, so
 * these counts do not describe what wavelet mode repaired.
 */
export interface CrackleDetection {
  /** Samples deviating from the local median by more than `threshold`. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  perSecond: number;
}

/**
 * What one channel's decrackle pass found and what it did to it, from
 * {@link masteringRepairDecrackleStereo}.
 *
 * The two modes report through different fields: median mode fills
 * `replacedSamples` only, wavelet mode fills `detailCoefficients`,
 * `shrunkCoefficients` and `noiseSigma` only. The field belonging to the
 * other mode reads zero because that mode did not run, which the caller
 * knows from the config it passed rather than from the value.
 */
export interface DecrackleReport {
  /** This channel's own analysis of the input. */
  detected: CrackleDetection;
  /** Median mode: samples the filter overwrote, equal to `detected.sampleCount`. */
  replacedSamples: number;
  /** Wavelet mode: detail coefficients examined by the unshifted pass, not by every pass the mode averages. */
  detailCoefficients: number;
  /** Wavelet mode: of those, driven to zero. */
  shrunkCoefficients: number;
  /** Wavelet mode: the MAD noise estimate that set every level's threshold, which `threshold` only caps. */
  noiseSigma: number;
}

/**
 * A decrackled stereo pair and what each channel's pass did, from
 * {@link masteringRepairDecrackleStereo}.
 *
 * Crackle is surface damage: the two channels carry different scratches at
 * different instants, so each channel is decrackled independently. Unlike
 * declick and declip, no run is ever widened to match the other channel and
 * no report field counts such a widening.
 */
export interface DecrackleStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DecrackleReport;
  rightReport: DecrackleReport;
}

/**
 * What a denoise analysis found in the input, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 *
 * These are absolute levels, which makes them the one part of a stereo
 * denoise report that depends on how many channels were passed: the estimator
 * runs on the channel-summed power, so two identical channels read about 3 dB
 * above the same material through the mono entry point. Compare a stereo floor
 * against another stereo floor, never against a mono one.
 */
export interface NoiseDetection {
  /** Broadband estimated noise floor, in dBFS. */
  floorDbfs: number;
  /**
   * The noise floor's shape, low band to high, length 32. A geometric grid
   * from 20 Hz to Nyquist -- the same axis the mastering report's
   * `bandEnergyDeltaDb` uses, so a noise floor and a tonal-balance change can
   * be read together.
   */
  bandFloorDbfs: number[];
}

/**
 * What a denoise pass found and what it removed, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 */
export interface DenoiseReport {
  /** Analysis of the input, before the mask. */
  detected: NoiseDetection;
  /**
   * Mean attenuation the gain mask applied. Zero reads the same whether the
   * mask was transparent or no mask ran at all.
   */
  meanReductionDb: number;
  /**
   * Deepest attenuation any cell applied. At `reductionDb` the floor set the
   * depth rather than the estimate.
   */
  maxReductionDb: number;
  /**
   * Fraction of cells sitting on that floor. Always 0 in
   * `spectralSubtraction` mode, which floors on `spectralFloor` instead, so 0
   * from that mode is the mode and not a measurement.
   */
  floorLimitedFraction: number;
}

/**
 * A denoised stereo pair and the one mask that produced it, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 *
 * One `report` rather than a per-channel pair: the gain mask is built from the
 * channel-summed power and applied unchanged to both channels, so a pair would
 * be two copies of one measurement and would read as though the two could
 * differ.
 */
export interface DenoiseStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DenoiseReport;
}

/**
 * A denoised channel set and the one mask that produced it, from
 * {@link masteringRepairDenoiseClassicalLinked}.
 *
 * The N-channel form of {@link DenoiseStereoResult}, carrying one `report` for
 * the same reason: the gain mask is built from the channel-summed power and
 * applied unchanged to every channel, so a per-channel pair would be N copies of
 * one measurement.
 *
 * `report.detected` is a measurement of the SET. Its levels are absolute dBFS
 * taken on the summed power, so N identical channels read `10*log10(N)` dB above
 * one of them alone. The attenuation figures on {@link DenoiseReport} are
 * fractions and do not move with the channel count.
 */
export interface DenoiseLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DenoiseReport;
}

/**
 * What a dehum analysis found, from {@link masteringRepairDehumStereo}.
 *
 * Always measured through the estimation path, whatever `adaptive` is
 * configured to: the fixed path notches the configured frequency without
 * ever looking for hum, so a detector following the flag would hand back its
 * own input.
 */
export interface HumDetection {
  /** Tracked fundamental; the configured value when adaptive tracking is off. */
  fundamentalHz: number;
  /**
   * Winning candidate's projected energy over the median candidate. 1.0 means
   * no peak was found at all. Not a lock flag.
   */
  fundamentalProminence: number;
  /** Harmonics above the floor, not necessarily a contiguous run from the first. */
  harmonics: number;
  /**
   * Input level at each k*f0, k ascending, length 16. Measured for every k the
   * sample rate carries, not only the notched ones; a k*f0 at or past Nyquist
   * reads the dB floor because nothing is there to measure.
   */
  harmonicDbfs: number[];
}

/**
 * What one channel's dehum pass found and what it did to it, from
 * {@link masteringRepairDehumStereo}.
 */
export interface DehumReport {
  /** This channel's own analysis, before filtering. */
  detected: HumDetection;
  /** Harmonics the cascade reached; fewer than the configured count once k*f0 hits Nyquist. */
  notchedHarmonics: number;
  /** Frequency the last notch refresh used. */
  appliedFundamentalHz: number;
  /**
   * Largest excursion of the tracked frequency from the configured one. Zero
   * without adaptive tracking, which is the measurement rather than an unset
   * field.
   */
  fundamentalDriftHz: number;
}

/**
 * A dehummed stereo pair and what each channel's pass did, from
 * {@link masteringRepairDehumStereo}.
 *
 * With `adaptive` set, the tracker reads the channel mean and both cascades
 * follow the one frequency it finds, so `leftReport` and `rightReport` share
 * the same `appliedFundamentalHz` and `fundamentalDriftHz` by construction;
 * only the frequency is shared, so each report's `detected` still measures
 * that channel's own input. Without `adaptive`, which is the default,
 * nothing is shared and the two channels are filtered independently.
 */
export interface DehumStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DehumReport;
  rightReport: DehumReport;
}

/**
 * What a dereverb analysis found in the input, from
 * {@link masteringRepairDereverbClassicalStereo}.
 *
 * NOT an ISO 3382 reverberation time: no Schroeder integration, no
 * noise-floor truncation, STFT bins rather than octave bands, and music is not
 * a free decay. Use {@link estimateRoom} for a graded RT60; this reports what
 * the module itself measured while deciding how much to subtract.
 */
export interface ReverbDetection {
  /**
   * Decay across the module's own late lag, in dB. Less negative means the
   * material sustains across that lag, which a late tail does and a dry offset
   * does not -- so a reverberant input reads *higher* here than the same
   * material dry, which is the opposite of what the name suggests.
   */
  lateDecayRatioDb: number;
  /**
   * Mean WPE predictor norm before the clamp. Zero whenever the WPE stage did
   * not run, which is the case unless `wpeEnabled` is set -- and it is clear
   * by default, so a default-config pass reports 0 here as its measurement.
   */
  latePredictability: number;
}

/**
 * What a dereverb pass found and what it removed, from
 * {@link masteringRepairDereverbClassicalStereo}.
 */
export interface DereverbReport {
  /** Analysis of the input. */
  detected: ReverbDetection;
  /** Mean attenuation the subtraction applied. */
  meanReductionDb: number;
  /**
   * Fraction of cells the `threshold` gate admitted as late reverberation. The
   * only observation of that knob: 0 alongside a nonzero `meanReductionDb`
   * says the gate admitted nothing.
   */
  suppressedFraction: number;
  /**
   * Mean predictor norm actually applied, after the clamp. Below
   * `detected.latePredictability` says the clamp acted, an otherwise silent
   * branch. Zero when the WPE stage did not run, so 0 by default.
   */
  wpePredictorNorm: number;
}

/**
 * A dereverberated stereo pair and the one mask that produced it, from
 * {@link masteringRepairDereverbClassicalStereo}.
 *
 * One `report` rather than a per-channel pair: the mask is built from the
 * channel-summed power and the WPE stage accumulates over both channels, so a
 * pair would be two copies of one measurement. Every field of that report is a
 * ratio or a fraction, so unlike {@link NoiseDetection} nothing here shifts
 * with the channel count and a stereo figure is comparable against a mono one.
 */
export interface DereverbStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DereverbReport;
}

/**
 * A dereverberated channel set and the one mask that produced it, from
 * {@link masteringRepairDereverbClassicalLinked}.
 *
 * The N-channel form of {@link DereverbStereoResult}: one mask over the
 * channel-summed power and one WPE predictor set fitted over every channel's
 * statistics, so a per-channel report pair would be N copies of one measurement.
 *
 * Every field of that report is a ratio or a fraction, so unlike
 * {@link DenoiseLinkedResult} nothing here shifts with the channel count and a
 * figure measured over a set is comparable against a mono one.
 */
export interface DereverbLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DereverbReport;
}

/**
 * One half-open sample range, in INPUT-buffer coordinates, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * The coordinates are the input's, so `lastExclusive - first` is the number of
 * samples the range covers and the returned channels are that long -- not the
 * length they are indexed by.
 */
export interface TrimRange {
  /** First kept sample. */
  first: number;
  /** One past the last kept sample. */
  lastExclusive: number;
}

/**
 * What a trim pass kept and what it dropped, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * A pass that kept nothing reports the range `(length, length)`, which counts
 * the whole buffer as removed head and leaves removed tail at 0. The two still
 * sum to the input length, so a caller reporting how much went reads the right
 * total; only the split between the ends is arbitrary there.
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
 * A trimmed stereo pair, the range both channels were cut to, and the two
 * per-channel ranges that range is the union of, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * Unlike every other repair stereo result, `left` and `right` are SHORTER than
 * the input -- trimming is the point -- and they are empty when neither channel
 * carried signal, which is a success rather than an error. There is no separate
 * length field: `left.length` is the output length.
 *
 * `report.range` is the union that was applied to both channels. `leftRange`
 * and `rightRange` are the per-channel scans it was formed from, so a caller
 * can see which channel decided each edge; a channel carrying nothing reports
 * an empty range and contributes nothing to the union.
 */
export interface TrimSilenceStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: TrimReport;
  leftRange: TrimRange;
  rightRange: TrimRange;
}
