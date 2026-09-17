export interface MasteringOptions {
  /**
   * Integrated-loudness target in LUFS. Default -14. Must be finite; a
   * non-finite value is rejected.
   *
   * Deliberately NOT the contract of {@link EngineBounceOptions.targetLufs},
   * which shares this name, unit and default but resolves a non-finite value to
   * the default. There the loudness stage derives a per-sample gain and checks
   * nothing itself; here the target reaches a validator that owns the domain.
   */
  targetLufs?: number;
  /** True-peak ceiling in dBTP. Default -1. Must be finite. */
  ceilingDb?: number;
  /** True-peak oversampling factor. Default 4. */
  truePeakOversample?: number;
  /**
   * Post true-peak limiter release in ms. Default 0 => library default (50 ms).
   * Any other value is used as given; a negative or non-finite one is rejected.
   */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

/**
 * Built-in mastering presets. The restoration presets — `vinyl`, `tapeHiss`,
 * `fieldRecording`, `voiceMemo` and `shellac78` — enable repair stages only and
 * leave level alone. Every other preset carries an integrated-loudness target
 * and a true-peak ceiling, which are not equally binding: the ceiling always
 * holds, while the loudness target is what one normalization pass aims at.
 *
 * Reaching a target above the input's loudness costs gain the input's peak
 * headroom may not have, so the stage drives its true-peak limiter up to
 * `loudness.maxLimiterGainReductionDb` (12 dB by default) to close the distance
 * and stops there. Material needing more limiting than that — and material where
 * the limiter's own gain reduction takes back part of the applied gain, which a
 * single pass does not re-measure — finishes below target with
 * `loudnessTargetLimited` set on the result. Peak-normalized input, whose
 * headroom is ~0 dB, is the common case for both. Read `outputLufs` for what was
 * achieved rather than assuming the preset's target.
 */
export type MasteringPreset =
  | 'pop'
  | 'edm'
  | 'acoustic'
  | 'hipHop'
  | 'aiMusic'
  | 'speech'
  | 'streaming'
  | 'youtube'
  | 'broadcast'
  | 'podcast'
  | 'audiobook'
  | 'cinema'
  | 'jpop'
  | 'ambient'
  | 'lofi'
  | 'classical'
  | 'drumAndBass'
  | 'techno'
  | 'metal'
  | 'trap'
  | 'rnb'
  | 'jazz'
  | 'kpop'
  | 'trance'
  | 'gameOst'
  | 'vinyl'
  | 'tapeHiss'
  | 'fieldRecording'
  | 'voiceMemo'
  | 'shellac78';

/**
 * One delivery target `masteringStreamingPreview` reports a normalization gain
 * for.
 *
 * Both numbers are required and must be finite. Unlike
 * {@link EngineBounceOptions.targetLufs}, which shares this name, unit and
 * default, a non-finite value here selects nothing — this interface has no
 * "use the library default" spelling — so it is refused by name.
 */
export interface StreamingPlatform {
  /** Platform name, echoed into the reported result. */
  name: string;
  /** Integrated-loudness target in LUFS, e.g. -14. */
  targetLufs: number;
  /** True-peak ceiling in dBTP, e.g. -1. */
  ceilingDb: number;
}

// BEGIN GENERATED SoloProcessor (make processor-types)
/**
 * Every processor name `masteringProcessorNames()` can return, and therefore
 * every name `masteringProcess` / `masteringProcessStereo` accept.
 */
export const SOLO_PROCESSORS = [
  'dynamics.brickwallLimiter',
  'dynamics.compressor',
  'dynamics.deesser',
  'dynamics.duckingProcessor',
  'dynamics.expander',
  'dynamics.gate',
  'dynamics.limiter',
  'dynamics.parallelComp',
  'dynamics.sidechainRouter',
  'dynamics.transientShaper',
  'dynamics.upwardCompressor',
  'dynamics.upwardExpander',
  'dynamics.vocalRider',
  'effects.acoustic.roomMorph',
  'effects.delay.stereo',
  'effects.modulation.autoWah',
  'effects.modulation.chorus',
  'effects.modulation.ensemble',
  'effects.modulation.flanger',
  'effects.modulation.phaser',
  'effects.modulation.pitchShifter',
  'effects.modulation.ringModulator',
  'effects.modulation.rotary',
  'effects.modulation.wah',
  'effects.reverb.convolution',
  'effects.reverb.dattorro',
  'effects.reverb.fdn',
  'effects.reverb.plate',
  'effects.reverb.room',
  'effects.reverb.velvet',
  'eq.apiStyle',
  'eq.bandPass',
  'eq.cutFilter',
  'eq.dynamic',
  'eq.equalizer',
  'eq.graphic',
  'eq.linearPhase',
  'eq.midSide',
  'eq.minimumPhase',
  'eq.parametric',
  'eq.pultec',
  'eq.shelving',
  'eq.tilt',
  'final.bitDepth',
  'final.dither',
  'final.outputChain',
  'maximizer.adaptiveRelease',
  'maximizer.loudnessOptimize',
  'maximizer.maximizer',
  'maximizer.softKneeMax',
  'maximizer.truePeakLimiter',
  'multiband.compressor',
  'multiband.dynamicEq',
  'multiband.expander',
  'multiband.imager',
  'multiband.limiter',
  'multiband.saturation',
  'repair.declick',
  'repair.declip',
  'repair.decrackle',
  'repair.dehum',
  'repair.denoiseClassical',
  'repair.dereverbClassical',
  'repair.trimSilence',
  'saturation.ampSim',
  'saturation.bitcrusher',
  'saturation.exciter',
  'saturation.hardClipper',
  'saturation.multibandExciter',
  'saturation.softClipper',
  'saturation.tape',
  'saturation.transformer',
  'saturation.tube',
  'saturation.waveshaper',
  'spectral.airBand',
  'spectral.lowEndFocus',
  'spectral.presenceEnhancer',
  'spectral.spectralShaper',
  'stereo.autoPan',
  'stereo.haasEnhancer',
  'stereo.imager',
  'stereo.monoMaker',
  'stereo.phaseAlign',
  'stereo.stereoBalance',
] as const;

export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];
// END GENERATED SoloProcessor

