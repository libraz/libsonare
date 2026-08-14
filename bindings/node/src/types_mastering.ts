export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export type VoicePresetCategory =
  | 'monitor'
  | 'bright'
  | 'soft'
  | 'deep'
  | 'robot'
  | 'dark'
  | 'custom';

export interface RealtimeVoiceChangerPresetMetadata {
  schemaVersion: 1;
  id: string;
  name: string;
  description?: string;
  category: VoicePresetCategory;
}

export type RealtimeVoiceChangerPreset =
  | (RealtimeVoiceChangerPresetMetadata & {
      dsp: Record<string, unknown>;
      macros?: never;
    })
  | (RealtimeVoiceChangerPresetMetadata & {
      macros: Record<string, number>;
      dsp?: never;
    });

export type RealtimeVoiceChangerConfigInput =
  | VoicePresetId
  | RealtimeVoiceChangerPreset
  | RealtimeVoiceChangerConfig;

export interface RealtimeVoiceChangerOptions {
  sampleRate: number;
  maxBlockSize?: number;
  channels?: 1 | 2;
  preset?: RealtimeVoiceChangerConfigInput;
}

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`.
 */
export interface RealtimeVoiceChangerConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
}

/** Options for the high-level {@link mastering} one-shot. All fields are optional. */
export interface MasteringOptions {
  /** Integrated-loudness target in LUFS. Default -14. */
  targetLufs?: number;
  /** True-peak ceiling in dBTP. Default -1. */
  ceilingDb?: number;
  /** True-peak oversampling factor. Default 4. */
  truePeakOversample?: number;
  /** Post true-peak limiter release in ms. Default 0 => library default (50 ms). */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

/** Options for {@link pitchCorrectTimevarying}. All fields are optional. */
export interface PitchCorrectOptions {
  /** `'midi'` retunes toward {@link targetMidi}; `'scale'` snaps to the key. Default `'midi'`. */
  mode?: 'midi' | 'scale';
  /** Fixed target note when `mode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `mode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /** 12-bit degree mask, bit `i` = semitone `i` above the root enabled. Default C major. */
  scaleModeMask?: number;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  referenceMidi?: number;
  /** Correction strength in `[0, 1]`; 1 = full snap, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Per-frame voiced flags (non-zero = voiced); omit to treat all frames as voiced. */
  voiced?: Int32Array;
  /** Per-frame voicing probability in `[0, 1]`; omit to derive from `voiced`. */
  voicedProb?: Float32Array;
}

/** Options for {@link noteStretch}. All fields are optional. */
export interface NoteStretchOptions {
  /** First sample of the note to stretch. Default 0. */
  onsetSample?: number;
  /** Last sample of the note to stretch. Defaults to the input length. */
  offsetSample?: number;
  /** Stretch ratio (1 = unchanged). Default 1. */
  stretchRatio?: number;
}

/** Options for {@link noteMove}. */
export interface NoteMoveOptions {
  onsetSample?: number;
  /** Defaults to the input length. */
  offsetSample?: number;
  targetOnsetSample?: number;
}

export interface LufsResult {
  integratedLufs: number;
  /** Final complete 400 ms window, not Max-M. */
  momentaryLufs: number;
  /** Final complete 3 s window, not Max-S. */
  shortTermLufs: number;
  /** Maximum 400 ms window (EBU R128 Max-M). */
  maxMomentaryLufs: number;
  /** Maximum 3 s window (EBU R128 Max-S). */
  maxShortTermLufs: number;
  loudnessRange: number;
}

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

export interface MasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
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

export type PanMode =
  | 'balance'
  | 'pan'
  | 'stereoPan'
  | 'stereo-pan'
  | 'dualPan'
  | 'dual-pan'
  | number;

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source.
 */
export interface SurroundPan {
  /** -180..180 deg, 0 = front-center, positive = right. */
  azimuth?: number;
  /** Reserved (no height beds in phase 1). */
  elevation?: number;
  /** 0 = point source, 1 = spread across the front. */
  divergence?: number;
  /** 0..1 scalar send into the LFE plane. */
  lfe?: number;
  /** Reserved (focus/spread), defaults to 1. */
  distance?: number;
}

export interface MixOptions {
  inputTrimDb?: number | number[];
  faderDb?: number | number[];
  pan?: number | number[];
  panMode?: PanMode | PanMode[];
  width?: number | number[];
  muted?: boolean | boolean[];
}

export interface MixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /** Per-plane true-peak dB, length channelCount; [0]/[1] mirror truePeakDbL/R. */
  truePeakDb: number[];
}

export interface MixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 's-curve';

/**
 * Pan law applied by a strip's panner. On mono strips it changes the centre
 * gain; on stereo strips in Balance mode the centre remains unity and it
 * changes only the far-channel taper. Mapped to C enum ints
 * `0=const3dB`, `1=const4.5dB`, `2=const6dB`, `3=linear0dB`.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/**
 * Accepted pan-law name aliases. Names are normalized case-insensitively with
 * underscores treated as hyphens at runtime; the canonical four spellings in
 * {@link PanLaw} remain the preferred TypeScript values.
 */
export type PanLawName =
  | PanLaw
  | 'const-3db'
  | '-3db'
  | 'const-4.5db'
  | '-4.5db'
  | 'const-6db'
  | '-6db'
  | 'linear-0db'
  | 'linear'
  | '0db';

/** Pan-law name or raw C ABI ordinal. */
export type PanLawInput = PanLawName | number;

/**
 * Meter tap point on a strip. Mapped to the C enum ints
 * `0=preFader`, `1=postFader`.
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/**
 * A reference to a strip in the {@link Mixer}: either a 0-based strip index or
 * the strip's string id.
 */
export type StripRef = number | string;

/** Single goniometer sample from {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}

/**
 * How a {@link SpectralRegionOp} modifies the masked STFT bins. Mirrors the C
 * `SonareSpectralEditMode`:
 * - `'gain'`: multiply magnitude by `10^(gainDb/20)` (phase kept).
 * - `'attenuate'`: gain with a (typically negative) `gainDb`.
 * - `'mute'`: hard-zero the masked bins (`gainDb` ignored).
 * - `'heal'`: tonal continuation from neighbouring time frames.
 */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/**
 * One time x frequency rectangle edit op for {@link spectralEdit}. Ops apply in
 * array order. Mirrors the C `SonareSpectralRegionOp`.
 */
export interface SpectralRegionOp {
  /** Region time start in input samples (clamped to `[0, length]`). Default 0. */
  startSample?: number;
  /** Region time end (exclusive) in input samples. Default = signal length. */
  endSample?: number;
  /** Region frequency low edge in Hz (clamped to `[0, nyquist]`). Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz; `<= 0` or `>= nyquist` means nyquist. Default 0. */
  highHz?: number;
  /** Gain in dB for `'gain'` / `'attenuate'`; ignored by `'mute'` / `'heal'`. Default 0. */
  gainDb?: number;
  /** How the op modifies the masked bins. Default `'gain'`. */
  mode?: SpectralEditMode;
}

/**
 * STFT + heal parameters for {@link spectralEdit}. All fields are optional;
 * omitted fields take the documented native defaults. Mirrors the C
 * `SonareSpectralEditConfig`.
 */
export interface SpectralEditOptions {
  /** FFT size; must be a power of two `>= 2`. Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy `0 < hop <= nFft / 2`. Default 512. */
  hopLength?: number;
  /** Analysis/synthesis window. Default `'hann'`. */
  window?: 'hann' | 'hamming' | 'blackman' | 'rectangular';
  /** Neighbour frames each side used by `'heal'`. Default 2. */
  healRadiusFrames?: number;
}
