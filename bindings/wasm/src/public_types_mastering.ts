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
  | 'gameOst';

export interface StreamingPlatform {
  name: string;
  targetLufs: number;
  ceilingDb: number;
}

export type SoloProcessor =
  | 'dynamics.brickwallLimiter'
  | 'dynamics.compressor'
  | 'dynamics.deesser'
  | 'dynamics.expander'
  | 'dynamics.gate'
  | 'dynamics.limiter'
  | 'dynamics.parallelComp'
  | 'dynamics.sidechainRouter'
  | 'dynamics.duckingProcessor'
  | 'dynamics.transientShaper'
  | 'dynamics.upwardCompressor'
  | 'dynamics.upwardExpander'
  | 'dynamics.vocalRider'
  | 'eq.apiStyle'
  | 'eq.bandPass'
  | 'eq.cutFilter'
  | 'eq.dynamic'
  | 'eq.equalizer'
  | 'eq.graphic'
  | 'eq.linearPhase'
  | 'eq.midSide'
  | 'eq.minimumPhase'
  | 'eq.parametric'
  | 'eq.pultec'
  | 'eq.shelving'
  | 'eq.tilt'
  | 'final.bitDepth'
  | 'final.dither'
  | 'final.outputChain'
  | 'maximizer.adaptiveRelease'
  | 'maximizer.loudnessOptimize'
  | 'maximizer.maximizer'
  | 'maximizer.softKneeMax'
  | 'maximizer.truePeakLimiter'
  | 'multiband.compressor'
  | 'multiband.dynamicEq'
  | 'multiband.expander'
  | 'multiband.imager'
  | 'multiband.limiter'
  | 'multiband.saturation'
  | 'repair.declick'
  | 'repair.declip'
  | 'repair.decrackle'
  | 'repair.dehum'
  | 'repair.denoiseClassical'
  | 'repair.dereverbClassical'
  | 'repair.trimSilence'
  | 'saturation.bitcrusher'
  | 'saturation.exciter'
  | 'saturation.hardClipper'
  | 'saturation.multibandExciter'
  | 'saturation.ampSim'
  | 'saturation.softClipper'
  | 'saturation.tape'
  | 'saturation.transformer'
  | 'saturation.tube'
  | 'saturation.waveshaper'
  | 'spectral.airBand'
  | 'spectral.lowEndFocus'
  | 'spectral.presenceEnhancer'
  | 'spectral.spectralShaper'
  | 'stereo.autoPan'
  | 'stereo.haasEnhancer'
  | 'stereo.imager'
  | 'stereo.monoMaker'
  | 'stereo.phaseAlign'
  | 'stereo.stereoBalance';

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
}

export type MasteringProcessorParams = Record<string, number | boolean>;

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
          nFft?: number;
          hopLength?: number;
          ddAlpha?: number;
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
    };
    exciter?: {
      enabled?: boolean;
      frequencyHz?: number;
      driveDb?: number;
      amount?: number;
      q?: number;
      evenOddMix?: number;
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
  };
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
   * `loudness.ceilingDb - loudnessStaticGainPeakDb` so the streaming preview
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
   * ITU-R BS.1770-4 true peak of the output (dBTP), measured with the chain's
   * configured loudness true-peak oversample factor (default 4x). Lets callers
   * verify a preset ceiling was met without a second oversampled scan.
   */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 Loudness Range of the output (LU). */
  outputLra: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
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
}
