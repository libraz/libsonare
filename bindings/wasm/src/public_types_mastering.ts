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
 *
 * The restoration presets' repair stages, like every classical denoise
 * configuration, can mistake a steady tone -- a calibration tone, a drone, a
 * long held note -- for noise and pull it down by the configured reduction
 * depth. Check for musical sustained tones before applying one.
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
  'effects.filter.vowel',
  'effects.gsEfx',
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
  'saturation.distortion',
  'saturation.exciter',
  'saturation.hardClipper',
  'saturation.multibandExciter',
  'saturation.overdrive',
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
  'stereo.binaural',
  'stereo.haasEnhancer',
  'stereo.imager',
  'stereo.monoMaker',
  'stereo.phaseAlign',
  'stereo.stereoBalance',
  'utility.gain',
  'voice.changer',
] as const;

export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];
// END GENERATED SoloProcessor

export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

/** Pair processors with a stereo entry point. */
export type StereoPairProcessor = 'match.abCrossfade';

export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

/**
 * A JSON string known to parse to a `R`.
 *
 * The helpers that return one cross as a string each facade parses, so nothing
 * type-checks the parse; this carries the result type at compile time only and
 * is a plain `string` at run time. Read it back with {@link JsonResult}.
 */
export type TypedJson<R> = string & { readonly __jsonResult?: R };

/** The result type a {@link TypedJson} string parses to. */
export type JsonResult<J> = J extends { readonly __jsonResult?: infer R } ? NonNullable<R> : never;

/**
 * The chain document {@link masteringAssistantSuggest} suggests: a version and
 * the flat `{ "module.param": value }` params. `params` keys depend on the
 * suggested stages, so they are not enumerated; a version 2 document carries the
 * multiband stage as a structured object under `dynamics.multibandComp`.
 */
export interface MasteringChainConfigDocument {
  version: 1 | 2;
  params: Record<string, number | boolean | Record<string, unknown>>;
}

/**
 * The measurements the assistant summarises its suggestion from. Flat, unlike
 * `MasteringAudioProfile`'s nested groups. A non-finite loudness is `null`.
 */
export interface MasteringAssistantProfile {
  durationSec: number;
  bpm: number;
  bpmConfidence: number;
  integratedLufs: number | null;
  lraLu: number;
  truePeakDb: number;
  crestFactorDb: number;
  spectralCentroidHz: number;
  spectralFlatness: number;
  attackDensity: number;
  sustainRatio: number;
}

/** The shape {@link masteringAssistantSuggest}'s and its stereo form's JSON parses to. */
export interface MasteringAssistantResult {
  chainConfig: MasteringChainConfigDocument;
  /** One line per decision the assistant made, in the order it made them. */
  explanation: string[];
  profile: MasteringAssistantProfile;
}

/** One delivery target in a {@link MasteringStreamingPreviewResult}. */
export interface MasteringStreamingPreviewPlatform {
  name: string;
  /** `null` for a silent or below-gate take. */
  integratedLufs: number | null;
  truePeakDb: number;
  /** Gain that lands the take on the platform's target; 0 when the loudness is `null`. */
  normalizationGainDb: number;
  /** True when that gain would push the true peak above the platform's ceiling. */
  ceilingRisk: boolean;
}

/** The shape {@link masteringStreamingPreview}'s and its stereo form's JSON parses to. */
export interface MasteringStreamingPreviewResult {
  platforms: MasteringStreamingPreviewPlatform[];
}

/** Result of the `match.referenceLoudness` pair analysis. A silent take reads `null`. */
export interface MatchReferenceLoudnessResult {
  sourceLufs: number | null;
  referenceLufs: number | null;
  gainToMatchDb: number | null;
}

/** One band of the `match.tonalBalance` and `match.tonalBalanceLogBands` analyses. */
export interface MatchTonalBalanceBand {
  lowHz: number;
  highHz: number;
  sourceDb: number;
  referenceDb: number;
  deviationDb: number;
}

/** Result of the `match.tonalBalance` pair analysis. */
export interface MatchTonalBalanceResult {
  bands: MatchTonalBalanceBand[];
}

/** Result of the `match.tonalBalanceLogBands` pair analysis. */
export interface MatchTonalBalanceLogBandsResult {
  bands: MatchTonalBalanceBand[];
}

/** Result of the `match.matchEqCurve` pair analysis: `gainDb[i]` is the gain at `frequencies[i]`. */
export interface MatchEqCurveResult {
  frequencies: number[];
  gainDb: number[];
}

/** Result of the `match.estimateReferenceDelaySamples` pair analysis. */
export interface MatchEstimateReferenceDelaySamplesResult {
  delaySamples: number;
}

/** What each {@link PairAnalysis} name's JSON parses to. */
export interface PairAnalysisResultMap {
  'match.referenceLoudness': MatchReferenceLoudnessResult;
  'match.tonalBalance': MatchTonalBalanceResult;
  'match.tonalBalanceLogBands': MatchTonalBalanceLogBandsResult;
  'match.matchEqCurve': MatchEqCurveResult;
  'match.estimateReferenceDelaySamples': MatchEstimateReferenceDelaySamplesResult;
}

/** Result of the `stereo.monoCompatCheck` analysis. `width` is `null` for a fully out-of-phase pair. */
export interface StereoMonoCompatCheckResult {
  correlation: number;
  width: number | null;
  monoPeak: number;
  sideRms: number;
  likelyMonoCompatible: boolean;
}

/** One band of the `stereo.monoCompatCheckLogBands` analysis. */
export interface StereoMonoCompatBand {
  lowHz: number;
  highHz: number;
  correlation: number;
  sideRms: number;
}