export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

export interface MasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited?: boolean;
  latencySamples?: number;
  /**
   * Samples the named processor replaced with a finite in-domain one,
   * keeping the output finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before the
   * processor runs, so a replacement is always of a value the processor
   * itself produced.
   *
   * Whether this can be non-zero depends on which processor was named: one
   * that does not substitute reports zero because it has nothing to replace
   * with, not because nothing needed replacing.
   *
   * @example
   * ```ts
   * const result = masteringProcess('maximizer.truePeakLimiter', samples, 44100);
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
}

export interface MasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * See {@link MasteringResult.nonFiniteSubstitutionCount}. Summed over both
   * channels.
   */
  nonFiniteSubstitutionCount: number;
}

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

/** What gain-matching one take to another's loudness took, and produced. */
export interface LoudnessMatchResult {
  /** The source, gain-matched to the reference's integrated loudness. */
  samples: Float32Array;
  sampleRate: number;
  /**
   * The reference's BS.1770 integrated loudness. Non-finite for a silent or
   * below-gate take, which is also when `appliedGainDb` is 0.
   */
  referenceLufs: number;
  /** The source's, before the gain. Same non-finite case. */
  sourceLufs: number;
  /** Gain applied to the source, in dB. */
  appliedGainDb: number;
  /**
   * The matched take's true peak after the gain, in dBTP. It can sit above
   * 0 dBTP: the gain is applied with no upper bound, because clamping for
   * headroom would return the source at its own loudness whenever it started
   * near full scale, which is the one thing a loudness match must not do.
   * Limiting is the caller's decision, so a value above 0 is a report rather
   * than a defect.
   */
  matchedTruePeakDbtp: number;
}

/** Generic traversal view used by chain-config tooling; public configs are fully typed below. */
export type MasteringChainSection = Record<string, unknown>;

/**
 * Nested mastering-chain configuration. Top-level keys are the processing
 * modules; nest processor and parameter names beneath them, e.g.
 *
 * ```ts
 * masteringChain(samples, sr, {
 *   dynamics: { compressor: { thresholdDb: -24 } },
 *   loudness: { targetLufs: -14 },
 * });
 * ```
 *
 * A boolean toggles a module/processor's `enabled` flag; setting any field
 * implicitly enables its module unless `enabled: false` is also given. Unknown
 * keys throw at apply time. (`stereo.*` modules apply on the stereo path only.)
 *
 * Exception — color stages as `masterAudio` overrides: the `saturation.tape`
 * and `saturation.exciter` stages are engaged from an override only when you
 * pass `enabled: true` explicitly. On a preset where they are off, adjusting a
 * parameter alone (e.g. `saturation: { tape: { driveDb: 6 } }`) has no audible
 * effect; use `saturation: { tape: { enabled: true, driveDb: 6 } }`.
 */
