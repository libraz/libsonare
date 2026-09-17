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

export interface StreamingPlatform {
  name: string;
  targetLufs: number;
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

/** Options for `mastering`. All fields are optional. */
export interface MasteringOptions {
  /** Target integrated LUFS. Default -14. */
  targetLufs?: number;
  /** True/sample peak ceiling in dBFS. Default -1. */
  ceilingDb?: number;
  /** Oversampling factor used for peak estimation. Default 4. */
  truePeakOversample?: number;
  /** Post true-peak limiter release in ms. Default 0 => library default (50 ms). */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

/** What gain-matching one take to another's loudness took, and produced. */
export interface LoudnessMatchResult {
  /** The source take at the reference's loudness. */
  samples: Float32Array;
  sampleRate: number;
  /**
   * The reference take's BS.1770 integrated loudness. Non-finite for a silent
   * or below-gate take — the documented outcome, not a failure.
   */
  referenceLufs: number;
  /** The matched take's, before the gain. Non-finite in the same case. */
  sourceLufs: number;
  /**
   * `referenceLufs - sourceLufs`. Applied with no upper bound, and 0 whenever
   * either loudness is non-finite.
   */
  appliedGainDb: number;
  /**
   * The matched take's true peak after the gain, in dBTP.
   *
   * The match does not cap the gain, so this can sit above 0 dBTP: clamping to
   * headroom would leave a near-full-scale source at its own loudness, which is
   * the one thing a loudness match must not do. Limit downstream if the peak
   * matters more than the match.
   */
  matchedTruePeakDbtp: number;
}

/**
 * Mastering loudness/true-peak processing result
 */
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
   * with, not because nothing needed replacing. The counter saturates rather
   * than wrapping, because a wrapped total could read as the one value zero
   * is reserved for.
   *
   * @example
   * ```ts
   * const result = masteringProcess({
   *   processorName: 'maximizer.truePeakLimiter',
   *   samples,
   *   sampleRate,
   * });
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // `result.samples` is not derived from `samples` everywhere
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
}

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

export type MasteringProcessorParams = Record<string, number | boolean>;

/**
 * Params accepted by the assistant entry points. Every key is numeric except
 * `targetPlatform`, which is a delivery-target NAME (`'broadcast'`, `'podcast'`,
 * `'club'`, ...). A number is rejected for it: the numeric index the C ABI
 * carries is a transport detail for callers that cannot pass a string, not part
 * of the JavaScript vocabulary.
 */
export type MasteringAssistantParams = Record<string, number | boolean | string>;

/**
 * Nested mastering-chain configuration. A boolean toggles a module/processor's
 * `enabled` flag; setting any field implicitly enables its module unless
 * `enabled: false` is also given.
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
    /** Canonical nested tilt stage. */
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

/**
 * Configuration for the block-by-block {@link StreamingMasteringChain}.
 *
 * Extends {@link MasteringChainConfig} with optional precomputed loudness
 * parameters. The streaming chain cannot measure whole-signal integrated LUFS,
 * so an enabled `loudness` stage normally throws at construction. To let a
 * preset's streaming preview match its offline render, the caller may
 * precompute the loudness normalization gain offline (e.g.
 * `targetLufs - measuredIntegratedLufs`) and supply it here.
 */
export interface StreamingMasteringChainConfig extends MasteringChainConfig {
  /**
   * Precomputed static loudness gain in dB. When omitted (the default), an
   * enabled `loudness` stage still throws. When provided and `loudness.enabled`
   * is set, the chain applies this fixed gain per block before the loudness
   * stage's true-peak limiter instead of throwing.
   */
  loudnessStaticGainDb?: number;

  /**
   * Offline-measured true-peak (dBFS) of the source the static gain was
   * computed for. When provided, the static gain is clamped to
   * `(loudness.ceilingDb - loudnessStaticGainPeakDb) +
   * max(loudness.maxLimiterGainReductionDb, 0)` so the streaming preview
   * does not drive the loudness limiter harder than the offline chain. When
   * omitted (the default) the static gain is applied verbatim.
   */
  loudnessStaticGainPeakDb?: number;
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
   * Aggregated, so it does not identify which one substituted; run a limiter
   * individually through {@link masteringProcess} to attribute a non-zero count.
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
  /**
   * See {@link MasteringChainResult.nonFiniteSubstitutionCount}. Aggregated
   * over both channels, so it does not identify which channel substituted.
   */
  nonFiniteSubstitutionCount: number;
  stageGainReductions: StageGainReduction[];
  report: MasteringReport;
}

/**
 * @deprecated Use {@link MasteringChainStereoResult}. Retained as an alias for
 * source compatibility; the canonical name matches the Node and Python
 * bindings (`MasteringChainStereoResult`).
 */
export type MasteringStereoChainResult = MasteringChainStereoResult;

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
   * See {@link MasteringResult.nonFiniteSubstitutionCount}. Aggregated over both
   * channels, so it does not identify which channel substituted.
   */
  nonFiniteSubstitutionCount: number;
}