/** Result of the `stereo.monoCompatCheckLogBands` analysis. */
export interface StereoMonoCompatCheckLogBandsResult {
  bands: StereoMonoCompatBand[];
}

/** What each {@link StereoAnalysis} name's JSON parses to. */
export interface StereoAnalysisResultMap {
  'stereo.monoCompatCheck': StereoMonoCompatCheckResult;
  'stereo.monoCompatCheckLogBands': StereoMonoCompatCheckLogBandsResult;
}

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

/**
 * The loudness numbers a {@link StreamingMasteringChain} is constructed with,
 * measured at the loudness stage's input (after every earlier enabled stage),
 * which is where the offline chain measures.
 *
 * `loudnessStaticGainDb` equals the gain {@link masteringChain} applies, ceiling
 * clamp included, and 0 for a silent or below-gate stage input. Pass it and
 * `truePeakDb` as the chain config's `loudnessStaticGainDb` and
 * `loudnessStaticGainPeakDb`.
 */
export interface StreamingLoudnessGainResult {
  /** Static gain in dB the offline loudness stage applies. */
  loudnessStaticGainDb: number;
  /** True peak in dBTP of the loudness stage's input; -120 for digital silence. */
  truePeakDb: number;
  /** Integrated loudness in LUFS of the loudness stage's input; `-Infinity` below the absolute gate. */
  integratedLufs: number;
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
   * The gain that lands the remeasured source on `referenceLufs`; equals
   * `referenceLufs - sourceLufs` unless the gain moves blocks across the
   * absolute gate. Applied with no upper bound, and 0 whenever either loudness
   * is non-finite.
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

/** Stereo counterpart of {@link LoudnessMatchResult}. */
export interface LoudnessMatchStereoResult {
  /** The left source channel, gain-matched to the reference loudness. */
  left: Float32Array;
  /** The right source channel, gain-matched with the same gain as `left`. */
  right: Float32Array;
  sampleRate: number;
  /** The reference program's BS.1770 integrated loudness. */
  referenceLufs: number;
  /** The stereo source program's loudness before the gain. */
  sourceLufs: number;
  /** One gain applied to both source channels, in dB. */
  appliedGainDb: number;
  /** Maximum true peak across the matched left and right channels, in dBTP. */
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
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
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

export type MasteringProcessorParams = Record<string, number | boolean>;

/**
 * Params of a solo processor, keyed as in `masteringInsertParamInfo`. An
 * enum-valued key also takes its `choices` name (`noiseEstimator: 'mcra'`).
 */
export type MasteringSoloProcessorParams = Record<string, number | boolean | string>;

/** An insert parameter value: a solo value, or a list of numbers for an `array`-typed key. */
export type MasteringInsertParamValue = number | boolean | string | readonly number[];

/**
 * Insert parameters, keyed as in `masteringInsertParamInfo`; the shape an insert
 * is built from. Each value matches its key's declared `type`: a number or
 * boolean, an enum name, a string for a `string` key, a list of numbers for an
 * `array` key.
 */
export type MasteringInsertParams = Record<string, MasteringInsertParamValue>;

/**
 * Params accepted by the assistant entry points. Every key is numeric except
 * two NAMES: `targetPlatform`, a delivery target (`'broadcast'`, `'podcast'`,
 * `'club'`, ...), and `preset`, the mastering preset the suggestion starts from
 * (default `'streaming'`; restoration presets are refused). The assistant never
 * picks a preset from the audio. A number is rejected for either: the numeric
 * index the C ABI carries is a transport detail for callers that cannot pass a
 * string, not part of the JavaScript vocabulary.
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
          /** 0 / `'logMmse'`, 1 / `'mmseStsa'`, 2 / `'spectralSubtraction'`. */
          mode?: number | 'logMmse' | 'mmseStsa' | 'spectralSubtraction';
          /** 0 / `'quantile'`, 1 / `'mcra'`, 2 / `'imcra'`, 3 / `'spp'` (speech-presence probability). */
          noiseEstimator?: number | 'quantile' | 'mcra' | 'imcra' | 'spp';
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
      /** 0 / `'median'`, 1 / `'waveletShrinkage'`. */
      mode?: number | 'median' | 'waveletShrinkage';
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
      /** 0 / `'subtract'` the tracked harmonics, 1 / `'notch'` (cascaded notches). */
      mode?: number | 'subtract' | 'notch';
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
       * Antialiasing mode ordinal: 0 = none, 3 = 4x oversampling, which adds
       * 48 samples of latency at any host rate. The ADAA modes (1, 2) name a
       * member the exciter does not implement and are rejected; an ordinal
       * outside 0-3 is rejected as out of range.
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
   * is checked field by field while a dotted key is only checked at run time. An
   * enum-valued key also takes its name (`'repair.denoise.noiseEstimator': 'mcra'`).
   */
  [flatKey: `${string}.${string}`]: number | boolean | string | undefined;
}

/**
 * Configuration for the block-by-block {@link StreamingMasteringChain}.
 *
 * Extends {@link MasteringChainConfig} with optional precomputed loudness
 * parameters. The streaming chain cannot measure whole-signal integrated LUFS,
 * so an enabled `loudness` stage normally throws at construction. To let a
 * preset's streaming preview match its offline render, the caller precomputes
 * the loudness normalization gain offline with {@link streamingLoudnessGain},
 * which measures at the loudness stage's input like the offline chain, and
 * supplies it here.
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
   * True peak (dBFS) of the loudness stage's input the static gain was
   * computed for (`truePeakDb` of {@link streamingLoudnessGain}). When provided, the static gain is clamped to
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
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * See {@link MasteringResult.nonFiniteSubstitutionCount}. Aggregated over both
   * channels, so it does not identify which channel substituted.
   */
  nonFiniteSubstitutionCount: number;
}