export interface MasteringChainConfig {
  repair?: {
    /** `boolean` is retained as a deprecated shorthand for `{ enabled }`. */
    denoise?:
      | boolean
      | {
          enabled?: boolean;
          /** 0 = log-MMSE, 1 = MMSE-STSA, 2 = spectral subtraction. */
          mode?: number;
          /** 0 = quantile, 1 = MCRA, 2 = IMCRA, 3 = speech-presence probability. */
          noiseEstimator?: number;
          nFft?: number;
          hopLength?: number;
          ddAlpha?: number;
          reductionDb?: number;
          /** @deprecated Use `reductionDb`; converted to it (dB = -20*log10(gainFloor)). */
          gainFloor?: number;
          overSubtraction?: number;
          spectralFloor?: number;
          noiseEstimationQuantile?: number;
          speechPresenceGain?: boolean;
          gainSmoothing?: boolean;
        };
    nFft?: number;
    hopLength?: number;
    ddAlpha?: number;
    reductionDb?: number;
    /** @deprecated Use `denoise.reductionDb`; converted to it (dB = -20*log10(gainFloor)). */
    gainFloor?: number;
    declip?: {
      enabled?: boolean;
      clipThreshold?: number;
      lpcOrder?: number;
      iterations?: number;
      lpcBlend?: number;
    };
    decrackle?: {
      enabled?: boolean;
      threshold?: number;
      /** 0 = median, 1 = wavelet shrinkage. */
      mode?: number;
      levels?: number;
    };
    dehum?: {
      enabled?: boolean;
      fundamentalHz?: number;
      harmonics?: number;
      q?: number;
      adaptive?: boolean;
      searchRangeHz?: number;
      adaptation?: number;
      frameSize?: number;
      pllBandwidth?: number;
      /** 0 = subtract the tracked harmonics, 1 = cascaded notches. */
      mode?: number;
    };
    declick?: {
      enabled?: boolean;
      threshold?: number;
      neighborRatio?: number;
      maxClickSamples?: number;
      lpcOrder?: number;
      residualRatio?: number;
    };
    dereverb?: {
      enabled?: boolean;
      threshold?: number;
      attenuation?: number;
      nFft?: number;
      hopLength?: number;
      t60Sec?: number;
      lateDelayMs?: number;
      overSubtraction?: number;
      spectralFloor?: number;
      wpeEnabled?: boolean;
      wpeIterations?: number;
      wpeTaps?: number;
      wpeStrength?: number;
    };
  };
  eq?: {
    tilt?: {
      enabled?: boolean;
      tiltDb?: number;
      pivotHz?: number;
    };
    /** @deprecated Use `eq.tilt.tiltDb`. */
    tiltDb?: number;
    /** @deprecated Use `eq.tilt.pivotHz`. */
    pivotHz?: number;
  };
  dynamics?: {
    compressor?: {
      enabled?: boolean;
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      kneeDb?: number;
      makeupGainDb?: number;
      autoMakeup?: boolean;
    };
    deesser?: {
      enabled?: boolean;
      frequencyHz?: number;
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      rangeDb?: number;
      bandpassQ?: number;
    };
    transientShaper?: {
      enabled?: boolean;
      attackGainDb?: number;
      sustainGainDb?: number;
      fastAttackMs?: number;
      fastReleaseMs?: number;
      slowAttackMs?: number;
      slowReleaseMs?: number;
      sensitivity?: number;
      maxGainDb?: number;
      gainSmoothingMs?: number;
      lookaheadMs?: number;
    };
    multibandComp?: {
      enabled?: boolean;
      lowCutoffHz?: number;
      highCutoffHz?: number;
      lowThresholdDb?: number;
      lowRatio?: number;
      lowAttackMs?: number;
      lowReleaseMs?: number;
      midThresholdDb?: number;
      midRatio?: number;
      midAttackMs?: number;
      midReleaseMs?: number;
      highThresholdDb?: number;
      highRatio?: number;
      highAttackMs?: number;
      highReleaseMs?: number;
    };
  };
  saturation?: {
    tape?: {
      enabled?: boolean;
      driveDb?: number;
      saturation?: number;
      hysteresis?: number;
      outputGainDb?: number;
      speedIps?: number;
      headBumpDb?: number;
      bias?: number;
      gapLoss?: number;
      /** Jiles-Atherton core oversampling: 1 (default), 2, or 4. */
      oversampleFactor?: number;
    };
    exciter?: {
      enabled?: boolean;
      frequencyHz?: number;
      driveDb?: number;
      amount?: number;
      q?: number;
      evenOddMix?: number;
      /**
       * Antialiasing mode ordinal: 0 = none, 3 = 4x oversampling. The ADAA
       * modes (1, 2) name a member the exciter does not implement and are
       * rejected; an ordinal outside 0-3 is rejected as out of range.
       */
      aliasing?: number;
    };
  };
  spectral?: {
    airBand?: {
      enabled?: boolean;
      amount?: number;
      shelfFrequencyHz?: number;
      dynamicThresholdDb?: number;
      dynamicRangeDb?: number;
    };
  };
  stereo?: {
    imager?: {
      enabled?: boolean;
      width?: number;
      outputGainDb?: number;
      decorrelationAmount?: number;
      preserveEnergy?: boolean;
    };
    monoMaker?: {
      enabled?: boolean;
      amount?: number;
      frequencyHz?: number;
    };
  };
  maximizer?: {
    truePeakLimiter?: {
      enabled?: boolean;
      ceilingDb?: number;
      lookaheadMs?: number;
      releaseMs?: number;
      oversampleFactor?: number;
      applyGainAtInputRate?: boolean;
    };
  };
  loudness?: {
    enabled?: boolean;
    targetLufs?: number;
    ceilingDb?: number;
    truePeakOversample?: number;
    releaseMs?: number;
    applyGainAtInputRate?: boolean;
    /**
     * How deep, in dB, the stage may drive its post-gain true-peak limiter to
     * reach {@link targetLufs}. Default 12. The static normalization gain may
     * exceed the peak headroom toward `ceilingDb` by this much; `0` restores a
     * strict headroom clamp, under which peak-normalized input keeps its input
     * loudness whatever target is asked for. `ceilingDb` holds at every setting.
     */
    maxLimiterGainReductionDb?: number;
  };
  /**
   * Dot-notation spelling of any leaf above, e.g. `'loudness.targetLufs': -20`
   * beside or instead of `loudness: { targetLufs: -20 }`. It is the form the C
   * ABI carries parameters in, so a caller assembling overrides dynamically can
   * emit it directly; the core validates the key and rejects an unknown one.
   * The nested spelling is canonical — prefer it in hand-written code, where it
   * is checked field by field while a dotted key is only checked at run time.
   */
  [flatKey: `${string}.${string}`]: number | boolean | undefined;
}

/** Gain reduction reported by a single dynamics/maximizer chain stage. */
export interface StageGainReduction {
  /** Stage identifier, e.g. `"dynamics.compressor"`. */
  stage: string;
  /**
   * Most recent (typically last-block) gain reduction in dB (negative or
   * zero); for multiband stages it is the most-reduced band.
   */
  gainReductionDb: number;
}

/** Existing EBU R128 measurements captured before or after mastering. */
export interface MasteringLoudnessSummary {
  integratedLufs: number;
  maxMomentaryLufs: number;
  maxShortTermLufs: number;
  truePeakDbtp: number;
  loudnessRange: number;
}

/** Compact explanation of how an offline mastering chain changed a program. */
export interface MasteringReport {
  before: MasteringLoudnessSummary;
  after: MasteringLoudnessSummary;
  appliedGainDb: number;
  /** Most-negative final dynamics/limiter gain reduction, or zero when none ran. */
  maxGainReductionDb: number;
  loudnessTargetLimited: boolean;
  /** 32 logarithmically-spaced after-minus-before spectral energy deltas (dB). */
  bandEnergyDeltaDb: Float32Array;
}

export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
  /**
   * ITU-R BS.1770-4 true peak of the output (dBTP). Lets callers verify a
   * preset ceiling was met without a second oversampled scan.
   *
   * The oversample factor follows the peak-limiting stage the chain actually
   * applied, so it is not fixed: the loudness stage's true-peak oversample
   * (default 4x) when loudness is enabled, and the maximizer true-peak
   * limiter's own oversample factor when loudness is disabled but that stage
   * ran. The two disagree by roughly 0.02 dB between 4x and 8x, so comparing
   * this against an independently measured peak needs the same factor.
   */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 Loudness Range of the output (LU). */
  outputLra: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * Samples a stage replaced with a finite in-domain one, keeping the
   * output finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before any stage
   * runs, so a replacement is always of a value a stage itself produced.
   *
   * Only the true-peak limiters replace anything, so with the maximizer's
   * limiter and the loudness stage both disabled a zero here means no stage
   * was able to replace anything rather than that nothing needed replacing.
   *
   * @example
   * ```ts
   * const result = masterAudio(samples, 44100, 'pop');
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
  /** Per-stage gain reductions for the dynamics/maximizer stages (a subset of `stages`). */
  stageGainReductions: StageGainReduction[];
  report: MasteringReport;
}

export interface MasteringChainStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
  /** See {@link MasteringChainResult} for field semantics. */
  outputTruePeakDbtp: number;
  outputLra: number;
  loudnessTargetLimited: boolean;
  /** See {@link MasteringChainResult} for field semantics. Aggregated over both channels. */
  nonFiniteSubstitutionCount: number;
  stageGainReductions: StageGainReduction[];
  report: MasteringReport;
}
