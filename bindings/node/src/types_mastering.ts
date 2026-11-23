import type { CompressorDetector } from './mastering_dynamics.js';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  HumDetection,
  NoiseDetection,
  ReverbDetection,
} from './types_repair.js';

/** Options of {@link masteringNormalizeLoudness}. All fields are optional. */
export interface MasteringNormalizeLoudnessOptions {
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
  /**
   * True-peak ceiling in dBTP. Default -1. Must be finite. Held on the true peak
   * read at {@link truePeakOversample}; a finer meter can read up to ~0.15 dB higher.
   */
  ceilingDbtp?: number;
  /** True-peak oversampling factor. Default 4; an explicit 0 is a `RangeError`. */
  truePeakOversample?: number;
  /**
   * Post true-peak limiter release in ms: one time constant, about 63% of the way at the set
   * time. Default 50; an explicit 0, a negative or a non-finite value is a `RangeError`.
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
 * holds, while the loudness target is what one normalization pass aims at. The
 * ceiling is held on the true peak read at the configured oversampling (4x, the
 * BS.1770 measure, by default); a finer meter can read up to ~0.15 dB higher.
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
 * configuration, can mistake a steady tone — a calibration tone, a drone, a
 * long held note — for noise and pull it down by the configured reduction
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

/**
 * One delivery target `masteringStreamingPreview` reports a normalization gain
 * for.
 *
 * Both numbers are required and must be finite. Unlike
 * {@link EngineBounceOptions.targetLufs}, which shares this name, unit and
 * default, a non-finite value here selects nothing — this interface has no
 * "use the library default" spelling — so it is refused by name.
 */
/**
 * Delivery targets the mastering assistant accepts as `targetPlatform`, in the
 * order {@link masteringPlatformNames} lists them.
 */
export type MasteringPlatform =
  | 'streaming'
  | 'youtube'
  | 'broadcast'
  | 'podcast'
  | 'audiobook'
  | 'cinema'
  | 'club'
  | 'cd';

/** One delivery target: its loudness target and true-peak ceiling. */
export interface StreamingPlatform {
  /** Platform name, echoed into the reported result. */
  name: string;
  /** Integrated-loudness target in LUFS, e.g. -14. */
  targetLufs: number;
  /** True-peak ceiling in dBTP, e.g. -1. */
  ceilingDbtp: number;
}

// BEGIN GENERATED SoloProcessor (make processor-types)
/**
 * Every processor name `masteringProcessorNames()` can return, and therefore
 * every name `masteringApplyProcessor` / `masteringApplyProcessorStereo` accept.
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
  'effects.reverb.fdn',
  'effects.reverb.plate',
  'effects.reverb.room',
  'effects.reverb.velvet',
  'eq.bandPass',
  'eq.cutFilter',
  'eq.dynamic',
  'eq.equalizer',
  'eq.graphic',
  'eq.linearPhase',
  'eq.midSide',
  'eq.minimumPhase',
  'eq.parametric',
  'eq.passiveTube',
  'eq.shelving',
  'eq.steppedProportionalQ',
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

/** A processor name the solo apply path accepts; see `SOLO_PROCESSORS`. */
export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];
// END GENERATED SoloProcessor

// BEGIN GENERATED MasteringInsertName (make processor-types)
/** Every processor id the realtime insert factory can build. */
export const MASTERING_INSERT_NAMES = [
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
  'effects.reverb.fdn',
  'effects.reverb.plate',
  'effects.reverb.room',
  'effects.reverb.velvet',
  'eq.bandPass',
  'eq.cutFilter',
  'eq.dynamic',
  'eq.equalizer',
  'eq.graphic',
  'eq.linearPhase',
  'eq.midSide',
  'eq.minimumPhase',
  'eq.parametric',
  'eq.passiveTube',
  'eq.shelving',
  'eq.steppedProportionalQ',
  'eq.tilt',
  'maximizer.adaptiveRelease',
  'maximizer.maximizer',
  'maximizer.softKneeMax',
  'maximizer.truePeakLimiter',
  'multiband.compressor',
  'multiband.dynamicEq',
  'multiband.expander',
  'multiband.imager',
  'multiband.limiter',
  'multiband.saturation',
  'repair.decrackle',
  'repair.dehum',
  'repair.denoiseClassical',
  'repair.dereverbClassical',
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

/** A processor id the realtime insert factory builds; see `MASTERING_INSERT_NAMES`. */
export type MasteringInsertName = (typeof MASTERING_INSERT_NAMES)[number];
// END GENERATED MasteringInsertName

// BEGIN GENERATED MasteringProcessorParamMap (make processor-types)
/**
 * Parameters of each catalog processor id, keyed as in `masteringInsertParamInfo`.
 * Every key is optional; an enum-valued key takes one of its `choices` names.
 */
export interface MasteringProcessorParamMap {
  /** Parameters of the `dynamics.brickwallLimiter` processor. */
  'dynamics.brickwallLimiter': {
    /** Unit: dBFS. Range: (-inf, 0]. Default: -1. */
    ceilingDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    releaseMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    lookaheadMs?: number;
  };
  /** Parameters of the `dynamics.compressor` processor. */
  'dynamics.compressor': {
    /** Unit: dB. Default: -18. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    releaseMs?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    makeupGainDb?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    kneeDb?: number;
    /** Default: false. */
    autoMakeup?: boolean;
    /** Default: 'rms'. */
    detector?: 'peak' | 'rms' | 'logRms';
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    sidechainHighPassHz?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    pdrTimeMs?: number;
    /** Range: [1, inf). Default: 1. */
    pdrReleaseScale?: number;
  };
  /** Parameters of the `dynamics.deesser` processor. */
  'dynamics.deesser': {
    /** Unit: Hz. Range: (0, inf). Default: 6000. */
    frequencyHz?: number;
    /** Unit: dB. Default: -24. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 4. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 60. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, inf). Default: 12. */
    rangeDb?: number;
    /** Range: (0, inf). Default: 1.5. */
    bandPassQ?: number;
  };
  /** Parameters of the `dynamics.duckingProcessor` processor. */
  'dynamics.duckingProcessor': {
    /** Unit: dB. Default: -18. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 8. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 2. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 150. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, inf). Default: 18. */
    rangeDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    lookaheadMs?: number;
  };
  /** Parameters of the `dynamics.expander` processor. */
  'dynamics.expander': {
    /** Unit: dB. Default: -40. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    releaseMs?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    rangeDb?: number;
  };
  /** Parameters of the `dynamics.gate` processor. */
  'dynamics.gate': {
    /** Unit: dB. Default: -50. */
    thresholdDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 2. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 80. */
    releaseMs?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -80. */
    rangeDb?: number;
    /** Unit: dB. Default: -50. */
    closeThresholdDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    holdMs?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    keyHighPassHz?: number;
  };
  /** Parameters of the `dynamics.limiter` processor. */
  'dynamics.limiter': {
    /** Unit: dB. Default: -1. */
    thresholdDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    releaseMs?: number;
    /** Unit: ratio. Range: [0, inf). Default: 0. */
    ratio?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    postGainDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    lookaheadMs?: number;
  };
  /** Parameters of the `dynamics.parallelComp` processor. */
  'dynamics.parallelComp': {
    /** Unit: dB. Default: -18. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 4. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    releaseMs?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    makeupGainDb?: number;
    /** Range: [0, 1]. Default: 0.5. */
    mix?: number;
    /** Unit: dBFS. Range: [-890, 770]. Default: 0. */
    outputCeilingDb?: number;
    /** Default: true. */
    linkedDetection?: boolean;
    /** Default: true. */
    outputLimiter?: boolean;
  };
  /** Parameters of the `dynamics.sidechainRouter` processor. */
  'dynamics.sidechainRouter': {
    /** Unit: dB. Default: -24. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 4. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, inf). Default: 18. */
    rangeDb?: number;
    /** Default: false. */
    keyListen?: boolean;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    lookaheadMs?: number;
    /** Default: false. */
    monoSumming?: boolean;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    sidechainHighPassHz?: number;
  };
  /** Parameters of the `dynamics.transientShaper` processor. */
  'dynamics.transientShaper': {
    /** Unit: dB. Default: 3. */
    attackGainDb?: number;
    /** Unit: dB. Default: 0. */
    sustainGainDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    fastAttackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 20. */
    fastReleaseMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 15. */
    slowAttackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 200. */
    slowReleaseMs?: number;
    /** Range: [0, inf). Default: 1. */
    sensitivity?: number;
    /** Unit: dB. Range: [0, 770]. Default: 12. */
    maxGainDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    gainSmoothingMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    lookaheadMs?: number;
  };
  /** Parameters of the `dynamics.upwardCompressor` processor. */
  'dynamics.upwardCompressor': {
    /** Unit: dB. Default: -30. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, 770]. Default: 12. */
    rangeDb?: number;
  };
  /** Parameters of the `dynamics.upwardExpander` processor. */
  'dynamics.upwardExpander': {
    /** Unit: dB. Default: -24. */
    thresholdDb?: number;
    /** Unit: ratio. Range: [1, inf). Default: 1.5. */
    ratio?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 80. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, 770]. Default: 12. */
    rangeDb?: number;
  };
  /** Parameters of the `dynamics.vocalRider` processor. */
  'dynamics.vocalRider': {
    /** Unit: dB. Default: -18. */
    targetDb?: number;
    /** Unit: dB. Range: [0, 770]. Default: 6. */
    maxBoostDb?: number;
    /** Unit: dB. Range: [0, inf). Default: 6. */
    maxCutDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 500. */
    releaseMs?: number;
    /** Unit: dB. Range: (-inf, 764]. Default: 0. */
    outputGainDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    gainSmoothingMs?: number;
    /** Unit: dB. Default: -60. */
    noiseFloorDb?: number;
    /** Default: true. */
    linkedDetection?: boolean;
  };
  /** Parameters of the `effects.acoustic.roomMorph` processor; positions are in metres. */
  'effects.acoustic.roomMorph': {
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Range: [0, 1]. Default: 0.5. */
    sourceTailSuppression?: number;
    /** Range: [0, 1]. Default: 0.2. */
    absorption?: number;
    /** Default: true. */
    airAbsorptionEnabled?: boolean;
    /** Unit: %. Range: [0, 100]. Default: 50. */
    airHumidityPercent?: number;
    /** Unit: degC. Range: (-273.15, inf). Default: 20. */
    airTemperatureC?: number;
    /** Default: unset. */
    bandAbsorption?: readonly number[];
    /** Default: unset. */
    bandScattering?: readonly number[];
    /** Unit: ms. Range: [0, 1000]. Default: 5. */
    crossfadeMs?: number;
    /** Unit: m. Range: (0, 10000]. Default: 3. */
    heightM?: number;
    /** Unit: count. Range: [0, inf). Default: 3. */
    ismOrder?: number;
    /** Unit: m. Range: (0, 10000]. Default: 7. */
    lengthM?: number;
    /** Unit: m. Range: (0, inf). Default: 5. */
    listenerX?: number;
    /** Unit: m. Range: (0, inf). Default: 4. */
    listenerY?: number;
    /** Unit: m. Range: [0, inf). Default: 1.7. */
    listenerZ?: number;
    /** Range: [0, 5]. Default: 0. */
    materialPreset?: number;
    /** Unit: s. Range: [0, 600]. Default: 0. */
    maxSec?: number;
    /** Unit: ms. Range: [0, 10000]. Default: 0. */
    mixingTimeMs?: number;
    /** Default: true. */
    preferEyring?: boolean;
    /** Unit: m. Range: (0, 2.5]. Default: 0.5. */
    receiverSpacingM?: number;
    /** Range: [0, inf). Default: 1. */
    seed?: number;
    /** Unit: m. Range: [0, inf). Default: 1. */
    sourceX?: number;
    /** Unit: m. Range: [0, inf). Default: 1. */
    sourceY?: number;
    /** Unit: m. Range: [0, inf). Default: 1.2. */
    sourceZ?: number;
    /** Unit: m. Range: (0, 10000]. Default: 5. */
    widthM?: number;
  };
  /** Parameters of the `effects.delay.stereo` processor. */
  'effects.delay.stereo': {
    /** Unit: ms. Range: [0, 4000]. Default: 250. */
    delayTimeLMs?: number;
    /** Unit: ms. Range: [0, 4000]. Default: 250. */
    delayTimeRMs?: number;
    /** Range: [-0.98, 0.98]. Default: 0.25. */
    feedback?: number;
    /** Range: [0, 1]. Default: 0. */
    pingPong?: number;
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    dampingHz?: number;
    /** Unit: ms. Range: [0, 4000]. Default: 0. */
    tap3Ms?: number;
    /** Unit: ms. Range: [0, 4000]. Default: 0. */
    tap4Ms?: number;
    /** Unit: dB. Range: [-120, 40]. Default: 0. */
    tap1LevelDb?: number;
    /** Unit: dB. Range: [-120, 40]. Default: 0. */
    tap2LevelDb?: number;
    /** Unit: dB. Range: [-120, 40]. Default: 0. */
    tap3LevelDb?: number;
    /** Unit: dB. Range: [-120, 40]. Default: 0. */
    tap4LevelDb?: number;
    /** Range: [-1, 1]. Default: 0. */
    tap3Pan?: number;
    /** Range: [-1, 1]. Default: 0. */
    tap4Pan?: number;
    /** Default: false. */
    invertL?: boolean;
    /** Default: false. */
    invertR?: boolean;
    /** Unit: Hz. Range: [0, 100]. Default: 0. */
    modRateHz?: number;
    /** Unit: ms. Range: [0, 4000]. Default: 0. */
    modDepthMs?: number;
    /** Unit: deg. Range: [0, 360]. Default: 0. */
    modPhaseDeg?: number;
    /** Unit: ms. Range: [0, 10000]. Default: 0. */
    glideMs?: number;
    /** Default: 'normal'. */
    crossMode?: 'normal' | 'pingPong' | 'cross';
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
  };
  /** Parameters of the `effects.filter.vowel` processor. */
  'effects.filter.vowel': {
    /** Range: [0, 4]. Default: 0. */
    vowel?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    accelMs?: number;
    /** Range: [0, 1]. Default: 0.5. */
    drive?: number;
    /** Default: false. */
    driveOn?: boolean;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
  };
  /** Parameters of the `effects.gsEfx` processor. */
  'effects.gsEfx': {
    /** Range: [0, 127]. Default: 0. */
    byte0?: number;
    /** Range: [0, 127]. Default: 0. */
    byte1?: number;
    /** Range: [0, 127]. Default: 0. */
    byte2?: number;
    /** Range: [0, 127]. Default: 0. */
    byte3?: number;
    /** Range: [0, 127]. Default: 0. */
    byte4?: number;
    /** Range: [0, 127]. Default: 0. */
    byte5?: number;
    /** Range: [0, 127]. Default: 0. */
    byte6?: number;
    /** Range: [0, 127]. Default: 0. */
    byte7?: number;
    /** Range: [0, 127]. Default: 0. */
    byte8?: number;
    /** Range: [0, 127]. Default: 0. */
    byte9?: number;
    /** Range: [0, 127]. Default: 0. */
    byte10?: number;
    /** Range: [0, 127]. Default: 0. */
    byte11?: number;
    /** Range: [0, 127]. Default: 0. */
    byte12?: number;
    /** Range: [0, 127]. Default: 0. */
    byte13?: number;
    /** Range: [0, 127]. Default: 0. */
    byte14?: number;
    /** Range: [0, 127]. Default: 0. */
    byte15?: number;
    /** Range: [0, 127]. Default: 0. */
    byte16?: number;
    /** Range: [0, 127]. Default: 0. */
    byte17?: number;
    /** Range: [0, 127]. Default: 0. */
    byte18?: number;
    /** Range: [0, 127]. Default: 127. */
    byte19?: number;
    /** Default: 'modern'. */
    realization?: 'modern' | 'classic';
    /** Default: 0. */
    typeLsb?: number;
    /** Default: 0. */
    typeMsb?: number;
  };
  /** Parameters of the `effects.modulation.autoWah` processor. */
  'effects.modulation.autoWah': {
    /** Range: [0, inf). Default: 1. */
    sensitivity?: number;
    /** Unit: Hz. Range: [10, inf). Default: 300. */
    minHz?: number;
    /** Unit: Hz. Range: [10, inf). Default: 2500. */
    maxHz?: number;
    /** Range: [0.5, inf). Default: 4. */
    resonance?: number;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
    /** Unit: Hz. Range: [0, inf). Default: 1. */
    lfoRateHz?: number;
    /** Range: [0, 1]. Default: 0. */
    lfoDepth?: number;
    /** Default: 'bandPass'. */
    filterType?: 'bandPass' | 'lowPass';
    /** Default: 'up'. */
    direction?: 'up' | 'down';
    /** Default: 'linearHz'. */
    sweepLaw?: 'linearHz' | 'linearOctave';
    /** Unit: ms. Range: [0, inf). Default: 8. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 120. */
    releaseMs?: number;
  };
  /** Parameters of the `effects.modulation.chorus` processor. */
  'effects.modulation.chorus': {
    /** Unit: Hz. Range: [0, inf). Default: 0.8. */
    rateHz?: number;
    /** Unit: ms. Range: [0, 50]. Default: 6. */
    depthMs?: number;
    /** Unit: ms. Range: [0, 100]. Default: 14. */
    centerDelayMs?: number;
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Unit: Hz. Default: 0. */
    preFilterHz?: number;
    /** Range: [-0.98, 0.98]. Default: 0. */
    feedback?: number;
    /** Unit: deg. Range: [0, 180]. Default: 90. */
    phaseDeg?: number;
    /** Default: 'off'. */
    preFilterMode?: 'off' | 'lowPass' | 'highPass';
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  /** Parameters of the `effects.modulation.ensemble` processor. */
  'effects.modulation.ensemble': {
    /** Unit: Hz. Range: [0, inf). Default: 0.6. */
    rateSlowHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 5.5. */
    rateFastHz?: number;
    /** Unit: ms. Range: [0, 10]. Default: 1.8. */
    depthSlowMs?: number;
    /** Unit: ms. Range: [0, 10]. Default: 0.25. */
    depthFastMs?: number;
    /** Unit: ms. Range: [0, 100]. Default: 5. */
    centerDelayMs?: number;
    /** Unit: Hz. Range: [500, 20000]. Default: 6500. */
    toneHz?: number;
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    rateHz?: number;
    /** Unit: ms. Range: [0, 20]. Default: 0. */
    preDelayDevMs?: number;
    /** Range: [-1, 1]. Default: 0. */
    depthDev?: number;
    /** Range: [0, 1]. Default: 0. */
    panDev?: number;
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  /** Parameters of the `effects.modulation.flanger` processor. */
  'effects.modulation.flanger': {
    /** Unit: Hz. Range: [0, inf). Default: 0.25. */
    rateHz?: number;
    /** Unit: ms. Range: [0, 100]. Default: 2. */
    depthMs?: number;
    /** Unit: ms. Range: [0, 100]. Default: 3. */
    centerDelayMs?: number;
    /** Range: [-0.98, 0.98]. Default: 0.3. */
    feedback?: number;
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Unit: Hz. Default: 0. */
    preFilterHz?: number;
    /** Unit: deg. Range: [0, 180]. Default: 180. */
    phaseDeg?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    stepRateHz?: number;
    /** Default: 'off'. */
    preFilterMode?: 'off' | 'lowPass' | 'highPass';
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  /** Parameters of the `effects.modulation.phaser` processor. */
  'effects.modulation.phaser': {
    /** Unit: Hz. Range: [0, inf). Default: 0.4. */
    rateHz?: number;
    /** Unit: Hz. Range: [1, inf). Default: 300. */
    minHz?: number;
    /** Unit: Hz. Range: [1, inf). Default: 1600. */
    maxHz?: number;
    /** Range: [0, 1]. Default: 0.5. */
    dryWet?: number;
    /** Range: [-0.98, 0.98]. Default: 0. */
    feedback?: number;
    /** Range: [0, 1]. Default: 1. */
    depth?: number;
    /** Default: 'crossfade'. */
    mixMode?: 'crossfade' | 'drySum';
    /** Unit: count. Range: [1, 12]. Default: 4. */
    stages?: number;
  };
  /** Parameters of the `effects.modulation.pitchShifter` processor. */
  'effects.modulation.pitchShifter': {
    /** Unit: semitones. Range: [-24, 24]. Default: 0. */
    pitchSemitones?: number;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
    /** Unit: cents. Range: [-100, 100]. Default: 0. */
    pitchCents?: number;
    /** Range: [-1, 1]. Default: 0. */
    pan?: number;
    /** Unit: semitones. Range: [-24, 24]. Default: 0. */
    pitchSemitones2?: number;
    /** Unit: cents. Range: [-100, 100]. Default: 0. */
    pitchCents2?: number;
    /** Range: [0, 1]. Default: 0. */
    level2?: number;
    /** Range: [-1, 1]. Default: 0. */
    pan2?: number;
    /** Range: [-0.98, 0.98]. Default: 0. */
    feedback?: number;
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
    /** Default: false. */
    antiAlias?: boolean;
    /** Unit: ms. Range: [0.5, 1000]. Default: 22.5. */
    windowMs?: number;
    /** Unit: ms. Range: [0, 1000]. Default: 0. */
    preDelayMs?: number;
    /** Unit: ms. Range: [0, 1000]. Default: 0. */
    preDelay2Ms?: number;
  };
  /** Parameters of the `effects.modulation.ringModulator` processor. */
  'effects.modulation.ringModulator': {
    /** Unit: Hz. Range: [0, inf). Default: 200. */
    carrierHz?: number;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
    /** Default: 'sine'. */
    shape?: 'sine' | 'triangle' | 'square' | 'sawUp' | 'sawDown';
    /** Unit: deg. Range: [0, 360]. Default: 0. */
    phaseDeg?: number;
    /** Range: [0, 1]. Default: 0. */
    stereoSpread?: number;
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  /** Parameters of the `effects.modulation.rotary` processor. */
  'effects.modulation.rotary': {
    /** Unit: Hz. Range: [0, inf). Default: 6. */
    rateHz?: number;
    /** Unit: ms. Range: [0, 20]. Default: 1.2. */
    depthMs?: number;
    /** Range: [0, 1]. Default: 0.5. */
    tremolo?: number;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
    /** Unit: Hz. Range: [0, inf). Default: 4.44. */
    drumRateHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0.8. */
    hornSlowHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 6. */
    hornFastHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0.6. */
    drumSlowHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 4.44. */
    drumFastHz?: number;
    /** Range: [-1, 1]. Default: -1. */
    speed?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    hornLevelDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    drumLevelDb?: number;
    /** Default: 'linear'. */
    interpolation?: 'linear' | 'lagrange3';
    /** Default: 'classic'. */
    model?: 'classic' | 'geometric';
    /** Range: [0, 1]. Default: 1. */
    stereoSpread?: number;
    /** Unit: s. Range: [0, inf). Default: 0. */
    accelTauSec?: number;
    /** Unit: s. Range: [0, inf). Default: 0. */
    decelTauSec?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    undershootHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    drumUndershootHz?: number;
  };
  /** Parameters of the `effects.modulation.wah` processor. */
  'effects.modulation.wah': {
    /** Unit: Hz. Range: [0, inf). Default: 1.5. */
    rateHz?: number;
    /** Unit: Hz. Range: [10, inf). Default: 400. */
    minHz?: number;
    /** Unit: Hz. Range: [10, inf). Default: 2000. */
    maxHz?: number;
    /** Range: [0.5, inf). Default: 4. */
    resonance?: number;
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
    /** Default: 'bandPass'. */
    filterType?: 'bandPass' | 'lowPass';
    /** Default: 'linearHz'. */
    sweepLaw?: 'linearHz' | 'linearOctave';
  };
  /** Parameters of the `effects.reverb.convolution` processor. */
  'effects.reverb.convolution': {
    /** Range: [0, 1]. Default: 0.35. */
    dryWet?: number;
    /** Unit: s. Range: [0, 12]. Default: unset. */
    decaySec?: number;
    /** Default: unset. */
    irF32Base64?: string;
    /** Unit: ms. Range: [0, 1000]. Default: 0. */
    preDelayMs?: number;
    /** Range: [0, inf). Default: 1364306893. */
    seed?: number;
  };
  /** Parameters of the `effects.reverb.fdn` processor. */
  'effects.reverb.fdn': {
    /** Range: [0, 1.5]. Default: 0.55. */
    decay?: number;
    /** Range: [0, 1]. Default: 0.5. */
    damping?: number;
    /** Range: [0, 1]. Default: 0.35. */
    dryWet?: number;
    /** Unit: s. Range: [0, 15]. Default: unset. */
    decaySec?: number;
    /** Range: [0, 1]. Default: 0.5. */
    hfDamping?: number;
  };
  /** Parameters of the `effects.reverb.plate` processor. */
  'effects.reverb.plate': {
    /** Range: [0, 0.98]. Default: 0.5. */
    decay?: number;
    /** Range: [0, 1]. Default: 0.5. */
    damping?: number;
    /** Range: [0, 1]. Default: 0.35. */
    dryWet?: number;
    /** Unit: Hz. Default: 0.5. */
    modRateHz?: number;
    /** Unit: samples. Range: [0, 672]. Default: 6. */
    modDepthSamples?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    dampingHz?: number;
    /** Unit: dB. Default: -120. */
    gateThresholdDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    gateHoldMs?: number;
    /** Default: 'normal'. */
    gateType?: 'normal' | 'reverse' | 'sweep1' | 'sweep2';
    /** Unit: ms. Range: [0, 4000]. Default: unset. */
    preDelayMs?: number;
    /** Range: [0, 6]. Default: 0. */
    character?: number;
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
    /** Unit: s. Range: [0.05, 62.0087]. Default: unset. */
    decaySec?: number;
  };
  /** Parameters of the `effects.reverb.room` processor; positions are in metres. */
  'effects.reverb.room': {
    /** Default: 0.35. */
    dryWet?: number;
    /** Range: [0, 1]. Default: 0.2. */
    absorption?: number;
    /** Default: true. */
    airAbsorptionEnabled?: boolean;
    /** Unit: %. Range: [0, 100]. Default: 50. */
    airHumidityPercent?: number;
    /** Unit: degC. Range: (-273.15, inf). Default: 20. */
    airTemperatureC?: number;
    /** Unit: m. Range: (0, 10000]. Default: 3. */
    heightM?: number;
    /** Unit: count. Range: [0, inf). Default: 3. */
    ismOrder?: number;
    /** Unit: m. Range: (0, 10000]. Default: 7. */
    lengthM?: number;
    /** Unit: m. Range: (0, inf). Default: 5. */
    listenerX?: number;
    /** Unit: m. Range: (0, inf). Default: 4. */
    listenerY?: number;
    /** Unit: m. Range: [0, inf). Default: 1.7. */
    listenerZ?: number;
    /** Unit: s. Range: [0, 600]. Default: 0. */
    maxSec?: number;
    /** Unit: m. Range: (0, 2.5]. Default: 0.5. */
    receiverSpacingM?: number;
    /** Range: [0, inf). Default: 1. */
    seed?: number;
    /** Unit: m. Range: [0, inf). Default: 1. */
    sourceX?: number;
    /** Unit: m. Range: [0, inf). Default: 1. */
    sourceY?: number;
    /** Unit: m. Range: [0, inf). Default: 1.2. */
    sourceZ?: number;
    /** Unit: m. Range: (0, 10000]. Default: 5. */
    widthM?: number;
  };
  /** Parameters of the `effects.reverb.velvet` processor. */
  'effects.reverb.velvet': {
    /** Range: [0, 1]. Default: 0.45. */
    decay?: number;
    /** Unit: s. Range: [0.05, 12]. Default: 1.5. */
    reverbTimeSec?: number;
    /** Range: [0, 1]. Default: 0.3. */
    dryWet?: number;
    /** Unit: Hz. Range: [1000, 3000]. Default: 2000. */
    densityHz?: number;
    /** Unit: s. Range: [0.0475, 11.4]. Default: unset. */
    decaySec?: number;
    /** Default: true. */
    enableShelf?: boolean;
  };
  /** Parameters of the `eq.bandPass` processor. */
  'eq.bandPass': {
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    bandPassFrequencyHz?: number;
    /** Range: (0, inf). Default: 1. */
    bandPassQ?: number;
    /** Unit: Hz. Default: 1000. */
    notchFrequencyHz?: number;
    /** Default: 1. */
    notchQ?: number;
    /** Default: true. */
    bandPassEnabled?: boolean;
    /** Default: false. */
    notchEnabled?: boolean;
  };
  /** Parameters of the `eq.cutFilter` processor. */
  'eq.cutFilter': {
    /** Unit: Hz. Default: 20. */
    highPassFrequencyHz?: number;
    /** Default: 0.70710677. */
    highPassQ?: number;
    /** Unit: Hz. Default: 20000. */
    lowPassFrequencyHz?: number;
    /** Default: 0.70710677. */
    lowPassQ?: number;
    /** Default: false. */
    highPassEnabled?: boolean;
    /** Default: 'db12PerOct'. */
    highPassSlope?:
      | 'db12PerOct'
      | 'db24PerOct'
      | 'db6PerOct'
      | 'db18PerOct'
      | 'db30PerOct'
      | 'db36PerOct'
      | 'db42PerOct'
      | 'db48PerOct'
      | 'db54PerOct'
      | 'db60PerOct'
      | 'db66PerOct'
      | 'db72PerOct'
      | 'db78PerOct'
      | 'db84PerOct'
      | 'db90PerOct'
      | 'db96PerOct'
      | 'brickwall';
    /** Default: false. */
    lowPassEnabled?: boolean;
    /** Default: 'db12PerOct'. */
    lowPassSlope?:
      | 'db12PerOct'
      | 'db24PerOct'
      | 'db6PerOct'
      | 'db18PerOct'
      | 'db30PerOct'
      | 'db36PerOct'
      | 'db42PerOct'
      | 'db48PerOct'
      | 'db54PerOct'
      | 'db60PerOct'
      | 'db66PerOct'
      | 'db72PerOct'
      | 'db78PerOct'
      | 'db84PerOct'
      | 'db90PerOct'
      | 'db96PerOct'
      | 'brickwall';
  };
  /** Parameters of the `eq.dynamic` processor. */
  'eq.dynamic': {
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band3.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.q'?: number;
    /** Unit: dB. Default: -24. */
    'band3.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band3.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band4.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.q'?: number;
    /** Unit: dB. Default: -24. */
    'band4.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band4.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band5.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.q'?: number;
    /** Unit: dB. Default: -24. */
    'band5.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band5.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band6.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.q'?: number;
    /** Unit: dB. Default: -24. */
    'band6.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band6.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band7.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.q'?: number;
    /** Unit: dB. Default: -24. */
    'band7.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band7.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.detectorDelayMs'?: number;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: 'peak'. */
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: 'peak'. */
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: 'peak'. */
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: 'peak'. */
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: 'peak'. */
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
  };
  /** Parameters of the `eq.equalizer` processor. */
  'eq.equalizer': {
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band0.gainDb'?: number;
    /** Default: 0.70710677. */
    'band0.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band1.gainDb'?: number;
    /** Default: 0.70710677. */
    'band1.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band2.gainDb'?: number;
    /** Default: 0.70710677. */
    'band2.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band3.gainDb'?: number;
    /** Default: 0.70710677. */
    'band3.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band4.gainDb'?: number;
    /** Default: 0.70710677. */
    'band4.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band5.gainDb'?: number;
    /** Default: 0.70710677. */
    'band5.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band6.gainDb'?: number;
    /** Default: 0.70710677. */
    'band6.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band7.gainDb'?: number;
    /** Default: 0.70710677. */
    'band7.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band8.gainDb'?: number;
    /** Default: 0.70710677. */
    'band8.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band9.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band9.gainDb'?: number;
    /** Default: 0.70710677. */
    'band9.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band10.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band10.gainDb'?: number;
    /** Default: 0.70710677. */
    'band10.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band11.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band11.gainDb'?: number;
    /** Default: 0.70710677. */
    'band11.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band12.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band12.gainDb'?: number;
    /** Default: 0.70710677. */
    'band12.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band13.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band13.gainDb'?: number;
    /** Default: 0.70710677. */
    'band13.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band14.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band14.gainDb'?: number;
    /** Default: 0.70710677. */
    'band14.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band15.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band15.gainDb'?: number;
    /** Default: 0.70710677. */
    'band15.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band16.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band16.gainDb'?: number;
    /** Default: 0.70710677. */
    'band16.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band17.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band17.gainDb'?: number;
    /** Default: 0.70710677. */
    'band17.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band18.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band18.gainDb'?: number;
    /** Default: 0.70710677. */
    'band18.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band19.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band19.gainDb'?: number;
    /** Default: 0.70710677. */
    'band19.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band20.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band20.gainDb'?: number;
    /** Default: 0.70710677. */
    'band20.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band21.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band21.gainDb'?: number;
    /** Default: 0.70710677. */
    'band21.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band22.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band22.gainDb'?: number;
    /** Default: 0.70710677. */
    'band22.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band23.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band23.gainDb'?: number;
    /** Default: 0.70710677. */
    'band23.q'?: number;
    /** Default: false. */
    autoGain?: boolean;
    /** Unit: ms. Default: 5. */
    'band0.attackMs'?: number;
    /** Default: false. */
    'band0.autoThreshold'?: boolean;
    /** Default: false. */
    'band0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band0.detectorDelayMs'?: number;
    /** Default: false. */
    'band0.dynamic'?: boolean;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band0.slopeDbOct'?: number;
    /** Default: false. */
    'band0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band1.attackMs'?: number;
    /** Default: false. */
    'band1.autoThreshold'?: boolean;
    /** Default: false. */
    'band1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band1.detectorDelayMs'?: number;
    /** Default: false. */
    'band1.dynamic'?: boolean;
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band1.slopeDbOct'?: number;
    /** Default: false. */
    'band1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band10.attackMs'?: number;
    /** Default: false. */
    'band10.autoThreshold'?: boolean;
    /** Default: false. */
    'band10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band10.detectorDelayMs'?: number;
    /** Default: false. */
    'band10.dynamic'?: boolean;
    /** Default: true. */
    'band10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band10.slopeDbOct'?: number;
    /** Default: false. */
    'band10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band11.attackMs'?: number;
    /** Default: false. */
    'band11.autoThreshold'?: boolean;
    /** Default: false. */
    'band11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band11.detectorDelayMs'?: number;
    /** Default: false. */
    'band11.dynamic'?: boolean;
    /** Default: true. */
    'band11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band11.slopeDbOct'?: number;
    /** Default: false. */
    'band11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band12.attackMs'?: number;
    /** Default: false. */
    'band12.autoThreshold'?: boolean;
    /** Default: false. */
    'band12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band12.detectorDelayMs'?: number;
    /** Default: false. */
    'band12.dynamic'?: boolean;
    /** Default: true. */
    'band12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band12.slopeDbOct'?: number;
    /** Default: false. */
    'band12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band13.attackMs'?: number;
    /** Default: false. */
    'band13.autoThreshold'?: boolean;
    /** Default: false. */
    'band13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band13.detectorDelayMs'?: number;
    /** Default: false. */
    'band13.dynamic'?: boolean;
    /** Default: true. */
    'band13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band13.slopeDbOct'?: number;
    /** Default: false. */
    'band13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band14.attackMs'?: number;
    /** Default: false. */
    'band14.autoThreshold'?: boolean;
    /** Default: false. */
    'band14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band14.detectorDelayMs'?: number;
    /** Default: false. */
    'band14.dynamic'?: boolean;
    /** Default: true. */
    'band14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band14.slopeDbOct'?: number;
    /** Default: false. */
    'band14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band15.attackMs'?: number;
    /** Default: false. */
    'band15.autoThreshold'?: boolean;
    /** Default: false. */
    'band15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band15.detectorDelayMs'?: number;
    /** Default: false. */
    'band15.dynamic'?: boolean;
    /** Default: true. */
    'band15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band15.slopeDbOct'?: number;
    /** Default: false. */
    'band15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band16.attackMs'?: number;
    /** Default: false. */
    'band16.autoThreshold'?: boolean;
    /** Default: false. */
    'band16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band16.detectorDelayMs'?: number;
    /** Default: false. */
    'band16.dynamic'?: boolean;
    /** Default: true. */
    'band16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band16.slopeDbOct'?: number;
    /** Default: false. */
    'band16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band17.attackMs'?: number;
    /** Default: false. */
    'band17.autoThreshold'?: boolean;
    /** Default: false. */
    'band17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band17.detectorDelayMs'?: number;
    /** Default: false. */
    'band17.dynamic'?: boolean;
    /** Default: true. */
    'band17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band17.slopeDbOct'?: number;
    /** Default: false. */
    'band17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band18.attackMs'?: number;
    /** Default: false. */
    'band18.autoThreshold'?: boolean;
    /** Default: false. */
    'band18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band18.detectorDelayMs'?: number;
    /** Default: false. */
    'band18.dynamic'?: boolean;
    /** Default: true. */
    'band18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band18.slopeDbOct'?: number;
    /** Default: false. */
    'band18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band19.attackMs'?: number;
    /** Default: false. */
    'band19.autoThreshold'?: boolean;
    /** Default: false. */
    'band19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band19.detectorDelayMs'?: number;
    /** Default: false. */
    'band19.dynamic'?: boolean;
    /** Default: true. */
    'band19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band19.slopeDbOct'?: number;
    /** Default: false. */
    'band19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band2.attackMs'?: number;
    /** Default: false. */
    'band2.autoThreshold'?: boolean;
    /** Default: false. */
    'band2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band2.detectorDelayMs'?: number;
    /** Default: false. */
    'band2.dynamic'?: boolean;
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band2.slopeDbOct'?: number;
    /** Default: false. */
    'band2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band20.attackMs'?: number;
    /** Default: false. */
    'band20.autoThreshold'?: boolean;
    /** Default: false. */
    'band20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band20.detectorDelayMs'?: number;
    /** Default: false. */
    'band20.dynamic'?: boolean;
    /** Default: true. */
    'band20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band20.slopeDbOct'?: number;
    /** Default: false. */
    'band20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band21.attackMs'?: number;
    /** Default: false. */
    'band21.autoThreshold'?: boolean;
    /** Default: false. */
    'band21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band21.detectorDelayMs'?: number;
    /** Default: false. */
    'band21.dynamic'?: boolean;
    /** Default: true. */
    'band21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band21.slopeDbOct'?: number;
    /** Default: false. */
    'band21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band22.attackMs'?: number;
    /** Default: false. */
    'band22.autoThreshold'?: boolean;
    /** Default: false. */
    'band22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band22.detectorDelayMs'?: number;
    /** Default: false. */
    'band22.dynamic'?: boolean;
    /** Default: true. */
    'band22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band22.slopeDbOct'?: number;
    /** Default: false. */
    'band22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band23.attackMs'?: number;
    /** Default: false. */
    'band23.autoThreshold'?: boolean;
    /** Default: false. */
    'band23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band23.detectorDelayMs'?: number;
    /** Default: false. */
    'band23.dynamic'?: boolean;
    /** Default: true. */
    'band23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band23.slopeDbOct'?: number;
    /** Default: false. */
    'band23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band3.attackMs'?: number;
    /** Default: false. */
    'band3.autoThreshold'?: boolean;
    /** Default: false. */
    'band3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band3.detectorDelayMs'?: number;
    /** Default: false. */
    'band3.dynamic'?: boolean;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band3.slopeDbOct'?: number;
    /** Default: false. */
    'band3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band4.attackMs'?: number;
    /** Default: false. */
    'band4.autoThreshold'?: boolean;
    /** Default: false. */
    'band4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band4.detectorDelayMs'?: number;
    /** Default: false. */
    'band4.dynamic'?: boolean;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band4.slopeDbOct'?: number;
    /** Default: false. */
    'band4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band5.attackMs'?: number;
    /** Default: false. */
    'band5.autoThreshold'?: boolean;
    /** Default: false. */
    'band5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band5.detectorDelayMs'?: number;
    /** Default: false. */
    'band5.dynamic'?: boolean;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band5.slopeDbOct'?: number;
    /** Default: false. */
    'band5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band6.attackMs'?: number;
    /** Default: false. */
    'band6.autoThreshold'?: boolean;
    /** Default: false. */
    'band6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band6.detectorDelayMs'?: number;
    /** Default: false. */
    'band6.dynamic'?: boolean;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band6.slopeDbOct'?: number;
    /** Default: false. */
    'band6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band7.attackMs'?: number;
    /** Default: false. */
    'band7.autoThreshold'?: boolean;
    /** Default: false. */
    'band7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band7.detectorDelayMs'?: number;
    /** Default: false. */
    'band7.dynamic'?: boolean;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band7.slopeDbOct'?: number;
    /** Default: false. */
    'band7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band8.attackMs'?: number;
    /** Default: false. */
    'band8.autoThreshold'?: boolean;
    /** Default: false. */
    'band8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band8.detectorDelayMs'?: number;
    /** Default: false. */
    'band8.dynamic'?: boolean;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band8.slopeDbOct'?: number;
    /** Default: false. */
    'band8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band9.attackMs'?: number;
    /** Default: false. */
    'band9.autoThreshold'?: boolean;
    /** Default: false. */
    'band9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band9.detectorDelayMs'?: number;
    /** Default: false. */
    'band9.dynamic'?: boolean;
    /** Default: true. */
    'band9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band9.slopeDbOct'?: number;
    /** Default: false. */
    'band9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'tiltShelf'
      | 'flatTilt'
      | 'allPass';
    /** Unit: samples. Default: 2048. */
    fftSize?: number;
    /** Range: [0, 2]. Default: 1. */
    gainScale?: number;
    /** Unit: samples. Range: [1, inf). Default: 513. */
    kernelSize?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    outputGainDb?: number;
    /** Range: [-1, 1]. Default: 0. */
    outputPan?: number;
    /** Unit: samples. Range: [0, inf). Default: 0. */
    partitionSize?: number;
    /** Default: 'zeroLatency'. */
    phaseMode?: 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'custom'. */
    resolution?: 'custom' | 'low' | 'medium' | 'high' | 'veryHigh' | 'maximum';
    /** Default: true. */
    usePartitionedConvolution?: boolean;
  };
  /** Parameters of the `eq.graphic` processor. */
  'eq.graphic': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band0GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band1GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band2GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band3GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band4GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band5GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band6GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band7GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band8GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band9GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band10GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band11GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band12GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band13GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band14GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band15GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band16GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band17GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band18GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band19GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band20GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band21GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band22GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band23GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band24GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band25GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band26GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band27GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band28GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band29GainDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    band30GainDb?: number;
    /** Range: [0, inf). Default: 0. */
    q?: number;
  };
  /** Parameters of the `eq.linearPhase` processor. */
  'eq.linearPhase': {
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band0.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band1.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band2.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band3.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band3.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band4.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band4.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band5.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band5.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band6.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band6.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band7.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band7.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band8.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band8.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band8.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band9.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band9.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band9.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band10.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band10.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band10.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band11.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band11.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band11.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band12.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band12.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band12.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band13.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band13.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band13.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band14.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band14.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band14.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band15.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band15.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band15.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band16.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band16.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band16.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band17.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band17.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band17.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band18.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band18.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band18.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band19.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band19.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band19.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band20.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band20.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band20.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band21.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band21.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band21.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band22.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band22.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band22.q'?: number;
    /** Unit: Hz. Range: (0, 24000). Default: 1000. */
    'band23.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band23.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band23.q'?: number;
    /** Unit: ms. Default: 5. */
    'band0.attackMs'?: number;
    /** Default: false. */
    'band0.autoThreshold'?: boolean;
    /** Default: false. */
    'band0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band0.detectorDelayMs'?: number;
    /** Default: false. */
    'band0.dynamic'?: boolean;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band0.slopeDbOct'?: number;
    /** Default: false. */
    'band0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band1.attackMs'?: number;
    /** Default: false. */
    'band1.autoThreshold'?: boolean;
    /** Default: false. */
    'band1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band1.detectorDelayMs'?: number;
    /** Default: false. */
    'band1.dynamic'?: boolean;
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band1.slopeDbOct'?: number;
    /** Default: false. */
    'band1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band10.attackMs'?: number;
    /** Default: false. */
    'band10.autoThreshold'?: boolean;
    /** Default: false. */
    'band10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band10.detectorDelayMs'?: number;
    /** Default: false. */
    'band10.dynamic'?: boolean;
    /** Default: true. */
    'band10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band10.slopeDbOct'?: number;
    /** Default: false. */
    'band10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band11.attackMs'?: number;
    /** Default: false. */
    'band11.autoThreshold'?: boolean;
    /** Default: false. */
    'band11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band11.detectorDelayMs'?: number;
    /** Default: false. */
    'band11.dynamic'?: boolean;
    /** Default: true. */
    'band11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band11.slopeDbOct'?: number;
    /** Default: false. */
    'band11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band12.attackMs'?: number;
    /** Default: false. */
    'band12.autoThreshold'?: boolean;
    /** Default: false. */
    'band12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band12.detectorDelayMs'?: number;
    /** Default: false. */
    'band12.dynamic'?: boolean;
    /** Default: true. */
    'band12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band12.slopeDbOct'?: number;
    /** Default: false. */
    'band12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band13.attackMs'?: number;
    /** Default: false. */
    'band13.autoThreshold'?: boolean;
    /** Default: false. */
    'band13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band13.detectorDelayMs'?: number;
    /** Default: false. */
    'band13.dynamic'?: boolean;
    /** Default: true. */
    'band13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band13.slopeDbOct'?: number;
    /** Default: false. */
    'band13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band14.attackMs'?: number;
    /** Default: false. */
    'band14.autoThreshold'?: boolean;
    /** Default: false. */
    'band14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band14.detectorDelayMs'?: number;
    /** Default: false. */
    'band14.dynamic'?: boolean;
    /** Default: true. */
    'band14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band14.slopeDbOct'?: number;
    /** Default: false. */
    'band14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band15.attackMs'?: number;
    /** Default: false. */
    'band15.autoThreshold'?: boolean;
    /** Default: false. */
    'band15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band15.detectorDelayMs'?: number;
    /** Default: false. */
    'band15.dynamic'?: boolean;
    /** Default: true. */
    'band15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band15.slopeDbOct'?: number;
    /** Default: false. */
    'band15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band16.attackMs'?: number;
    /** Default: false. */
    'band16.autoThreshold'?: boolean;
    /** Default: false. */
    'band16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band16.detectorDelayMs'?: number;
    /** Default: false. */
    'band16.dynamic'?: boolean;
    /** Default: true. */
    'band16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band16.slopeDbOct'?: number;
    /** Default: false. */
    'band16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band17.attackMs'?: number;
    /** Default: false. */
    'band17.autoThreshold'?: boolean;
    /** Default: false. */
    'band17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band17.detectorDelayMs'?: number;
    /** Default: false. */
    'band17.dynamic'?: boolean;
    /** Default: true. */
    'band17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band17.slopeDbOct'?: number;
    /** Default: false. */
    'band17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band18.attackMs'?: number;
    /** Default: false. */
    'band18.autoThreshold'?: boolean;
    /** Default: false. */
    'band18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band18.detectorDelayMs'?: number;
    /** Default: false. */
    'band18.dynamic'?: boolean;
    /** Default: true. */
    'band18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band18.slopeDbOct'?: number;
    /** Default: false. */
    'band18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band19.attackMs'?: number;
    /** Default: false. */
    'band19.autoThreshold'?: boolean;
    /** Default: false. */
    'band19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band19.detectorDelayMs'?: number;
    /** Default: false. */
    'band19.dynamic'?: boolean;
    /** Default: true. */
    'band19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band19.slopeDbOct'?: number;
    /** Default: false. */
    'band19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band2.attackMs'?: number;
    /** Default: false. */
    'band2.autoThreshold'?: boolean;
    /** Default: false. */
    'band2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band2.detectorDelayMs'?: number;
    /** Default: false. */
    'band2.dynamic'?: boolean;
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band2.slopeDbOct'?: number;
    /** Default: false. */
    'band2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band20.attackMs'?: number;
    /** Default: false. */
    'band20.autoThreshold'?: boolean;
    /** Default: false. */
    'band20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band20.detectorDelayMs'?: number;
    /** Default: false. */
    'band20.dynamic'?: boolean;
    /** Default: true. */
    'band20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band20.slopeDbOct'?: number;
    /** Default: false. */
    'band20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band21.attackMs'?: number;
    /** Default: false. */
    'band21.autoThreshold'?: boolean;
    /** Default: false. */
    'band21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band21.detectorDelayMs'?: number;
    /** Default: false. */
    'band21.dynamic'?: boolean;
    /** Default: true. */
    'band21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band21.slopeDbOct'?: number;
    /** Default: false. */
    'band21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band22.attackMs'?: number;
    /** Default: false. */
    'band22.autoThreshold'?: boolean;
    /** Default: false. */
    'band22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band22.detectorDelayMs'?: number;
    /** Default: false. */
    'band22.dynamic'?: boolean;
    /** Default: true. */
    'band22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band22.slopeDbOct'?: number;
    /** Default: false. */
    'band22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band23.attackMs'?: number;
    /** Default: false. */
    'band23.autoThreshold'?: boolean;
    /** Default: false. */
    'band23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band23.detectorDelayMs'?: number;
    /** Default: false. */
    'band23.dynamic'?: boolean;
    /** Default: true. */
    'band23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band23.slopeDbOct'?: number;
    /** Default: false. */
    'band23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band3.attackMs'?: number;
    /** Default: false. */
    'band3.autoThreshold'?: boolean;
    /** Default: false. */
    'band3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band3.detectorDelayMs'?: number;
    /** Default: false. */
    'band3.dynamic'?: boolean;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band3.slopeDbOct'?: number;
    /** Default: false. */
    'band3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band4.attackMs'?: number;
    /** Default: false. */
    'band4.autoThreshold'?: boolean;
    /** Default: false. */
    'band4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band4.detectorDelayMs'?: number;
    /** Default: false. */
    'band4.dynamic'?: boolean;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band4.slopeDbOct'?: number;
    /** Default: false. */
    'band4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band5.attackMs'?: number;
    /** Default: false. */
    'band5.autoThreshold'?: boolean;
    /** Default: false. */
    'band5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band5.detectorDelayMs'?: number;
    /** Default: false. */
    'band5.dynamic'?: boolean;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band5.slopeDbOct'?: number;
    /** Default: false. */
    'band5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band6.attackMs'?: number;
    /** Default: false. */
    'band6.autoThreshold'?: boolean;
    /** Default: false. */
    'band6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band6.detectorDelayMs'?: number;
    /** Default: false. */
    'band6.dynamic'?: boolean;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band6.slopeDbOct'?: number;
    /** Default: false. */
    'band6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band7.attackMs'?: number;
    /** Default: false. */
    'band7.autoThreshold'?: boolean;
    /** Default: false. */
    'band7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band7.detectorDelayMs'?: number;
    /** Default: false. */
    'band7.dynamic'?: boolean;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band7.slopeDbOct'?: number;
    /** Default: false. */
    'band7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band8.attackMs'?: number;
    /** Default: false. */
    'band8.autoThreshold'?: boolean;
    /** Default: false. */
    'band8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band8.detectorDelayMs'?: number;
    /** Default: false. */
    'band8.dynamic'?: boolean;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band8.slopeDbOct'?: number;
    /** Default: false. */
    'band8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: ms. Default: 5. */
    'band9.attackMs'?: number;
    /** Default: false. */
    'band9.autoThreshold'?: boolean;
    /** Default: false. */
    'band9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band9.detectorDelayMs'?: number;
    /** Default: false. */
    'band9.dynamic'?: boolean;
    /** Default: true. */
    'band9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band9.slopeDbOct'?: number;
    /** Default: false. */
    'band9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    /** Unit: samples. Default: 2048. */
    fftSize?: number;
    /** Unit: samples. Range: [1, inf). Default: 513. */
    kernelSize?: number;
    /** Unit: samples. Range: [0, inf). Default: 0. */
    partitionSize?: number;
    /** Default: 'custom'. */
    resolution?: 'custom' | 'low' | 'medium' | 'high' | 'veryHigh' | 'maximum';
    /** Default: true. */
    usePartitionedConvolution?: boolean;
  };
  /** Parameters of the `eq.midSide` processor. */
  'eq.midSide': {
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand0.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand0.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand1.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand1.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand2.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand2.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand3.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand3.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand4.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand4.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand5.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand5.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand6.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand6.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand7.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand7.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand8.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand8.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand8.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand9.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand9.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand9.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand10.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand10.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand10.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand11.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand11.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand11.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand12.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand12.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand12.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand13.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand13.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand13.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand14.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand14.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand14.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand15.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand15.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand15.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand16.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand16.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand16.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand17.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand17.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand17.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand18.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand18.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand18.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand19.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand19.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand19.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand20.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand20.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand20.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand21.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand21.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand21.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand22.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand22.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand22.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'midBand23.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'midBand23.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'midBand23.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand0.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand0.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand1.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand1.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand2.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand2.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand3.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand3.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand4.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand4.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand5.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand5.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand6.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand6.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand7.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand7.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand8.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand8.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand8.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand9.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand9.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand9.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand10.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand10.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand10.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand11.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand11.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand11.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand12.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand12.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand12.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand13.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand13.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand13.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand14.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand14.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand14.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand15.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand15.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand15.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand16.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand16.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand16.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand17.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand17.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand17.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand18.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand18.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand18.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand19.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand19.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand19.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand20.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand20.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand20.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand21.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand21.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand21.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand22.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand22.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand22.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'sideBand23.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'sideBand23.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'sideBand23.q'?: number;
    /** Unit: ms. Default: 5. */
    'midBand0.attackMs'?: number;
    /** Default: false. */
    'midBand0.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand0.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand0.dynamic'?: boolean;
    /** Default: true. */
    'midBand0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand0.slopeDbOct'?: number;
    /** Default: false. */
    'midBand0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand1.attackMs'?: number;
    /** Default: false. */
    'midBand1.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand1.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand1.dynamic'?: boolean;
    /** Default: true. */
    'midBand1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand1.slopeDbOct'?: number;
    /** Default: false. */
    'midBand1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand10.attackMs'?: number;
    /** Default: false. */
    'midBand10.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand10.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand10.dynamic'?: boolean;
    /** Default: true. */
    'midBand10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand10.slopeDbOct'?: number;
    /** Default: false. */
    'midBand10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand11.attackMs'?: number;
    /** Default: false. */
    'midBand11.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand11.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand11.dynamic'?: boolean;
    /** Default: true. */
    'midBand11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand11.slopeDbOct'?: number;
    /** Default: false. */
    'midBand11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand12.attackMs'?: number;
    /** Default: false. */
    'midBand12.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand12.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand12.dynamic'?: boolean;
    /** Default: true. */
    'midBand12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand12.slopeDbOct'?: number;
    /** Default: false. */
    'midBand12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand13.attackMs'?: number;
    /** Default: false. */
    'midBand13.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand13.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand13.dynamic'?: boolean;
    /** Default: true. */
    'midBand13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand13.slopeDbOct'?: number;
    /** Default: false. */
    'midBand13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand14.attackMs'?: number;
    /** Default: false. */
    'midBand14.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand14.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand14.dynamic'?: boolean;
    /** Default: true. */
    'midBand14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand14.slopeDbOct'?: number;
    /** Default: false. */
    'midBand14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand15.attackMs'?: number;
    /** Default: false. */
    'midBand15.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand15.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand15.dynamic'?: boolean;
    /** Default: true. */
    'midBand15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand15.slopeDbOct'?: number;
    /** Default: false. */
    'midBand15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand16.attackMs'?: number;
    /** Default: false. */
    'midBand16.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand16.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand16.dynamic'?: boolean;
    /** Default: true. */
    'midBand16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand16.slopeDbOct'?: number;
    /** Default: false. */
    'midBand16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand17.attackMs'?: number;
    /** Default: false. */
    'midBand17.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand17.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand17.dynamic'?: boolean;
    /** Default: true. */
    'midBand17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand17.slopeDbOct'?: number;
    /** Default: false. */
    'midBand17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand18.attackMs'?: number;
    /** Default: false. */
    'midBand18.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand18.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand18.dynamic'?: boolean;
    /** Default: true. */
    'midBand18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand18.slopeDbOct'?: number;
    /** Default: false. */
    'midBand18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand19.attackMs'?: number;
    /** Default: false. */
    'midBand19.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand19.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand19.dynamic'?: boolean;
    /** Default: true. */
    'midBand19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand19.slopeDbOct'?: number;
    /** Default: false. */
    'midBand19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand2.attackMs'?: number;
    /** Default: false. */
    'midBand2.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand2.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand2.dynamic'?: boolean;
    /** Default: true. */
    'midBand2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand2.slopeDbOct'?: number;
    /** Default: false. */
    'midBand2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand20.attackMs'?: number;
    /** Default: false. */
    'midBand20.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand20.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand20.dynamic'?: boolean;
    /** Default: true. */
    'midBand20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand20.slopeDbOct'?: number;
    /** Default: false. */
    'midBand20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand21.attackMs'?: number;
    /** Default: false. */
    'midBand21.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand21.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand21.dynamic'?: boolean;
    /** Default: true. */
    'midBand21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand21.slopeDbOct'?: number;
    /** Default: false. */
    'midBand21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand22.attackMs'?: number;
    /** Default: false. */
    'midBand22.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand22.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand22.dynamic'?: boolean;
    /** Default: true. */
    'midBand22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand22.slopeDbOct'?: number;
    /** Default: false. */
    'midBand22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand23.attackMs'?: number;
    /** Default: false. */
    'midBand23.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand23.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand23.dynamic'?: boolean;
    /** Default: true. */
    'midBand23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand23.slopeDbOct'?: number;
    /** Default: false. */
    'midBand23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand3.attackMs'?: number;
    /** Default: false. */
    'midBand3.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand3.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand3.dynamic'?: boolean;
    /** Default: true. */
    'midBand3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand3.slopeDbOct'?: number;
    /** Default: false. */
    'midBand3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand4.attackMs'?: number;
    /** Default: false. */
    'midBand4.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand4.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand4.dynamic'?: boolean;
    /** Default: true. */
    'midBand4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand4.slopeDbOct'?: number;
    /** Default: false. */
    'midBand4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand5.attackMs'?: number;
    /** Default: false. */
    'midBand5.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand5.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand5.dynamic'?: boolean;
    /** Default: true. */
    'midBand5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand5.slopeDbOct'?: number;
    /** Default: false. */
    'midBand5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand6.attackMs'?: number;
    /** Default: false. */
    'midBand6.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand6.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand6.dynamic'?: boolean;
    /** Default: true. */
    'midBand6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand6.slopeDbOct'?: number;
    /** Default: false. */
    'midBand6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand7.attackMs'?: number;
    /** Default: false. */
    'midBand7.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand7.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand7.dynamic'?: boolean;
    /** Default: true. */
    'midBand7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand7.slopeDbOct'?: number;
    /** Default: false. */
    'midBand7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand8.attackMs'?: number;
    /** Default: false. */
    'midBand8.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand8.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand8.dynamic'?: boolean;
    /** Default: true. */
    'midBand8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand8.slopeDbOct'?: number;
    /** Default: false. */
    'midBand8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'midBand9.attackMs'?: number;
    /** Default: false. */
    'midBand9.autoThreshold'?: boolean;
    /** Default: false. */
    'midBand9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'midBand9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'midBand9.detectorDelayMs'?: number;
    /** Default: false. */
    'midBand9.dynamic'?: boolean;
    /** Default: true. */
    'midBand9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'midBand9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'midBand9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'midBand9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'midBand9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'midBand9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'midBand9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'midBand9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'midBand9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'midBand9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'midBand9.slopeDbOct'?: number;
    /** Default: false. */
    'midBand9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'midBand9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'midBand9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand0.attackMs'?: number;
    /** Default: false. */
    'sideBand0.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand0.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand0.dynamic'?: boolean;
    /** Default: true. */
    'sideBand0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand0.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand1.attackMs'?: number;
    /** Default: false. */
    'sideBand1.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand1.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand1.dynamic'?: boolean;
    /** Default: true. */
    'sideBand1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand1.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand10.attackMs'?: number;
    /** Default: false. */
    'sideBand10.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand10.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand10.dynamic'?: boolean;
    /** Default: true. */
    'sideBand10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand10.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand11.attackMs'?: number;
    /** Default: false. */
    'sideBand11.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand11.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand11.dynamic'?: boolean;
    /** Default: true. */
    'sideBand11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand11.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand12.attackMs'?: number;
    /** Default: false. */
    'sideBand12.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand12.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand12.dynamic'?: boolean;
    /** Default: true. */
    'sideBand12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand12.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand13.attackMs'?: number;
    /** Default: false. */
    'sideBand13.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand13.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand13.dynamic'?: boolean;
    /** Default: true. */
    'sideBand13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand13.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand14.attackMs'?: number;
    /** Default: false. */
    'sideBand14.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand14.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand14.dynamic'?: boolean;
    /** Default: true. */
    'sideBand14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand14.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand15.attackMs'?: number;
    /** Default: false. */
    'sideBand15.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand15.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand15.dynamic'?: boolean;
    /** Default: true. */
    'sideBand15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand15.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand16.attackMs'?: number;
    /** Default: false. */
    'sideBand16.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand16.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand16.dynamic'?: boolean;
    /** Default: true. */
    'sideBand16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand16.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand17.attackMs'?: number;
    /** Default: false. */
    'sideBand17.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand17.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand17.dynamic'?: boolean;
    /** Default: true. */
    'sideBand17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand17.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand18.attackMs'?: number;
    /** Default: false. */
    'sideBand18.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand18.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand18.dynamic'?: boolean;
    /** Default: true. */
    'sideBand18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand18.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand19.attackMs'?: number;
    /** Default: false. */
    'sideBand19.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand19.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand19.dynamic'?: boolean;
    /** Default: true. */
    'sideBand19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand19.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand2.attackMs'?: number;
    /** Default: false. */
    'sideBand2.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand2.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand2.dynamic'?: boolean;
    /** Default: true. */
    'sideBand2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand2.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand20.attackMs'?: number;
    /** Default: false. */
    'sideBand20.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand20.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand20.dynamic'?: boolean;
    /** Default: true. */
    'sideBand20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand20.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand21.attackMs'?: number;
    /** Default: false. */
    'sideBand21.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand21.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand21.dynamic'?: boolean;
    /** Default: true. */
    'sideBand21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand21.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand22.attackMs'?: number;
    /** Default: false. */
    'sideBand22.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand22.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand22.dynamic'?: boolean;
    /** Default: true. */
    'sideBand22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand22.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand23.attackMs'?: number;
    /** Default: false. */
    'sideBand23.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand23.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand23.dynamic'?: boolean;
    /** Default: true. */
    'sideBand23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand23.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand3.attackMs'?: number;
    /** Default: false. */
    'sideBand3.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand3.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand3.dynamic'?: boolean;
    /** Default: true. */
    'sideBand3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand3.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand4.attackMs'?: number;
    /** Default: false. */
    'sideBand4.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand4.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand4.dynamic'?: boolean;
    /** Default: true. */
    'sideBand4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand4.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand5.attackMs'?: number;
    /** Default: false. */
    'sideBand5.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand5.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand5.dynamic'?: boolean;
    /** Default: true. */
    'sideBand5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand5.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand6.attackMs'?: number;
    /** Default: false. */
    'sideBand6.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand6.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand6.dynamic'?: boolean;
    /** Default: true. */
    'sideBand6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand6.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand7.attackMs'?: number;
    /** Default: false. */
    'sideBand7.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand7.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand7.dynamic'?: boolean;
    /** Default: true. */
    'sideBand7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand7.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand8.attackMs'?: number;
    /** Default: false. */
    'sideBand8.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand8.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand8.dynamic'?: boolean;
    /** Default: true. */
    'sideBand8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand8.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'sideBand9.attackMs'?: number;
    /** Default: false. */
    'sideBand9.autoThreshold'?: boolean;
    /** Default: false. */
    'sideBand9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'sideBand9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'sideBand9.detectorDelayMs'?: number;
    /** Default: false. */
    'sideBand9.dynamic'?: boolean;
    /** Default: true. */
    'sideBand9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'sideBand9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'sideBand9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'sideBand9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'sideBand9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'sideBand9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'sideBand9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'sideBand9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'sideBand9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'sideBand9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'sideBand9.slopeDbOct'?: number;
    /** Default: false. */
    'sideBand9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'sideBand9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'sideBand9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
  };
  /** Parameters of the `eq.minimumPhase` processor. */
  'eq.minimumPhase': {
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band0.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band1.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band2.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band3.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band3.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band4.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band4.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band5.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band5.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band6.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band6.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band7.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band7.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band8.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band8.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band8.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band9.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band9.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band9.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band10.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band10.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band10.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band11.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band11.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band11.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band12.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band12.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band12.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band13.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band13.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band13.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band14.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band14.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band14.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band15.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band15.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band15.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band16.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band16.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band16.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band17.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band17.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band17.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band18.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band18.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band18.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band19.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band19.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band19.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band20.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band20.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band20.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band21.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band21.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band21.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band22.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band22.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band22.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band23.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band23.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band23.q'?: number;
    /** Unit: ms. Default: 5. */
    'band0.attackMs'?: number;
    /** Default: false. */
    'band0.autoThreshold'?: boolean;
    /** Default: false. */
    'band0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band0.detectorDelayMs'?: number;
    /** Default: false. */
    'band0.dynamic'?: boolean;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band0.slopeDbOct'?: number;
    /** Default: false. */
    'band0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band1.attackMs'?: number;
    /** Default: false. */
    'band1.autoThreshold'?: boolean;
    /** Default: false. */
    'band1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band1.detectorDelayMs'?: number;
    /** Default: false. */
    'band1.dynamic'?: boolean;
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band1.slopeDbOct'?: number;
    /** Default: false. */
    'band1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band10.attackMs'?: number;
    /** Default: false. */
    'band10.autoThreshold'?: boolean;
    /** Default: false. */
    'band10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band10.detectorDelayMs'?: number;
    /** Default: false. */
    'band10.dynamic'?: boolean;
    /** Default: true. */
    'band10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band10.slopeDbOct'?: number;
    /** Default: false. */
    'band10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band11.attackMs'?: number;
    /** Default: false. */
    'band11.autoThreshold'?: boolean;
    /** Default: false. */
    'band11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band11.detectorDelayMs'?: number;
    /** Default: false. */
    'band11.dynamic'?: boolean;
    /** Default: true. */
    'band11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band11.slopeDbOct'?: number;
    /** Default: false. */
    'band11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band12.attackMs'?: number;
    /** Default: false. */
    'band12.autoThreshold'?: boolean;
    /** Default: false. */
    'band12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band12.detectorDelayMs'?: number;
    /** Default: false. */
    'band12.dynamic'?: boolean;
    /** Default: true. */
    'band12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band12.slopeDbOct'?: number;
    /** Default: false. */
    'band12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band13.attackMs'?: number;
    /** Default: false. */
    'band13.autoThreshold'?: boolean;
    /** Default: false. */
    'band13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band13.detectorDelayMs'?: number;
    /** Default: false. */
    'band13.dynamic'?: boolean;
    /** Default: true. */
    'band13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band13.slopeDbOct'?: number;
    /** Default: false. */
    'band13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band14.attackMs'?: number;
    /** Default: false. */
    'band14.autoThreshold'?: boolean;
    /** Default: false. */
    'band14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band14.detectorDelayMs'?: number;
    /** Default: false. */
    'band14.dynamic'?: boolean;
    /** Default: true. */
    'band14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band14.slopeDbOct'?: number;
    /** Default: false. */
    'band14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band15.attackMs'?: number;
    /** Default: false. */
    'band15.autoThreshold'?: boolean;
    /** Default: false. */
    'band15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band15.detectorDelayMs'?: number;
    /** Default: false. */
    'band15.dynamic'?: boolean;
    /** Default: true. */
    'band15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band15.slopeDbOct'?: number;
    /** Default: false. */
    'band15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band16.attackMs'?: number;
    /** Default: false. */
    'band16.autoThreshold'?: boolean;
    /** Default: false. */
    'band16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band16.detectorDelayMs'?: number;
    /** Default: false. */
    'band16.dynamic'?: boolean;
    /** Default: true. */
    'band16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band16.slopeDbOct'?: number;
    /** Default: false. */
    'band16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band17.attackMs'?: number;
    /** Default: false. */
    'band17.autoThreshold'?: boolean;
    /** Default: false. */
    'band17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band17.detectorDelayMs'?: number;
    /** Default: false. */
    'band17.dynamic'?: boolean;
    /** Default: true. */
    'band17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band17.slopeDbOct'?: number;
    /** Default: false. */
    'band17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band18.attackMs'?: number;
    /** Default: false. */
    'band18.autoThreshold'?: boolean;
    /** Default: false. */
    'band18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band18.detectorDelayMs'?: number;
    /** Default: false. */
    'band18.dynamic'?: boolean;
    /** Default: true. */
    'band18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band18.slopeDbOct'?: number;
    /** Default: false. */
    'band18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band19.attackMs'?: number;
    /** Default: false. */
    'band19.autoThreshold'?: boolean;
    /** Default: false. */
    'band19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band19.detectorDelayMs'?: number;
    /** Default: false. */
    'band19.dynamic'?: boolean;
    /** Default: true. */
    'band19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band19.slopeDbOct'?: number;
    /** Default: false. */
    'band19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band2.attackMs'?: number;
    /** Default: false. */
    'band2.autoThreshold'?: boolean;
    /** Default: false. */
    'band2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band2.detectorDelayMs'?: number;
    /** Default: false. */
    'band2.dynamic'?: boolean;
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band2.slopeDbOct'?: number;
    /** Default: false. */
    'band2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band20.attackMs'?: number;
    /** Default: false. */
    'band20.autoThreshold'?: boolean;
    /** Default: false. */
    'band20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band20.detectorDelayMs'?: number;
    /** Default: false. */
    'band20.dynamic'?: boolean;
    /** Default: true. */
    'band20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band20.slopeDbOct'?: number;
    /** Default: false. */
    'band20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band21.attackMs'?: number;
    /** Default: false. */
    'band21.autoThreshold'?: boolean;
    /** Default: false. */
    'band21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band21.detectorDelayMs'?: number;
    /** Default: false. */
    'band21.dynamic'?: boolean;
    /** Default: true. */
    'band21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band21.slopeDbOct'?: number;
    /** Default: false. */
    'band21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band22.attackMs'?: number;
    /** Default: false. */
    'band22.autoThreshold'?: boolean;
    /** Default: false. */
    'band22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band22.detectorDelayMs'?: number;
    /** Default: false. */
    'band22.dynamic'?: boolean;
    /** Default: true. */
    'band22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band22.slopeDbOct'?: number;
    /** Default: false. */
    'band22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band23.attackMs'?: number;
    /** Default: false. */
    'band23.autoThreshold'?: boolean;
    /** Default: false. */
    'band23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band23.detectorDelayMs'?: number;
    /** Default: false. */
    'band23.dynamic'?: boolean;
    /** Default: true. */
    'band23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band23.slopeDbOct'?: number;
    /** Default: false. */
    'band23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band3.attackMs'?: number;
    /** Default: false. */
    'band3.autoThreshold'?: boolean;
    /** Default: false. */
    'band3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band3.detectorDelayMs'?: number;
    /** Default: false. */
    'band3.dynamic'?: boolean;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band3.slopeDbOct'?: number;
    /** Default: false. */
    'band3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band4.attackMs'?: number;
    /** Default: false. */
    'band4.autoThreshold'?: boolean;
    /** Default: false. */
    'band4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band4.detectorDelayMs'?: number;
    /** Default: false. */
    'band4.dynamic'?: boolean;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band4.slopeDbOct'?: number;
    /** Default: false. */
    'band4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band5.attackMs'?: number;
    /** Default: false. */
    'band5.autoThreshold'?: boolean;
    /** Default: false. */
    'band5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band5.detectorDelayMs'?: number;
    /** Default: false. */
    'band5.dynamic'?: boolean;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band5.slopeDbOct'?: number;
    /** Default: false. */
    'band5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band6.attackMs'?: number;
    /** Default: false. */
    'band6.autoThreshold'?: boolean;
    /** Default: false. */
    'band6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band6.detectorDelayMs'?: number;
    /** Default: false. */
    'band6.dynamic'?: boolean;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band6.slopeDbOct'?: number;
    /** Default: false. */
    'band6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band7.attackMs'?: number;
    /** Default: false. */
    'band7.autoThreshold'?: boolean;
    /** Default: false. */
    'band7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band7.detectorDelayMs'?: number;
    /** Default: false. */
    'band7.dynamic'?: boolean;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band7.slopeDbOct'?: number;
    /** Default: false. */
    'band7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band8.attackMs'?: number;
    /** Default: false. */
    'band8.autoThreshold'?: boolean;
    /** Default: false. */
    'band8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band8.detectorDelayMs'?: number;
    /** Default: false. */
    'band8.dynamic'?: boolean;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band8.slopeDbOct'?: number;
    /** Default: false. */
    'band8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band9.attackMs'?: number;
    /** Default: false. */
    'band9.autoThreshold'?: boolean;
    /** Default: false. */
    'band9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band9.detectorDelayMs'?: number;
    /** Default: false. */
    'band9.dynamic'?: boolean;
    /** Default: true. */
    'band9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band9.slopeDbOct'?: number;
    /** Default: false. */
    'band9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
  };
  /** Parameters of the `eq.parametric` processor. */
  'eq.parametric': {
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band0.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band1.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band2.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band3.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band3.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band4.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band4.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band5.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band5.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band6.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band6.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band7.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band7.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band8.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band8.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band8.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band9.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band9.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band9.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band10.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band10.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band10.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band11.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band11.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band11.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band12.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band12.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band12.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band13.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band13.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band13.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band14.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band14.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band14.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band15.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band15.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band15.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band16.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band16.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band16.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band17.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band17.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band17.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band18.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band18.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band18.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band19.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band19.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band19.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band20.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band20.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band20.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band21.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band21.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band21.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band22.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band22.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band22.q'?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 1000. */
    'band23.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band23.gainDb'?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    'band23.q'?: number;
    /** Unit: ms. Default: 5. */
    'band0.attackMs'?: number;
    /** Default: false. */
    'band0.autoThreshold'?: boolean;
    /** Default: false. */
    'band0.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band0.detectorDelayMs'?: number;
    /** Default: false. */
    'band0.dynamic'?: boolean;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band0.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band0.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band0.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band0.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band0.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band0.slopeDbOct'?: number;
    /** Default: false. */
    'band0.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band1.attackMs'?: number;
    /** Default: false. */
    'band1.autoThreshold'?: boolean;
    /** Default: false. */
    'band1.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band1.detectorDelayMs'?: number;
    /** Default: false. */
    'band1.dynamic'?: boolean;
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band1.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band1.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band1.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band1.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band1.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band1.slopeDbOct'?: number;
    /** Default: false. */
    'band1.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band10.attackMs'?: number;
    /** Default: false. */
    'band10.autoThreshold'?: boolean;
    /** Default: false. */
    'band10.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band10.detectorDelayMs'?: number;
    /** Default: false. */
    'band10.dynamic'?: boolean;
    /** Default: true. */
    'band10.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band10.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band10.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band10.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band10.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band10.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band10.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band10.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band10.slopeDbOct'?: number;
    /** Default: false. */
    'band10.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band10.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band11.attackMs'?: number;
    /** Default: false. */
    'band11.autoThreshold'?: boolean;
    /** Default: false. */
    'band11.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band11.detectorDelayMs'?: number;
    /** Default: false. */
    'band11.dynamic'?: boolean;
    /** Default: true. */
    'band11.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band11.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band11.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band11.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band11.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band11.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band11.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band11.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band11.slopeDbOct'?: number;
    /** Default: false. */
    'band11.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band11.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band12.attackMs'?: number;
    /** Default: false. */
    'band12.autoThreshold'?: boolean;
    /** Default: false. */
    'band12.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band12.detectorDelayMs'?: number;
    /** Default: false. */
    'band12.dynamic'?: boolean;
    /** Default: true. */
    'band12.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band12.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band12.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band12.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band12.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band12.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band12.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band12.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band12.slopeDbOct'?: number;
    /** Default: false. */
    'band12.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band12.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band13.attackMs'?: number;
    /** Default: false. */
    'band13.autoThreshold'?: boolean;
    /** Default: false. */
    'band13.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band13.detectorDelayMs'?: number;
    /** Default: false. */
    'band13.dynamic'?: boolean;
    /** Default: true. */
    'band13.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band13.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band13.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band13.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band13.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band13.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band13.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band13.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band13.slopeDbOct'?: number;
    /** Default: false. */
    'band13.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band13.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band14.attackMs'?: number;
    /** Default: false. */
    'band14.autoThreshold'?: boolean;
    /** Default: false. */
    'band14.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band14.detectorDelayMs'?: number;
    /** Default: false. */
    'band14.dynamic'?: boolean;
    /** Default: true. */
    'band14.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band14.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band14.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band14.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band14.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band14.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band14.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band14.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band14.slopeDbOct'?: number;
    /** Default: false. */
    'band14.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band14.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band15.attackMs'?: number;
    /** Default: false. */
    'band15.autoThreshold'?: boolean;
    /** Default: false. */
    'band15.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band15.detectorDelayMs'?: number;
    /** Default: false. */
    'band15.dynamic'?: boolean;
    /** Default: true. */
    'band15.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band15.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band15.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band15.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band15.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band15.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band15.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band15.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band15.slopeDbOct'?: number;
    /** Default: false. */
    'band15.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band15.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band16.attackMs'?: number;
    /** Default: false. */
    'band16.autoThreshold'?: boolean;
    /** Default: false. */
    'band16.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band16.detectorDelayMs'?: number;
    /** Default: false. */
    'band16.dynamic'?: boolean;
    /** Default: true. */
    'band16.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band16.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band16.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band16.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band16.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band16.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band16.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band16.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band16.slopeDbOct'?: number;
    /** Default: false. */
    'band16.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band16.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band17.attackMs'?: number;
    /** Default: false. */
    'band17.autoThreshold'?: boolean;
    /** Default: false. */
    'band17.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band17.detectorDelayMs'?: number;
    /** Default: false. */
    'band17.dynamic'?: boolean;
    /** Default: true. */
    'band17.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band17.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band17.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band17.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band17.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band17.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band17.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band17.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band17.slopeDbOct'?: number;
    /** Default: false. */
    'band17.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band17.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band18.attackMs'?: number;
    /** Default: false. */
    'band18.autoThreshold'?: boolean;
    /** Default: false. */
    'band18.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band18.detectorDelayMs'?: number;
    /** Default: false. */
    'band18.dynamic'?: boolean;
    /** Default: true. */
    'band18.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band18.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band18.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band18.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band18.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band18.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band18.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band18.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band18.slopeDbOct'?: number;
    /** Default: false. */
    'band18.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band18.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band19.attackMs'?: number;
    /** Default: false. */
    'band19.autoThreshold'?: boolean;
    /** Default: false. */
    'band19.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band19.detectorDelayMs'?: number;
    /** Default: false. */
    'band19.dynamic'?: boolean;
    /** Default: true. */
    'band19.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band19.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band19.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band19.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band19.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band19.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band19.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band19.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band19.slopeDbOct'?: number;
    /** Default: false. */
    'band19.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band19.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band2.attackMs'?: number;
    /** Default: false. */
    'band2.autoThreshold'?: boolean;
    /** Default: false. */
    'band2.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band2.detectorDelayMs'?: number;
    /** Default: false. */
    'band2.dynamic'?: boolean;
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band2.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band2.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band2.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band2.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band2.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band2.slopeDbOct'?: number;
    /** Default: false. */
    'band2.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band20.attackMs'?: number;
    /** Default: false. */
    'band20.autoThreshold'?: boolean;
    /** Default: false. */
    'band20.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band20.detectorDelayMs'?: number;
    /** Default: false. */
    'band20.dynamic'?: boolean;
    /** Default: true. */
    'band20.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band20.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band20.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band20.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band20.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band20.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band20.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band20.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band20.slopeDbOct'?: number;
    /** Default: false. */
    'band20.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band20.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band21.attackMs'?: number;
    /** Default: false. */
    'band21.autoThreshold'?: boolean;
    /** Default: false. */
    'band21.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band21.detectorDelayMs'?: number;
    /** Default: false. */
    'band21.dynamic'?: boolean;
    /** Default: true. */
    'band21.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band21.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band21.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band21.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band21.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band21.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band21.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band21.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band21.slopeDbOct'?: number;
    /** Default: false. */
    'band21.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band21.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band22.attackMs'?: number;
    /** Default: false. */
    'band22.autoThreshold'?: boolean;
    /** Default: false. */
    'band22.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band22.detectorDelayMs'?: number;
    /** Default: false. */
    'band22.dynamic'?: boolean;
    /** Default: true. */
    'band22.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band22.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band22.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band22.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band22.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band22.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band22.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band22.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band22.slopeDbOct'?: number;
    /** Default: false. */
    'band22.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band22.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band23.attackMs'?: number;
    /** Default: false. */
    'band23.autoThreshold'?: boolean;
    /** Default: false. */
    'band23.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band23.detectorDelayMs'?: number;
    /** Default: false. */
    'band23.dynamic'?: boolean;
    /** Default: true. */
    'band23.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band23.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band23.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band23.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band23.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band23.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band23.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band23.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band23.slopeDbOct'?: number;
    /** Default: false. */
    'band23.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band23.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band3.attackMs'?: number;
    /** Default: false. */
    'band3.autoThreshold'?: boolean;
    /** Default: false. */
    'band3.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band3.detectorDelayMs'?: number;
    /** Default: false. */
    'band3.dynamic'?: boolean;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band3.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band3.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band3.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band3.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band3.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band3.slopeDbOct'?: number;
    /** Default: false. */
    'band3.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band4.attackMs'?: number;
    /** Default: false. */
    'band4.autoThreshold'?: boolean;
    /** Default: false. */
    'band4.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band4.detectorDelayMs'?: number;
    /** Default: false. */
    'band4.dynamic'?: boolean;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band4.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band4.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band4.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band4.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band4.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band4.slopeDbOct'?: number;
    /** Default: false. */
    'band4.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band5.attackMs'?: number;
    /** Default: false. */
    'band5.autoThreshold'?: boolean;
    /** Default: false. */
    'band5.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band5.detectorDelayMs'?: number;
    /** Default: false. */
    'band5.dynamic'?: boolean;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band5.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band5.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band5.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band5.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band5.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band5.slopeDbOct'?: number;
    /** Default: false. */
    'band5.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band6.attackMs'?: number;
    /** Default: false. */
    'band6.autoThreshold'?: boolean;
    /** Default: false. */
    'band6.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band6.detectorDelayMs'?: number;
    /** Default: false. */
    'band6.dynamic'?: boolean;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band6.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band6.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band6.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band6.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band6.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band6.slopeDbOct'?: number;
    /** Default: false. */
    'band6.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band7.attackMs'?: number;
    /** Default: false. */
    'band7.autoThreshold'?: boolean;
    /** Default: false. */
    'band7.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band7.detectorDelayMs'?: number;
    /** Default: false. */
    'band7.dynamic'?: boolean;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band7.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band7.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band7.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band7.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band7.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band7.slopeDbOct'?: number;
    /** Default: false. */
    'band7.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band8.attackMs'?: number;
    /** Default: false. */
    'band8.autoThreshold'?: boolean;
    /** Default: false. */
    'band8.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band8.detectorDelayMs'?: number;
    /** Default: false. */
    'band8.dynamic'?: boolean;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band8.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band8.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band8.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band8.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band8.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band8.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band8.slopeDbOct'?: number;
    /** Default: false. */
    'band8.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band8.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Default: 5. */
    'band9.attackMs'?: number;
    /** Default: false. */
    'band9.autoThreshold'?: boolean;
    /** Default: false. */
    'band9.bypassed'?: boolean;
    /** Default: 'rbj'. */
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    /** Unit: ms. Default: 0. */
    'band9.detectorDelayMs'?: number;
    /** Default: false. */
    'band9.dynamic'?: boolean;
    /** Default: true. */
    'band9.enabled'?: boolean;
    /** Default: 'inherit'. */
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    /** Default: 'stereo'. */
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    /** Default: false. */
    'band9.proportionalQ'?: boolean;
    /** Default: 0.03. */
    'band9.proportionalQStrength'?: number;
    /** Unit: dB. Default: -6. */
    'band9.rangeDb'?: number;
    /** Unit: ratio. Default: 2. */
    'band9.ratio'?: number;
    /** Unit: ms. Default: 50. */
    'band9.releaseMs'?: number;
    /** Unit: Hz. Default: -1. */
    'band9.sidechainFreqHz'?: number;
    /** Default: 1. */
    'band9.sidechainQ'?: number;
    /** Unit: dB/oct. Default: 12. */
    'band9.slopeDbOct'?: number;
    /** Default: false. */
    'band9.soloed'?: boolean;
    /** Unit: dB. Default: -24. */
    'band9.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
  };
  /** Parameters of the `eq.passiveTube` processor. */
  'eq.passiveTube': {
    /** Unit: Hz. Range: (0, inf). Default: 60. */
    lowFrequencyHz?: number;
    /** Default: 0. */
    lowBoost?: number;
    /** Default: 0. */
    lowAttenuation?: number;
    /** Unit: Hz. Range: (0, inf). Default: 8000. */
    highBoostFrequencyHz?: number;
    /** Default: 0. */
    highBoost?: number;
    /** Default: 0.5. */
    highBandwidth?: number;
    /** Unit: Hz. Range: (0, inf). Default: 10000. */
    highAttenuationFrequencyHz?: number;
    /** Default: 0. */
    highAttenuation?: number;
    /** Range: [0, inf). Default: 0. */
    outputDrive?: number;
    /** Default: 'curveOnly'. */
    componentModel?: 'curveOnly' | 'wdfCircuit';
  };
  /** Parameters of the `eq.shelving` processor. */
  'eq.shelving': {
    /** Unit: Hz. Range: (0, Nyquist). Default: 100. */
    lowFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    lowGainDb?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    lowQ?: number;
    /** Unit: Hz. Range: (0, Nyquist). Default: 10000. */
    highFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    highGainDb?: number;
    /** Range: (0, inf). Default: 0.70710677. */
    highQ?: number;
    /** Default: true. */
    highEnabled?: boolean;
    /** Default: true. */
    lowEnabled?: boolean;
  };
  /** Parameters of the `eq.steppedProportionalQ` processor. */
  'eq.steppedProportionalQ': {
    /** Unit: Hz. Default: 100. */
    lowFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    lowGainDb?: number;
    /** Unit: Hz. Default: 400. */
    lowMidFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    lowMidGainDb?: number;
    /** Unit: Hz. Default: 3000. */
    highMidFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    highMidGainDb?: number;
    /** Unit: Hz. Default: 10000. */
    highFrequencyHz?: number;
    /** Unit: dB. Default: 0. */
    highGainDb?: number;
  };
  /** Parameters of the `eq.tilt` processor. */
  'eq.tilt': {
    /** Unit: dB. Default: 0. */
    tiltDb?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    pivotHz?: number;
  };
  /** Parameters of the `final.bitDepth` processor. */
  'final.bitDepth': Record<string, never>;
  /** Parameters of the `final.dither` processor. */
  'final.dither': Record<string, never>;
  /** Parameters of the `final.outputChain` processor. */
  'final.outputChain': Record<string, never>;
  /** Parameters of the `match.abCrossfade` processor. */
  'match.abCrossfade': Record<string, never>;
  /** Parameters of the `match.abSwitch` processor. */
  'match.abSwitch': Record<string, never>;
  /** Parameters of the `match.alignReferenceToSource` processor. */
  'match.alignReferenceToSource': Record<string, never>;
  /** Parameters of the `match.applyMatchEq` processor. */
  'match.applyMatchEq': Record<string, never>;
  /** Parameters of the `maximizer.adaptiveRelease` processor. */
  'maximizer.adaptiveRelease': {
    /** Unit: dBTP. Range: (-inf, 0]. Default: -1. */
    ceilingDbtp?: number;
    /** Unit: ms. Range: [0, inf). Default: 20. */
    minReleaseMs?: number;
    /** Unit: ms. Default: 250. */
    maxReleaseMs?: number;
    /** Unit: ms. Range: (0, inf). Default: 30. */
    crestWindowMs?: number;
    /** Range: (0, inf). Default: 2. */
    crestLow?: number;
    /** Default: 10. */
    crestHigh?: number;
    /** Unit: ms. Range: [0, inf). Default: 20. */
    releaseSmoothingMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    lookaheadMs?: number;
  };
  /** Parameters of the `maximizer.loudnessOptimize` processor. */
  'maximizer.loudnessOptimize': Record<string, never>;
  /** Parameters of the `maximizer.maximizer` processor. */
  'maximizer.maximizer': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    inputGainDb?: number;
    /** Unit: dBFS. Range: (-inf, 0]. Default: -1. */
    ceilingDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    releaseMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    lookaheadMs?: number;
  };
  /** Parameters of the `maximizer.softKneeMax` processor. */
  'maximizer.softKneeMax': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    inputGainDb?: number;
    /** Unit: dBFS. Range: (-inf, 0]. Default: -1. */
    ceilingDb?: number;
    /** Unit: dB. Range: [0, inf). Default: 6. */
    kneeDb?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    releaseMs?: number;
  };
  /** Parameters of the `maximizer.truePeakLimiter` processor. */
  'maximizer.truePeakLimiter': {
    /** Unit: dBTP. Range: (-inf, 0]. Default: -1. */
    ceilingDbtp?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    releaseMs?: number;
    /** Default: false. */
    applyGainAtInputRate?: boolean;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    lookaheadMs?: number;
    /** Unit: ratio. Default: 4. */
    oversampleFactor?: number;
  };
  /** Parameters of the `multiband.compressor` processor. */
  'multiband.compressor': {
    /** Unit: dB. Default: -18. */
    'band0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band0.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band0.makeupGainDb'?: number;
    /** Unit: dB. Default: -18. */
    'band1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band1.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band1.makeupGainDb'?: number;
    /** Unit: dB. Default: -18. */
    'band2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band2.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band2.makeupGainDb'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band0.kneeDb'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band1.kneeDb'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band2.kneeDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band3.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band3.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band3.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band3.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band3.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band4.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band4.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band4.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band4.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band4.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band5.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band5.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band5.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band5.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band5.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band6.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band6.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band6.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band6.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band6.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band7.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band7.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band7.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band7.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band7.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 10. */
    'band8.attackMs'?: number;
    /** Unit: dB. Range: [0, inf). Default: 0. */
    'band8.kneeDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band8.makeupGainDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band8.releaseMs'?: number;
    /** Unit: dB. Default: -18. */
    'band8.thresholdDb'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `multiband.dynamicEq` processor. */
  'multiband.dynamicEq': {
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn0.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn0.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn0.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn0.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn0.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn0.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn0.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn0.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn1.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn1.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn1.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn1.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn1.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn1.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn1.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn1.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn2.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn2.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn2.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn2.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn2.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn2.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn2.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn2.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn3.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn3.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn3.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn3.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn3.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn3.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn3.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn3.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn3.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn4.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn4.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn4.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn4.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn4.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn4.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn4.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn4.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn4.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn5.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn5.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn5.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn5.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn5.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn5.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn5.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn5.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn5.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn6.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn6.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn6.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn6.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn6.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn6.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn6.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn6.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn6.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band0.dyn7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band0.dyn7.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn7.q'?: number;
    /** Unit: dB. Default: -24. */
    'band0.dyn7.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.dyn7.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band0.dyn7.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.dyn7.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band0.dyn7.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.dyn7.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.dyn7.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn0.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn0.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn0.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn0.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn0.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn0.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn0.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn0.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn1.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn1.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn1.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn1.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn1.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn1.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn1.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn1.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn2.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn2.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn2.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn2.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn2.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn2.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn2.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn2.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn3.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn3.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn3.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn3.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn3.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn3.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn3.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn3.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn3.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn4.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn4.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn4.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn4.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn4.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn4.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn4.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn4.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn4.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn5.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn5.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn5.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn5.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn5.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn5.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn5.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn5.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn5.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn6.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn6.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn6.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn6.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn6.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn6.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn6.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn6.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn6.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band1.dyn7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band1.dyn7.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn7.q'?: number;
    /** Unit: dB. Default: -24. */
    'band1.dyn7.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.dyn7.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band1.dyn7.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.dyn7.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band1.dyn7.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.dyn7.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.dyn7.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn0.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn0.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn0.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn0.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn0.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn0.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn0.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn0.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn0.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn1.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn1.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn1.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn1.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn1.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn1.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn1.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn1.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn1.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn2.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn2.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn2.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn2.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn2.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn2.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn2.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn2.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn2.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn3.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn3.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn3.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn3.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn3.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn3.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn3.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn3.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn3.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn3.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn4.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn4.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn4.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn4.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn4.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn4.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn4.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn4.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn4.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn4.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn5.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn5.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn5.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn5.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn5.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn5.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn5.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn5.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn5.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn5.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn6.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn6.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn6.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn6.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn6.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn6.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn6.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn6.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn6.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn6.detectorDelayMs'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band2.dyn7.frequencyHz'?: number;
    /** Unit: dB. Default: 0. */
    'band2.dyn7.staticGainDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn7.q'?: number;
    /** Unit: dB. Default: -24. */
    'band2.dyn7.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.dyn7.ratio'?: number;
    /** Unit: dB. Default: -6. */
    'band2.dyn7.rangeDb'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.dyn7.sidechainQ'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band2.dyn7.sidechainFreqHz'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.dyn7.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band0.dyn0.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn1.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn2.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn3.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn4.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn5.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn6.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band0.dyn7.enabled'?: boolean;
    /** Default: 'peak'. */
    'band0.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn0.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn1.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn2.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn3.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn4.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn5.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn6.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band1.dyn7.enabled'?: boolean;
    /** Default: 'peak'. */
    'band1.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn0.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn1.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn2.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn3.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn4.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn5.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn6.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Default: true. */
    'band2.dyn7.enabled'?: boolean;
    /** Default: 'peak'. */
    'band2.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band3.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band3.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band3.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band3.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band3.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band3.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band3.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band3.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band4.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band4.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band4.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band4.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band4.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band4.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band4.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band4.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band5.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band5.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band5.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band5.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band5.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band5.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band5.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band5.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band6.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band6.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band6.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band6.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band6.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band6.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band6.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band6.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band7.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band7.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band7.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band7.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band7.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band7.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band7.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band7.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn0.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn0.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn0.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn0.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn0.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn0.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn0.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn0.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn0.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn0.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn1.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn1.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn1.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn1.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn1.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn1.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn1.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn1.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn1.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn1.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn2.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn2.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn2.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn2.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn2.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn2.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn2.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn2.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn2.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn2.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn3.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn3.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn3.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn3.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn3.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn3.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn3.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn3.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn3.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn4.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn4.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn4.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn4.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn4.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn4.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn4.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn4.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn4.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn5.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn5.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn5.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn5.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn5.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn5.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn5.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn5.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn5.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn6.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn6.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn6.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn6.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn6.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn6.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn6.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn6.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn6.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.dyn7.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band8.dyn7.detectorDelayMs'?: number;
    /** Default: true. */
    'band8.dyn7.enabled'?: boolean;
    /** Unit: Hz. Range: (0, inf). Default: 1000. */
    'band8.dyn7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn7.q'?: number;
    /** Unit: dB. Default: -6. */
    'band8.dyn7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.dyn7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.dyn7.releaseMs'?: number;
    /** Unit: Hz. Range: [-1, inf). Default: -1. */
    'band8.dyn7.sidechainFreqHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.dyn7.sidechainQ'?: number;
    /** Unit: dB. Default: 0. */
    'band8.dyn7.staticGainDb'?: number;
    /** Unit: dB. Default: -24. */
    'band8.dyn7.thresholdDb'?: number;
    /** Default: 'peak'. */
    'band8.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `multiband.expander` processor. */
  'multiband.expander': {
    /** Unit: dB. Default: -40. */
    'band0.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band0.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band0.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band0.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band0.rangeDb'?: number;
    /** Unit: dB. Default: -40. */
    'band1.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band1.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band1.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band1.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band1.rangeDb'?: number;
    /** Unit: dB. Default: -40. */
    'band2.thresholdDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band2.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band2.attackMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band2.releaseMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band2.rangeDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band3.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band3.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band3.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band3.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band3.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band4.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band4.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band4.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band4.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band4.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band5.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band5.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band5.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band5.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band5.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band6.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band6.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band6.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band6.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band6.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band7.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band7.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band7.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band7.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band7.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 5. */
    'band8.attackMs'?: number;
    /** Unit: dB. Range: (-inf, 0]. Default: -60. */
    'band8.rangeDb'?: number;
    /** Unit: ratio. Range: [1, inf). Default: 2. */
    'band8.ratio'?: number;
    /** Unit: ms. Range: [0, inf). Default: 100. */
    'band8.releaseMs'?: number;
    /** Unit: dB. Default: -40. */
    'band8.thresholdDb'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `multiband.imager` processor. */
  'multiband.imager': {
    /** Range: [0, inf). Default: 1. */
    'band0.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band0.decorrelationAmount'?: number;
    /** Range: [0, inf). Default: 1. */
    'band1.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band1.decorrelationAmount'?: number;
    /** Range: [0, inf). Default: 1. */
    'band2.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band2.decorrelationAmount'?: number;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: true. */
    'band0.preserveEnergy'?: boolean;
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: true. */
    'band1.preserveEnergy'?: boolean;
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: true. */
    'band2.preserveEnergy'?: boolean;
    /** Range: [0, 1]. Default: 0. */
    'band3.decorrelationAmount'?: number;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Default: true. */
    'band3.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band3.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band4.decorrelationAmount'?: number;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Default: true. */
    'band4.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band4.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band5.decorrelationAmount'?: number;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Default: true. */
    'band5.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band5.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band6.decorrelationAmount'?: number;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Default: true. */
    'band6.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band6.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band7.decorrelationAmount'?: number;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Default: true. */
    'band7.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band7.width'?: number;
    /** Range: [0, 1]. Default: 0. */
    'band8.decorrelationAmount'?: number;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Default: true. */
    'band8.preserveEnergy'?: boolean;
    /** Range: [0, inf). Default: 1. */
    'band8.width'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `multiband.limiter` processor. */
  'multiband.limiter': {
    /** Unit: dB. Default: -1. */
    'band0.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band0.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band1.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band1.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band2.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band2.releaseMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band0.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band1.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 0. */
    'band2.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band3.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band3.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band3.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band4.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band4.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band4.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band5.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band5.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band5.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band6.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band6.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band6.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band7.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band7.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band7.thresholdDb'?: number;
    /** Unit: ms. Range: [0, inf). Default: 1. */
    'band8.lookaheadMs'?: number;
    /** Unit: ms. Range: [0, inf). Default: 50. */
    'band8.releaseMs'?: number;
    /** Unit: dB. Default: -1. */
    'band8.thresholdDb'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `multiband.saturation` processor. */
  'multiband.saturation': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band0.driveDb'?: number;
    /** Range: [0, 1]. Default: 1. */
    'band0.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band0.outputGainDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band1.driveDb'?: number;
    /** Range: [0, 1]. Default: 1. */
    'band1.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band1.outputGainDb'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band2.driveDb'?: number;
    /** Range: [0, 1]. Default: 1. */
    'band2.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band2.outputGainDb'?: number;
    /** Default: true. */
    'band0.enabled'?: boolean;
    /** Default: 'softClip'. */
    'band0.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Default: true. */
    'band1.enabled'?: boolean;
    /** Default: 'softClip'. */
    'band1.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Default: true. */
    'band2.enabled'?: boolean;
    /** Default: 'softClip'. */
    'band2.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band3.driveDb'?: number;
    /** Default: true. */
    'band3.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band3.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band3.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band3.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band4.driveDb'?: number;
    /** Default: true. */
    'band4.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band4.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band4.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band4.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band5.driveDb'?: number;
    /** Default: true. */
    'band5.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band5.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band5.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band5.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band6.driveDb'?: number;
    /** Default: true. */
    'band6.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band6.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band6.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band6.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band7.driveDb'?: number;
    /** Default: true. */
    'band7.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band7.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band7.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band7.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band8.driveDb'?: number;
    /** Default: true. */
    'band8.enabled'?: boolean;
    /** Range: [0, 1]. Default: 1. */
    'band8.mix'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    'band8.outputGainDb'?: number;
    /** Default: 'softClip'. */
    'band8.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `repair.declick` processor. */
  'repair.declick': {
    /** Unit: count. Range: [0, 2048]. Default: 20. */
    lpcOrder?: number;
    /** Unit: samples. Range: [1, 512]. Default: 8. */
    maxClickSamples?: number;
    /** Unit: ratio. Range: (0, 100]. Default: 4. */
    neighborRatio?: number;
    /** Unit: ratio. Range: (0, 1000]. Default: 8. */
    residualRatio?: number;
    /** Range: (0, 10]. Default: 0.8. */
    threshold?: number;
  };
  /** Parameters of the `repair.declip` processor. */
  'repair.declip': {
    /** Unit: dBFS. Range: [-890, 0]. Default: -0.17547831. */
    clipThresholdDb?: number;
    /** Unit: count. Range: [1, 8]. Default: 2. */
    iterations?: number;
    /** Default: false. */
    linkChannels?: boolean;
    /** Unit: count. Range: [0, 2048]. Default: 36. */
    lpcOrder?: number;
  };
  /** Parameters of the `repair.decrackle` processor. */
  'repair.decrackle': {
    /** Unit: count. Range: [1, 24]. Default: 4. */
    levels?: number;
    /** Default: 'median'. */
    mode?: 'median';
    /** Range: (0, 1000]. Default: 0.4. */
    threshold?: number;
  };
  /** Parameters of the `repair.dehum` processor. */
  'repair.dehum': {
    /** Range: [0, 1]. Default: 0.25. */
    adaptation?: number;
    /** Default: false. */
    adaptive?: boolean;
    /** Unit: samples. Range: [16, 16384]. Default: 2048. */
    frameSize?: number;
    /** Unit: Hz. Range: (0, 5000]. Default: 50. */
    fundamentalHz?: number;
    /** Unit: count. Range: [1, 16]. Default: 4. */
    harmonics?: number;
    /** Default: 'subtract'. */
    mode?: 'subtract' | 'notch';
    /** Range: [0, 1]. Default: 0.01. */
    pllBandwidth?: number;
    /** Range: (0, 100]. Default: 20. */
    q?: number;
    /** Unit: Hz. Range: [0, 100]. Default: 2. */
    searchRangeHz?: number;
  };
  /** Parameters of the `repair.denoiseClassical` processor. */
  'repair.denoiseClassical': {
    /** Range: [0, 1). Default: 0.98. */
    ddAlpha?: number;
    /** Default: true. */
    gainSmoothing?: boolean;
    /** Unit: samples. Range: [1, inf). Default: 256. */
    hopLength?: number;
    /** Default: 'logMmse'. */
    mode?: 'logMmse' | 'mmseStsa' | 'spectralSubtraction';
    /** Unit: samples. Default: 1024. */
    nFft?: number;
    /** Range: (0, 1]. Default: 0.1. */
    noiseEstimationQuantile?: number;
    /** Default: 'spp'. */
    noiseEstimator?: 'mcra' | 'imcra' | 'spp';
    /** Unit: ratio. Range: [0, 16]. Default: 2. */
    overSubtraction?: number;
    /** Unit: dB. Range: [0, 120]. Default: 26. */
    reductionDb?: number;
    /** Range: [0, 1]. Default: 0.05. */
    spectralFloor?: number;
    /** Default: true. */
    speechPresenceGain?: boolean;
  };
  /** Parameters of the `repair.dereverbClassical` processor. */
  'repair.dereverbClassical': {
    /** Range: [0, 1]. Default: 1. */
    attenuation?: number;
    /** Unit: samples. Range: [1, inf). Default: 256. */
    hopLength?: number;
    /** Unit: ms. Range: [0, 500]. Default: 50. */
    lateDelayMs?: number;
    /** Unit: samples. Default: 1024. */
    nFft?: number;
    /** Unit: ratio. Range: [0, 16]. Default: 1. */
    overSubtraction?: number;
    /** Range: [0, 1]. Default: 0.08. */
    spectralFloor?: number;
    /** Unit: s. Range: (0, 10]. Default: 0.4. */
    t60Sec?: number;
    /** Range: [0, 1]. Default: 0. */
    threshold?: number;
    /** Default: false. */
    wpeEnabled?: boolean;
    /** Unit: count. Range: [1, 16]. Default: 2. */
    wpeIterations?: number;
    /** Range: [0, 1]. Default: 0.7. */
    wpeStrength?: number;
    /** Unit: count. Range: [1, 32]. Default: 3. */
    wpeTaps?: number;
  };
  /** Parameters of the `repair.trimSilence` processor. */
  'repair.trimSilence': {
    /** Unit: LUFS. Range: [-144, 0]. Default: -60. */
    gateLufs?: number;
    /** Default: 'peak'. */
    mode?: 'peak' | 'lufsGated';
    /** Unit: samples. Range: [0, 960000]. Default: 0. */
    paddingSamples?: number;
    /** Unit: dBFS. Range: [-890, 0]. Default: -60. */
    thresholdDb?: number;
    /** Unit: ms. Range: (0, 10000]. Default: 400. */
    windowMs?: number;
  };
  /** Parameters of the `saturation.ampSim` processor. */
  'saturation.ampSim': {
    /** Default: 0.5. */
    drive?: number;
    /** Unit: dB. Default: 0. */
    bassDb?: number;
    /** Unit: dB. Default: 0. */
    midDb?: number;
    /** Unit: dB. Default: 0. */
    trebleDb?: number;
    /** Unit: dB. Default: 0. */
    presenceDb?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    levelDb?: number;
    /** Default: 0. */
    power?: number;
    /** Default: 0. */
    sag?: number;
    /** Default: 0. */
    transformer?: number;
    /** Default: 0. */
    nfb?: number;
    /** Default: 0. */
    micAxis?: number;
    /** Default: 0. */
    micBAxis?: number;
    /** Default: 0. */
    micBlend?: number;
    /** Default: 0. */
    cone?: number;
    /** Default: 0. */
    crossover?: number;
    /** Default: 0. */
    biasShift?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    inputDb?: number;
    /** Default: true. */
    cab?: boolean;
    /** Default: 'guitar4x12'. */
    cabModel?: 'guitar4x12' | 'bass8x10' | 'guitar1x12Combo' | 'guitar2x12Open';
    /** Default: 'classicCrunch'. */
    ampModel?:
      | 'classicCrunch'
      | 'cleanCombo'
      | 'modernHiGain'
      | 'vintageCombo'
      | 'classAChime'
      | 'heavyHiGain';
    /** Default: true. */
    cabIrDrivers?: boolean;
    /** Default: unset. */
    cabIrF32Base64?: string;
    /** Default: false. */
    cabIrGenerate?: boolean;
    /** Unit: Hz. Default: 0. */
    cabIrSampleRate?: number;
    /** Default: 0. */
    doppler?: number;
    /** Unit: cm. Default: 15. */
    micBDistanceCm?: number;
    /** Default: false. */
    micBInvert?: boolean;
    /** Default: 'none'. */
    micBModel?: 'none' | 'dynamic' | 'ribbon' | 'condenser';
    /** Unit: cm. Default: 2.5. */
    micDistanceCm?: number;
    /** Default: 'none'. */
    micModel?: 'none' | 'dynamic' | 'ribbon' | 'condenser';
    /** Default: '6l6'. */
    powerTube?: '6l6' | 'el34' | 'el84' | '6v6';
    /** Unit: count. Default: 2. */
    preampStages?: number;
    /** Default: unset. */
    preset?: string;
    /** Default: -1. */
    presetIndex?: number;
    /** Default: 'voiced'. */
    topology?: 'voiced' | 'circuit';
  };
  /** Parameters of the `saturation.bitcrusher` processor. */
  'saturation.bitcrusher': {
    /** Unit: bits. Range: [2, 24]. Default: 12. */
    bitDepth?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    holdHz?: number;
    /** Range: [0, 1]. Default: 0. */
    radioNoiseLevel?: number;
    /** Range: [0, 1]. Default: 0. */
    wpNoiseLevel?: number;
    /** Range: [0, 1]. Default: 0. */
    discNoiseLevel?: number;
    /** Range: [0, 1]. Default: 0. */
    humLevel?: number;
    /** Range: [0, 1]. Default: 1. */
    noiseDetune?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    noiseLpfHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    wpNoiseLpfHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    discNoiseLpfHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    humLpfHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    preFilterHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 0. */
    postFilterHz?: number;
    /** Unit: Hz. Range: [0, inf). Default: 50. */
    humHz?: number;
    /** Default: false. */
    wpNoisePink?: boolean;
    /** Default: 'lp'. */
    discType?: 'lp' | 'ep' | 'sp' | 'rnd';
    /** Default: 'lowPass'. */
    filterType?: 'off' | 'lowPass' | 'highPass';
    /** Default: false. */
    mono?: boolean;
    /** Range: [0, 9]. Default: 0. */
    typeLadder?: number;
    /** Default: 'crossfade'. */
    mixLaw?: 'crossfade' | 'twoRamps';
    /** Range: [0, inf). Default: 5351397. */
    ditherSeed?: number;
    /** Default: 'none'. */
    ditherType?: 'none' | 'rpdf' | 'tpdf' | 'noiseShaped';
    /** Unit: ratio. Range: [1, inf). Default: 1. */
    downsampleFactor?: number;
    /** Default: 'fixedDepth'. */
    quantizerMode?: 'fixedDepth' | 'off';
  };
  /** Parameters of the `saturation.distortion` processor. */
  'saturation.distortion': {
    /** Unit: dB. Range: [0, 60]. Default: 30. */
    gainDb?: number;
    /** Unit: Hz. Range: [475, 20000]. Default: 4000. */
    toneHz?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    levelDb?: number;
  };
  /** Parameters of the `saturation.exciter` processor. */
  'saturation.exciter': {
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    frequencyHz?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    driveDb?: number;
    /** Range: [0, inf). Default: 0.25. */
    amount?: number;
    /** Range: (0, inf). Default: 1. */
    q?: number;
    /** Range: [0, 1]. Default: 0.5. */
    evenOddMix?: number;
    /** Default: 'none'. */
    aliasing?: 'none' | 'oversample4x';
  };
  /** Parameters of the `saturation.hardClipper` processor. */
  'saturation.hardClipper': {
    /** Range: (0, inf). Default: 1. */
    ceiling?: number;
    /** Default: 'none'. */
    aliasing?: 'none' | 'adaa1' | 'adaa2' | 'oversample4x';
  };
  /** Parameters of the `saturation.multibandExciter` processor. */
  'saturation.multibandExciter': {
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band0.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band0.driveDb'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band0.amount'?: number;
    /** Range: (0, inf). Default: 1. */
    'band0.q'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band0.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band1.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band1.driveDb'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band1.amount'?: number;
    /** Range: (0, inf). Default: 1. */
    'band1.q'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band1.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band2.frequencyHz'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band2.driveDb'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band2.amount'?: number;
    /** Range: (0, inf). Default: 1. */
    'band2.q'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band2.evenOddMix'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band3.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band3.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band3.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band3.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band3.q'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band4.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band4.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band4.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band4.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band4.q'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band5.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band5.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band5.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band5.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band5.q'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band6.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band6.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band6.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band6.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band6.q'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band7.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band7.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band7.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band7.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band7.q'?: number;
    /** Range: [0, inf). Default: 0.25. */
    'band8.amount'?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    'band8.driveDb'?: number;
    /** Range: [0, 1]. Default: 0.5. */
    'band8.evenOddMix'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    'band8.frequencyHz'?: number;
    /** Range: (0, inf). Default: 1. */
    'band8.q'?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    cutoff0Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: 2000. */
    cutoff1Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff2Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff3Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff4Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff5Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff6Hz?: number;
    /** Unit: Hz. Range: (0, inf). Default: unset. */
    cutoff7Hz?: number;
    /** Unit: samples. Default: 513. */
    firKernelSize?: number;
    /** Default: 'linkwitzRiley'. */
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    /** Default: 'lr4'. */
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  /** Parameters of the `saturation.overdrive` processor. */
  'saturation.overdrive': {
    /** Unit: dB. Range: [0, 41]. Default: 20. */
    gainDb?: number;
    /** Unit: Hz. Range: [500, 8000]. Default: 3000. */
    toneHz?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    levelDb?: number;
  };
  /** Parameters of the `saturation.softClipper` processor. */
  'saturation.softClipper': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    driveDb?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
    /** Default: 'none'. */
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
    /** Range: (0, inf). Default: 1. */
    ceiling?: number;
  };
  /** Parameters of the `saturation.tape` processor. */
  'saturation.tape': {
    /** Unit: dB. Range: (-inf, 770]. Default: 3. */
    driveDb?: number;
    /** Range: [0, 1]. Default: 0.5. */
    saturation?: number;
    /** Range: [0, 1]. Default: 0.2. */
    hysteresis?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    outputGainDb?: number;
    /** Unit: in/s. Range: (0, inf). Default: 15. */
    speedIps?: number;
    /** Unit: dB. Range: [0, 770]. Default: 1.5. */
    headBumpDb?: number;
    /** Default: 0. */
    bias?: number;
    /** Range: [0, 1]. Default: 0.2. */
    gapLoss?: number;
    /** Unit: ratio. Default: 1. */
    oversampleFactor?: number;
  };
  /** Parameters of the `saturation.transformer` processor. */
  'saturation.transformer': {
    /** Unit: dB. Range: (-inf, 770]. Default: 4. */
    driveDb?: number;
    /** Range: [-1, 1]. Default: 0.1. */
    asymmetry?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
  };
  /** Parameters of the `saturation.tube` processor. */
  'saturation.tube': {
    /** Unit: dB. Range: (-inf, 770]. Default: 6. */
    driveDb?: number;
    /** Default: 0.15. */
    bias?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
    /** Unit: V. Default: -1.6. */
    biasV?: number;
    /** Range: [0, 1]. Default: 1. */
    harmonicDrive?: number;
    /** Unit: ratio. Default: 4. */
    oversampleFactor?: number;
  };
  /** Parameters of the `saturation.waveshaper` processor. */
  'saturation.waveshaper': {
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    driveDb?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    outputGainDb?: number;
    /** Default: 'none'. */
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
    /** Default: 0. */
    bias?: number;
    /** Default: 'tanh'. */
    curve?: 'tanh' | 'arctan' | 'asymmetric';
  };
  /** Parameters of the `spectral.airBand` processor. */
  'spectral.airBand': {
    /** Range: [0, 1]. Default: 0.25. */
    amount?: number;
    /** Unit: Hz. Range: (0, inf). Default: 12000. */
    shelfFrequencyHz?: number;
    /** Unit: dB. Default: -36. */
    dynamicThresholdDb?: number;
    /** Unit: dB. Range: [0, inf). Default: 3. */
    dynamicRangeDb?: number;
  };
  /** Parameters of the `spectral.lowEndFocus` processor. */
  'spectral.lowEndFocus': {
    /** Unit: Hz. Range: (0, inf). Default: 150. */
    cutoffHz?: number;
    /** Range: [0, 2]. Default: 0. */
    width?: number;
    /** Range: [0, 1]. Default: 0. */
    subharmonicAmount?: number;
    /** Range: [0, 1]. Default: 0. */
    transientTightness?: number;
  };
  /** Parameters of the `spectral.presenceEnhancer` processor. */
  'spectral.presenceEnhancer': {
    /** Range: [0, 1]. Default: 0.2. */
    amount?: number;
    /** Range: [0, inf). Default: 2. */
    drive?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3200. */
    centerFrequencyHz?: number;
    /** Range: (0, inf). Default: 1.2. */
    q?: number;
    /** Default: 'none'. */
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
  };
  /** Parameters of the `spectral.spectralShaper` processor. */
  'spectral.spectralShaper': {
    /** Range: [0, inf). Default: 0.25. */
    threshold?: number;
    /** Range: [0, 1]. Default: 0.5. */
    amount?: number;
    /** Unit: Hz. Range: (0, inf). Default: 3000. */
    frequencyHz?: number;
    /** Unit: Hz. Default: 6000. */
    highFrequencyHz?: number;
    /** Unit: ms. Range: [0, inf). Default: 3. */
    attackMs?: number;
    /** Unit: ms. Range: [0, inf). Default: 80. */
    releaseMs?: number;
    /** Unit: dB. Range: [0, inf). Default: 12. */
    rangeDb?: number;
  };
  /** Parameters of the `stereo.autoPan` processor. */
  'stereo.autoPan': {
    /** Unit: Hz. Range: [0, inf). Default: 1. */
    rateHz?: number;
    /** Range: [0, 1]. Default: 1. */
    depth?: number;
    /** Default: 0. */
    phase?: number;
    /** Default: 'sine'. */
    shape?: 'sine' | 'triangle' | 'square' | 'sawUp' | 'sawDown';
  };
  /** Parameters of the `stereo.binaural` processor. */
  'stereo.binaural': {
    /** Unit: deg. Default: 0. */
    azimuthDeg?: number;
    /** Default: false. */
    autoTurn?: boolean;
    /** Unit: Hz. Range: [0, 10]. Default: 1. */
    turnRateHz?: number;
    /** Default: true. */
    clockwise?: boolean;
    /** Default: 'phones'. */
    output?: 'speakers' | 'phones';
    /** Range: [0, 1]. Default: 1. */
    dryWet?: number;
  };
  /** Parameters of the `stereo.haasEnhancer` processor. */
  'stereo.haasEnhancer': {
    /** Unit: ms. Range: [0, 1000]. Default: 12. */
    delayMs?: number;
    /** Range: [0, 1]. Default: 1. */
    mix?: number;
    /** Default: true. */
    delayRight?: boolean;
  };
  /** Parameters of the `stereo.imager` processor. */
  'stereo.imager': {
    /** Range: [0, 2]. Default: 1. */
    width?: number;
    /** Unit: dB. Range: (-inf, 770]. Default: 0. */
    outputGainDb?: number;
    /** Range: [0, 1]. Default: 0. */
    decorrelationAmount?: number;
    /** Default: true. */
    preserveEnergy?: boolean;
  };
  /** Parameters of the `stereo.monoMaker` processor. */
  'stereo.monoMaker': {
    /** Range: [0, 1]. Default: 1. */
    amount?: number;
    /** Unit: Hz. Range: (0, inf). Default: 120. */
    frequencyHz?: number;
  };
  /** Parameters of the `stereo.phaseAlign` processor. */
  'stereo.phaseAlign': {
    /** Unit: samples. Range: [0, 1). Default: 0. */
    fractionalDelaySamples?: number;
    /** Default: true. */
    delayRight?: boolean;
    /** Unit: samples. Range: [0, inf). Default: 0. */
    delaySamples?: number;
  };
  /** Parameters of the `stereo.stereoBalance` processor. */
  'stereo.stereoBalance': {
    /** Range: [-1, 1]. Default: 0. */
    balance?: number;
    /** Default: 'normalized'. */
    law?: 'normalized' | 'rawConstantPower';
    /** Range: [0, inf). Default: 1. */
    gain?: number;
    /** Default: true. */
    constantPower?: boolean;
  };
  /** Parameters of the `utility.gain` processor. */
  'utility.gain': {
    /** Unit: dB. Range: (-inf, 770.636]. Default: 0. */
    levelDb?: number;
  };
  /** Parameters of the `voice.changer` processor. */
  'voice.changer': {
    /** Unit: dB. Range: [-24, 24]. Default: 0. */
    inputGainDb?: number;
    /** Unit: dB. Range: [-36, 12]. Default: 0. */
    outputGainDb?: number;
    /** Range: [0, 1]. Default: 1. */
    wetMix?: number;
    /** Unit: semitones. Range: [-24, 24]. Default: 0. */
    retunePitchSemitones?: number;
    /** Range: [0, 1]. Default: 1. */
    retuneMix?: number;
    /** Unit: ratio. Range: [0.55, 1.65]. Default: 1. */
    formantFactor?: number;
    /** Range: [0, 1]. Default: 1. */
    formantAmount?: number;
    /** Range: [-1, 1]. Default: 0. */
    formantBody?: number;
    /** Range: [-1, 1]. Default: 0. */
    formantBrightness?: number;
    /** Range: [-1, 1]. Default: 0. */
    formantNasal?: number;
    /** Unit: Hz. Range: [20, 300]. Default: 80. */
    eqHighPassHz?: number;
    /** Unit: dB. Range: [-12, 12]. Default: 0. */
    eqBodyDb?: number;
    /** Unit: dB. Range: [-12, 12]. Default: 1. */
    eqPresenceDb?: number;
    /** Unit: dB. Range: [-12, 12]. Default: 0. */
    eqAirDb?: number;
    /** Unit: dB. Range: [-90, -12]. Default: -55. */
    gateThresholdDb?: number;
    /** Unit: ms. Range: [0.05, 100]. Default: 2. */
    gateAttackMs?: number;
    /** Unit: ms. Range: [5, 1000]. Default: 100. */
    gateReleaseMs?: number;
    /** Unit: dB. Range: [0, 80]. Default: 18. */
    gateRangeDb?: number;
    /** Unit: dB. Range: [-60, 0]. Default: -22. */
    compressorThresholdDb?: number;
    /** Unit: ratio. Range: [1, 20]. Default: 2.5. */
    compressorRatio?: number;
    /** Unit: ms. Range: [0.05, 200]. Default: 6. */
    compressorAttackMs?: number;
    /** Unit: ms. Range: [5, 2000]. Default: 90. */
    compressorReleaseMs?: number;
    /** Unit: dB. Range: [-12, 12]. Default: 1. */
    compressorMakeupGainDb?: number;
    /** Unit: Hz. Range: [3000, 12000]. Default: 7200. */
    deesserFrequencyHz?: number;
    /** Unit: dB. Range: [-60, -6]. Default: -28. */
    deesserThresholdDb?: number;
    /** Unit: ratio. Range: [1, 20]. Default: 4. */
    deesserRatio?: number;
    /** Unit: dB. Range: [0, 24]. Default: 8. */
    deesserRangeDb?: number;
    /** Range: [0, 0.45]. Default: 0.04. */
    reverbMix?: number;
    /** Unit: ms. Range: [40, 1800]. Default: 320. */
    reverbTimeMs?: number;
    /** Range: [0, 1]. Default: 0.55. */
    reverbDamping?: number;
    /** Unit: dBFS. Range: [-12, -1]. Default: -1. */
    limiterCeilingDb?: number;
    /** Unit: ms. Range: [1, 500]. Default: 50. */
    limiterReleaseMs?: number;
    /** Unit: dBTP. Range: [-12, 0]. Default: -1. */
    limiterIspCeilingDbtp?: number;
    /** Default: 'off'. */
    pitchCorrectionMode?: 'off' | 'auto' | 'robot';
    /** Range: [0, 1]. Default: 1. */
    pitchCorrectionAmount?: number;
    /** Unit: ms. Range: [0, 400]. Default: 20. */
    pitchCorrectionSpeedMs?: number;
    /** Range: [0, 11]. Default: 0. */
    pitchCorrectionKeyRoot?: number;
    /** Default: 'major'. */
    pitchCorrectionScale?: 'major' | 'naturalMinor' | 'chromatic';
    /** Range: [-1, 1]. Default: 0. */
    pitchCorrectionRobotOctave?: number;
    /** Unit: count. Range: [0, 3]. Default: 0. */
    harmonyVoices?: number;
    /** Range: [-7, 7]. Default: 2. */
    harmonyStep1?: number;
    /** Range: [-7, 7]. Default: 4. */
    harmonyStep2?: number;
    /** Range: [-7, 7]. Default: -3. */
    harmonyStep3?: number;
    /** Range: [0, 1]. Default: 0.7. */
    harmonyLevel?: number;
    /** Range: [0, 1]. Default: 1. */
    harmonyLeadLevel?: number;
    /** Unit: cents. Range: [0, 30]. Default: 6. */
    harmonyDetuneCents?: number;
    /** Unit: ms. Range: [0, 40]. Default: 12. */
    harmonyDelayMs?: number;
    /** Default: false. */
    vocoderEnabled?: boolean;
    /** Unit: ms. Range: [0.5, 50]. Default: 2. */
    vocoderAttackMs?: number;
    /** Unit: ms. Range: [2, 300]. Default: 20. */
    vocoderReleaseMs?: number;
    /** Range: [0, 1]. Default: 0.3. */
    vocoderSibilance?: number;
    /** Default: 'saw'. */
    vocoderCarrierWave?: 'saw' | 'pulse' | 'ensemble';
    /** Unit: cents. Range: [0, 30]. Default: 12. */
    vocoderEnsembleDetuneCents?: number;
    /** Range: [0, 1]. Default: 1. */
    vocoderLevel?: number;
    /** Default: 'none'. */
    characterType?: 'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo';
    /** Range: [0, 1]. Default: 0.5. */
    characterAmount?: number;
    /** Range: [-1, 1]. Default: 0. */
    characterTone?: number;
    /** Default: 'reverb'. */
    reverbVariation?: 'reverb' | 'echo' | 'tempoDelay' | 'dubEcho';
    /** Unit: ms. Range: [1, 3000]. Default: 250. */
    reverbVariationDelayMs?: number;
    /** Range: [0, 0.95]. Default: 0.35. */
    reverbVariationFeedback?: number;
    /** Unit: bpm. Range: [30, 300]. Default: 120. */
    reverbVariationTempoBpm?: number;
    /** Default: 'eighth'. */
    reverbVariationDivision?: 'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth';
    /** Default: 'hold'. */
    midiRelease?: 'hold' | 'input';
    /** Default: 'relative'. */
    formantMode?: 'relative' | 'absolute';
    /** Default: true. */
    limiterEnableIspLimiter?: boolean;
    /** Unit: samples. Range: [0, 8192]. Default: 0. */
    retuneGrainSizeSamples?: number;
    /** Default: 1. */
    reverbSeed?: number;
    /** Unit: count. Range: [8, 32]. Default: 20. */
    vocoderBands?: number;
    /** Default: 'internal'. */
    vocoderCarrierSource?: 'internal';
    /** Default: 'psola'. */
    voiceEngine?: 'grain' | 'psola';
    /** Unit: Hz. Range: [50, 200]. Default: 70. */
    voiceEngineF0MinHz?: number;
  };
}

/** The params object of processor `P`. */
export type MasteringProcessorParams<P extends keyof MasteringProcessorParamMap> =
  MasteringProcessorParamMap[P];
// END GENERATED MasteringProcessorParamMap

// BEGIN GENERATED MixScenePreset (make processor-types)
/** Every built-in mixing scene `mixingScenePreset` can return. */
export const MIX_SCENE_PRESETS = [
  'vocalReverbSend',
  'drumBusSubgroup',
  'commentaryDucking',
] as const;

/** A built-in mixing scene name; see `MIX_SCENE_PRESETS`. */
export type MixScenePreset = (typeof MIX_SCENE_PRESETS)[number];
// END GENERATED MixScenePreset

/** Names of the processors that take a source and a reference. */
export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

/** Pair processors with a stereo entry point. */
export type StereoPairProcessor = 'match.abCrossfade';

/** Names of the analyses that compare a source with a reference. */
export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

/** Names of the analyses of a stereo pair. */
export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

/** Loudness group of {@link MasteringAudioProfile}. */
export interface MasteringAudioProfileLoudness {
  /**
   * Integrated loudness in LUFS; `-Infinity` for silence or when nothing passes the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
  /**
   * EBU Tech 3342 loudness range, in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** True peak, in dBTP. `-Infinity` for silence; absent for an empty input. */
  truePeakDbtp?: number;
  /** Crest factor (peak over RMS level) in dB. Absent for silence, where no RMS level is measured. */
  crestFactorDb?: number;
}

/** Spectral group of {@link MasteringAudioProfile}; band levels (`*RmsDb`) are dBFS, mean square: a full-scale sine reads -3.01 dBFS in its band. */
export interface MasteringAudioProfileSpectral {
  /** Sub band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  subRmsDb?: number;
  /** Low band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  lowRmsDb?: number;
  /** Low-mid band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  lowMidRmsDb?: number;
  /** Mid band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  midRmsDb?: number;
  /** High-mid band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  highMidRmsDb?: number;
  /** High band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  highRmsDb?: number;
  /** Air band level in dBFS. `-Infinity` for an exactly silent band; absent when the band holds no bin. */
  airRmsDb?: number;
  /** Spectral centroid in Hz. */
  centroidHz: number;
  /** Spectral flatness of the input. */
  flatness: number;
  /** Spectral rolloff frequency in Hz. */
  rolloffHz: number;
}

/** Dynamics group of {@link MasteringAudioProfile}. */
export interface MasteringAudioProfileDynamics {
  /** Standard deviation of the short-term loudness, in LU. */
  shortTermLufsStd: number;
  /** Onsets per second of the input. */
  attackPerSecond: number;
  /** Sustain ratio of the input. */
  sustainRatio: number;
}

/** What the repair detectors measured, one record per detector. */
export interface MasteringAudioProfileDefects {
  /** Click detector reading. */
  click: ClickDetection;
  /** Clipping detector reading. */
  clip: ClipDetection;
  /** Crackle detector reading. */
  crackle: CrackleDetection;
  /** Noise reading. */
  noise: NoiseDetection;
  /** Hum reading. */
  hum: HumDetection;
  /** Reverb reading; `latePredictability` is absent, since the profile never runs WPE. */
  reverb: ReverbDetection;
}

/** What {@link masteringAudioProfile} and its stereo form return. */
export interface MasteringAudioProfile {
  /** Duration of the input in seconds. */
  durationSec: number;
  /** Estimated tempo in beats per minute. */
  bpm: number;
  /** Confidence of the tempo estimate. */
  bpmConfidence: number;
  /** Loudness group of the profile. */
  loudness: MasteringAudioProfileLoudness;
  /** Spectral group of the profile. */
  spectral: MasteringAudioProfileSpectral;
  /** Dynamics group of the profile. */
  dynamics: MasteringAudioProfileDynamics;
  /** What the repair detectors measured; absent when `detectDefects` is false. */
  defects?: MasteringAudioProfileDefects;
}

/**
 * The measurements the assistant summarises its suggestion from. Flat, unlike
 * `MasteringAudioProfile`'s nested groups.
 */
export interface MasteringAssistantProfile {
  /** Duration of the input in seconds. */
  durationSec: number;
  /** Estimated tempo in beats per minute. */
  bpm: number;
  /** Confidence of the tempo estimate. */
  bpmConfidence: number;
  /**
   * Integrated loudness in LUFS; `-Infinity` for silence or when nothing passes the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
  /**
   * EBU Tech 3342 loudness range, in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** True peak, in dBTP. `-Infinity` for silence; absent for an empty input. */
  truePeakDbtp?: number;
  /** Crest factor (peak over RMS level) in dB. Absent for silence, where no RMS level is measured. */
  crestFactorDb?: number;
  /** Spectral centroid in Hz. */
  spectralCentroidHz: number;
  /** Spectral flatness of the input. */
  spectralFlatness: number;
  /** Onsets per second of the input. */
  attackPerSecond: number;
  /** Sustain ratio of the input. */
  sustainRatio: number;
}

/** What {@link masteringAssistantSuggest} and its stereo form return. */
export interface MasteringAssistantResult {
  /** The suggested chain; pass it as `overrides` to {@link masterAudio} unchanged. */
  chainConfig: MasteringChainConfig;
  /** One line per decision the assistant made, in the order it made them. */
  explanation: string[];
  /** Measurements the suggestion was derived from. */
  profile: MasteringAssistantProfile;
}

/** One delivery target in a {@link MasteringStreamingPreviewResult}. */
export interface MasteringStreamingPreviewPlatform {
  /** Platform name, echoed from the requested target. */
  name: string;
  /**
   * Integrated loudness in LUFS; `-Infinity` for silence or when nothing passes the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
  /**
   * True peak of the take, in dBTP. `-Infinity` for silence.
   * Never absent: every input sample measures it.
   */
  truePeakDbtp: number;
  /** Gain that lands the take on the platform's target; absent when the take has no measurable loudness. */
  normalizationGainDb?: number;
  /** True when that gain would push the true peak above the platform's ceiling. */
  ceilingRisk: boolean;
}

/** What {@link masteringStreamingPreview} and its stereo form return. */
export interface MasteringStreamingPreviewResult {
  /** One entry per delivery target. */
  platforms: MasteringStreamingPreviewPlatform[];
}

/** Result of the `match.referenceLoudness` pair analysis. */
export interface MatchReferenceLoudnessResult {
  /**
   * Integrated loudness of the source in LUFS. `-Infinity` below the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  sourceLufs: number;
  /**
   * Integrated loudness of the reference in LUFS. `-Infinity` below the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  referenceLufs: number;
  /** Gain in dB that brings the source to the reference loudness. Absent when either side has no measurable loudness. */
  gainToMatchDb?: number;
}

/** One band of the `match.tonalBalance` and `match.tonalBalanceLogBands` analyses. */
export interface MatchTonalBalanceBand {
  /** Lower band edge in Hz. */
  lowHz: number;
  /** Upper band edge in Hz. */
  highHz: number;
  /** Source level in the band, in dB. `-Infinity` for a silent band; absent when the band holds no bin. */
  sourceDb?: number;
  /** Reference level in the band, in dB. `-Infinity` for a silent band; absent when the band holds no bin. */
  referenceDb?: number;
  /** Source level minus reference level in dB. `-Infinity` when only the source band is silent; absent when the reference band is silent or the band holds no bin. */
  deviationDb?: number;
}

/** Result of the `match.tonalBalance` pair analysis. */
export interface MatchTonalBalanceResult {
  /** Per-band readings. */
  bands: MatchTonalBalanceBand[];
}

/** Result of the `match.matchEqCurve` pair analysis: `gainDb[i]` is the gain at `frequencies[i]`. */
export interface MatchEqCurveResult {
  /** Frequencies in Hz. */
  frequencies: number[];
  /** Gain in dB at each frequency. */
  gainDb: number[];
}

/** Result of the `match.estimateReferenceDelaySamples` pair analysis. */
export interface MatchEstimateReferenceDelaySamplesResult {
  /** Estimated delay of the reference relative to the source, in samples. */
  delaySamples: number;
}

/** What each {@link PairAnalysis} name returns. */
export interface PairAnalysisResultMap {
  /** Loudness of source and reference and the gain that matches them. */
  'match.referenceLoudness': MatchReferenceLoudnessResult;
  /** Per-band level comparison on linear bands. */
  'match.tonalBalance': MatchTonalBalanceResult;
  /** Per-band level comparison on logarithmic bands. */
  'match.tonalBalanceLogBands': MatchTonalBalanceResult;
  /** Gain-versus-frequency curve that matches the source tonal balance to the reference. */
  'match.matchEqCurve': MatchEqCurveResult;
  /** Delay of the reference relative to the source, in samples. */
  'match.estimateReferenceDelaySamples': MatchEstimateReferenceDelaySamplesResult;
}

/** Result of the `stereo.monoCompatCheck` analysis. `width` is absent for a fully out-of-phase pair. */
export interface StereoMonoCompatCheckResult {
  /** Inter-channel correlation (-1 to 1). Absent when either channel is silent. */
  correlation?: number;
  /** Stereo width; `Infinity` for a fully out-of-phase pair, absent for silence. */
  width?: number;
  /** Peak of the mono fold-down 0.5 * (L + R), as a linear full-scale amplitude. */
  monoPeak: number;
  /** RMS of the side signal 0.5 * (L - R), as a linear full-scale amplitude. */
  sideRms: number;
  /** True when the pair is likely to survive a mono fold-down. */
  likelyMonoCompatible: boolean;
}

/** One band of the `stereo.monoCompatCheckLogBands` analysis. */
export interface StereoMonoCompatBand {
  /** Lower band edge in Hz. */
  lowHz: number;
  /** Upper band edge in Hz. */
  highHz: number;
  /** Inter-channel correlation (-1 to 1). Absent when either channel is silent in the band. */
  correlation?: number;
  /** RMS of the band's share of the side signal 0.5 * (L - R), as a linear full-scale amplitude. */
  sideRms: number;
}

/** Result of the `stereo.monoCompatCheckLogBands` analysis. */
export interface StereoMonoCompatCheckLogBandsResult {
  /** Per-band readings. */
  bands: StereoMonoCompatBand[];
}

/** What each {@link StereoAnalysis} name returns. */
export interface StereoAnalysisResultMap {
  /** Mono-compatibility reading of the whole pair. */
  'stereo.monoCompatCheck': StereoMonoCompatCheckResult;
  /** Mono-compatibility reading per logarithmic band. */
  'stereo.monoCompatCheckLogBands': StereoMonoCompatCheckLogBandsResult;
}

/** Result of a single mono mastering processor. */
export interface MasteringResult {
  /** Processed mono samples. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * Integrated loudness of the input in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  inputLufs: number;
  /**
   * Integrated loudness of the output in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  outputLufs: number;
  /** Sum of the static gains the stages applied, in dB; limiter gain reduction is not included. */
  appliedGainDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited?: boolean;
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
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
   * const result = masteringApplyProcessor({ processor: 'dynamics.limiter', samples, sampleRate: 44100 });
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
}

/**
 * Params of the pair processors and of the pair and stereo analyses, which the
 * processor catalog does not enumerate: finite numbers and booleans, keyed as
 * the processor reads them.
 */
export type MasteringPairParams = Record<string, number | boolean>;

/** What {@link masteringNormalizeLoudness} returns. */
export interface MasteringNormalizeLoudnessResult extends MasteringResult {
  /** True when peak headroom prevented the requested LUFS target; always reported. */
  loudnessTargetLimited: boolean;
}

/** Result of a single stereo mastering processor. */
export interface MasteringStereoResult {
  /** Left channel of the processed audio. */
  left: Float32Array;
  /** Right channel of the processed audio. */
  right: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * Integrated loudness of the input in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  inputLufs: number;
  /**
   * Integrated loudness of the output in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  outputLufs: number;
  /** Sum of the static gains the stages applied, in dB; limiter gain reduction is not included. */
  appliedGainDb: number;
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * See {@link MasteringResult.nonFiniteSubstitutionCount}. Summed over both
   * channels.
   */
  nonFiniteSubstitutionCount: number;
}

/**
 * The loudness numbers a {@link StreamingMasteringChain} is constructed with,
 * measured at the loudness stage's input (after every earlier enabled stage),
 * which is where the offline chain measures.
 *
 * `loudnessStaticGainDb` equals the gain {@link masterAudio} applies, ceiling
 * clamp included. Pass it and `truePeakDbtp` as the chain config's
 * `loudnessStaticGainDb` and `loudnessStaticGainPeakDbtp`; an absent gain
 * passes through as absent, which the chain reads as "apply no gain".
 */
export interface StreamingLoudnessGainResult {
  /**
   * Static gain in dB the offline loudness stage applies. Absent when the stage
   * input has no measurable loudness (silence, or nothing above the absolute gate).
   */
  loudnessStaticGainDb?: number;
  /**
   * True peak in dBTP of the loudness stage's input; `-Infinity` for digital silence.
   * Never absent: every input sample measures it.
   */
  truePeakDbtp: number;
  /**
   * Integrated loudness in LUFS of the loudness stage's input; `-Infinity` below the absolute gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
}

/** What gain-matching one take to another's loudness took, and produced. */
export interface LoudnessMatchResult {
  /** The source, gain-matched to the reference's integrated loudness. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * The reference's BS.1770 integrated loudness. Always finite: a silent or
   * below-gate take on either side is refused with `RangeError` ("... has no
   * measurable loudness").
   */
  referenceLufs: number;
  /** The source's, before the gain. Always finite, as `referenceLufs`. */
  sourceLufs: number;
  /** Gain applied to the source, in dB. */
  appliedGainDb: number;
  /**
   * The matched take's true peak after the gain, in dBTP. It can sit above
   * 0 dBTP: the gain is applied with no upper bound, because clamping for
   * headroom would return the source at its own loudness whenever it started
   * near full scale, which is the one thing a loudness match must not do.
   * Limiting is the caller's decision, so a value above 0 is a report rather
   * than a defect. `-Infinity` for silence.
   * Never absent: every input sample measures it.
   */
  matchedTruePeakDbtp: number;
}

/** Stereo counterpart of {@link LoudnessMatchResult}. */
export interface LoudnessMatchStereoResult {
  /** The left source channel, gain-matched to the reference loudness. */
  left: Float32Array;
  /** The right source channel, gain-matched with the same gain as `left`. */
  right: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * The reference program's BS.1770 integrated loudness.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  referenceLufs: number;
  /**
   * The stereo source program's loudness before the gain.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  sourceLufs: number;
  /** One gain applied to both source channels, in dB. */
  appliedGainDb: number;
  /**
   * Maximum true peak across the matched left and right channels, in dBTP. `-Infinity` for silence.
   * Never absent: every input sample measures it.
   */
  matchedTruePeakDbtp: number;
}

/**
 * One band of the N-band multiband compressor (`dynamics.multibandComp.bands`).
 * Every field is optional: an omitted field keeps the base config's value for
 * that band, or the compressor default for a band the crossover adds.
 */
export interface MultibandCompressorBand {
  /** Threshold in dB. */
  thresholdDb?: number;
  /** Compression ratio. */
  ratio?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  /** Knee width in dB. */
  kneeDb?: number;
  /** Makeup gain in dB. */
  makeupGainDb?: number;
  /** Derive the makeup gain automatically. */
  autoMakeup?: boolean;
  /** Level detector: `'peak'`, `'rms'` or `'logRms'`. */
  detector?: CompressorDetector;
  /** Sidechain high-pass corner in Hz, greater than 0; absent = no high-pass. */
  sidechainHighPassHz?: number;
  /** Program-dependent release time in ms. Default `0`. */
  pdrTimeMs?: number;
  /** Program-dependent release multiplier. Default `1`. */
  pdrReleaseScale?: number;
}

/**
 * Nested mastering-chain configuration. Top-level keys are the processing
 * modules; nest processor and parameter names beneath them, e.g.
 *
 * ```ts
 * masterAudio({
 *   samples,
 *   sampleRate,
 *   overrides: {
 *     dynamics: { compressor: { thresholdDb: -24 } },
 *     loudness: { targetLufs: -14 },
 *   },
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
  /** Repair modules (denoise, declip, decrackle, dehum, declick, dereverb). */
  repair?: {
    /** Broadband noise reduction from a running noise estimate. */
    denoise?: {
      /** Turns the denoise stage on. Default false. */
      enabled?: boolean;
      /** Gain function: `logMmse` (log-spectral amplitude, least musical noise; default), `mmseStsa`, or `spectralSubtraction` (cheapest, most prone to musical noise). */
      mode?: 'logMmse' | 'mmseStsa' | 'spectralSubtraction';
      /** `'spp'` is the speech-presence-probability estimator. */
      noiseEstimator?: 'quantile' | 'mcra' | 'imcra' | 'spp';
      /** STFT frame size in samples, a power of two. Default 1024. */
      nFft?: number;
      /** STFT hop in samples, at most `nFft / 2`. Default 256. */
      hopLength?: number;
      /** Decision-directed a priori SNR smoothing in [0, 1]; higher is smoother but adapts more slowly to changing noise. Default 0.98. */
      ddAlpha?: number;
      /** Deepest attenuation applied to any bin, in dB (0 to 120); the noise is left this far down rather than removed. Default 26. */
      reductionDb?: number;
      /** Over-subtraction factor (0 to 16) that scales the noise estimate removed. Used only by `spectralSubtraction`. Default 2. */
      overSubtraction?: number;
      /** Residual floor as a fraction of the noise spectrum (0 to 1). Used only by `spectralSubtraction`. Default 0.05. */
      spectralFloor?: number;
      /** Fraction of the quietest frames (0 to 1) taken as noise-only by the `quantile` estimator. Default 0.1. */
      noiseEstimationQuantile?: number;
      /** Weights the gain by the speech-presence probability so active passages are attenuated less. Default true. */
      speechPresenceGain?: boolean;
      /** Smooths the gain mask across time to suppress musical-noise artefacts. Default true. */
      gainSmoothing?: boolean;
    };
    /** Reconstruction of clipped peaks. */
    declip?: {
      /** Turns the declip stage on. Default false. */
      enabled?: boolean;
      /** Clip level in dBFS, at or below 0. */
      clipThresholdDb?: number;
      /** Order of the linear-predictive model used to reconstruct clipped runs (0 to 2048). Default 36. */
      lpcOrder?: number;
      /** Outer reconstruction rounds, each re-estimating the model (1 to 8). Default 2. */
      iterations?: number;
      /**
       * Reconstruct every channel over the union of all channels' clipped runs. Default false:
       * linked, the channels' repair errors agree, at the cost of re-estimating unclipped samples in the quieter channel; use it on near-mono material. Off, each channel repairs its own clipped runs and leaves every sample it did not clip bit-identical.
       */
      linkChannels?: boolean;
    };
    /** Removal of dense small impulses such as vinyl crackle. */
    decrackle?: {
      /** Turns the decrackle stage on. Default false. */
      enabled?: boolean;
      /** Sample magnitude deviating from the local median that counts as crackle (0 to 1000); in `waveletShrinkage` it caps the shrinkage threshold. Default 0.4. */
      threshold?: number;
      /** Removal method: `median` replaces outlier samples with the local median (default); `waveletShrinkage` shrinks Haar detail coefficients. */
      mode?: 'median' | 'waveletShrinkage';
      /** Haar decomposition levels used by `waveletShrinkage` (1 to 24). Default 4. */
      levels?: number;
    };
    /** Removal of mains hum at a fundamental and its harmonics. */
    dehum?: {
      /** Turns the dehum stage on. Default false. */
      enabled?: boolean;
      /** Hum fundamental in Hz, typically 50 or 60 (0 to 5000). Default 50. */
      fundamentalHz?: number;
      /** Number of harmonics of the fundamental removed (1 to 16). Default 4. */
      harmonics?: number;
      /** Selectivity of the removal (0 to 100); the notch Q in `notch` mode, and the inverse of the cancellation settling time in `subtract` mode. Default 20. */
      q?: number;
      /** Searches for and tracks the hum frequency around `fundamentalHz` instead of assuming it. Default false. */
      adaptive?: boolean;
      /** Half-width of the fundamental search in Hz (0 to 100). Used when `adaptive`. Default 2. */
      searchRangeHz?: number;
      /** How quickly the removal follows changes in the tracked tone (0 to 1). Used when `adaptive`. Default 0.25. */
      adaptation?: number;
      /** Analysis block and look-ahead in samples (16 to 16384). Used when `adaptive`. Default 2048. */
      frameSize?: number;
      /** Tracking loop bandwidth as a fraction of the tracked frequency (0 to 1). Used when `adaptive`. Default 0.01. */
      pllBandwidth?: number;
      /** `'subtract'` the tracked harmonics, or `'notch'` (cascaded notches). */
      mode?: 'subtract' | 'notch';
    };
    /** Removal of isolated clicks and pops. */
    declick?: {
      /** Turns the declick stage on. Default false. */
      enabled?: boolean;
      /** Sample magnitude above which a sample is a click candidate (0 to 10; full scale is 1). Default 0.8. */
      threshold?: number;
      /** How far a click must stand above the sample-to-sample level around it, as a ratio (0 to 100). Default 4. */
      neighborRatio?: number;
      /** Longest click run, in samples, that is repaired (1 to 512). Default 8. */
      maxClickSamples?: number;
      /** Order of the linear-predictive model that fills a click (0 to 2048). Default 20. */
      lpcOrder?: number;
      /** Ratio of the prediction residual to its typical level above which a run is treated as a click (0 to 1000). Default 8. */
      residualRatio?: number;
    };
    /** Reduction of late reverberation. */
    dereverb?: {
      /** Turns the dereverb stage on. Default false. */
      enabled?: boolean;
      /** Late-reverb detection threshold relative to a bin's own power (0 to 1); 0 treats every bin with late energy as reverb. Default 0. */
      threshold?: number;
      /** Share of the computed suppression that is applied, from 0 (none) to 1 (all). Default 1. */
      attenuation?: number;
      /** STFT frame size in samples, a power of two. Default 1024. */
      nFft?: number;
      /** STFT hop in samples, at most `nFft / 2`. Default 256. */
      hopLength?: number;
      /** Assumed reverberation decay time (RT60) in seconds (0 to 10). Default 0.4. */
      t60Sec?: number;
      /** Delay after which reverberation counts as late, in ms (0 to 500). Default 50. */
      lateDelayMs?: number;
      /** Over-subtraction factor (0 to 16) applied to the late-reverb estimate. Default 1. */
      overSubtraction?: number;
      /** Smallest gain the suppression may apply, as a fraction (0 to 1). Default 0.08. */
      spectralFloor?: number;
      /** Adds a weighted-prediction-error (WPE) stage that predicts and removes the late tail. Default false. */
      wpeEnabled?: boolean;
      /** WPE refinement passes (1 to 16). Default 2. */
      wpeIterations?: number;
      /** WPE predictor length in frames (1 to 32). Default 3. */
      wpeTaps?: number;
      /** Share of the WPE prediction that is subtracted (0 to 1). Default 0.7. */
      wpeStrength?: number;
    };
  };
  /** EQ modules. */
  eq?: {
    /** Spectral tilt EQ that raises one end of the spectrum and lowers the other. */
    tilt?: {
      /** Turns the tilt EQ stage on. Default false. */
      enabled?: boolean;
      /** Spectral tilt in dB: positive boosts the highs and cuts the lows around `pivotHz`, negative does the reverse. Default 0. */
      tiltDb?: number;
      /** Frequency in Hz around which the spectrum tilts and the gain is zero; must be above 0. Default 1000. */
      pivotHz?: number;
    };
  };
  /** Dynamics modules (compressor, deesser, transientShaper, multibandComp). */
  dynamics?: {
    /** Full-band compressor. */
    compressor?: {
      /** Turns the compressor stage on. Default false. */
      enabled?: boolean;
      /** Level in dB above which gain reduction starts. Default -18. */
      thresholdDb?: number;
      /** Compression ratio x:1 (at least 1); 2 reduces a 4 dB overshoot to 2 dB. Default 2. */
      ratio?: number;
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      attackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      releaseMs?: number;
      /** Width in dB of the soft knee around the threshold; 0 is a hard knee. Default 0. */
      kneeDb?: number;
      /** Gain in dB added after compression. Default 0. */
      makeupGainDb?: number;
      /** Derives the makeup gain from the threshold and ratio instead of using `makeupGainDb`. Default false. */
      autoMakeup?: boolean;
      /** Level detector: `'peak'`, `'rms'` or `'logRms'`. Default `'rms'`. */
      detector?: CompressorDetector;
      /** Sidechain high-pass corner in Hz, greater than 0; absent = no high-pass. */
      sidechainHighPassHz?: number;
      /** Program-dependent release time in ms; 0 leaves the release fixed. Default 0. */
      pdrTimeMs?: number;
      /** Program-dependent release multiplier. Default 1. */
      pdrReleaseScale?: number;
    };
    /** Sibilance reduction in a detection band. */
    deesser?: {
      /** Turns the de-esser stage on. Default false. */
      enabled?: boolean;
      /** Centre of the sibilance band that is detected and reduced, in Hz. Default 6000. */
      frequencyHz?: number;
      /** Level in dB of the sibilance band above which reduction starts. Default -24. */
      thresholdDb?: number;
      /** Reduction ratio x:1 (at least 1) applied above the threshold. Default 4. */
      ratio?: number;
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      attackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      releaseMs?: number;
      /** Largest gain reduction in dB the de-esser may apply (0 or more). Default 12. */
      rangeDb?: number;
      /** Q of the sibilance detection band; higher is narrower. Default 1.5. */
      bandPassQ?: number;
    };
    /** Attack and sustain shaping driven by fast and slow envelopes. */
    transientShaper?: {
      /** Turns the transient shaper stage on. Default false. */
      enabled?: boolean;
      /** Gain in dB applied to the attack portion of transients; positive sharpens, negative softens. Default 3. */
      attackGainDb?: number;
      /** Gain in dB applied to the sustain portion after a transient; positive lengthens, negative tightens. Default 0. */
      sustainGainDb?: number;
      /** Fast envelope follower attack in ms: one time constant, about 63% of the way at the set time. Default 0. */
      fastAttackMs?: number;
      /** Fast envelope follower release in ms: one time constant, about 63% of the way at the set time. Default 20. */
      fastReleaseMs?: number;
      /** Slow envelope follower attack in ms: one time constant, about 63% of the way at the set time. Default 15. */
      slowAttackMs?: number;
      /** Slow envelope follower release in ms: one time constant, about 63% of the way at the set time. Default 200. */
      slowReleaseMs?: number;
      /** Scales how strongly the difference between the fast and slow envelopes drives the gain (0 or more). Default 1. */
      sensitivity?: number;
      /** Largest gain change in dB, boost or cut, the shaper may apply (0 or more). Default 12. */
      maxGainDb?: number;
      /** Smoothing time in ms applied to the gain change; 0 applies it unsmoothed. Default 0. */
      gainSmoothingMs?: number;
      /** Look-ahead delay in ms that lets the gain act before the transient; adds latency. Default 0. */
      lookaheadMs?: number;
    };
    /** Compressor applied per crossover band. */
    multibandComp?: {
      /** Turns the multiband compressor stage on. Default false. */
      enabled?: boolean;
      /**
       * Crossover of the N-band form. `cutoffsHz` replaces the whole cutoff list
       * and sets the band count to `length + 1` (2 to 64 bands). An empty list is
       * refused. Giving any member of `crossover` enables the stage unless
       * `enabled` says otherwise.
       */
      crossover?: {
        /** Ascending split frequencies in Hz; N values give N + 1 bands. */
        cutoffsHz?: number[];
        /** Crossover slope: `lr2`, `lr4` (default) or `lr8`. */
        slope?: 'lr2' | 'lr4' | 'lr8';
        /** Crossover filter family; `firLinearPhase` is linear-phase and adds latency. Default `linkwitzRiley`. */
        mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
        /** FIR kernel length in samples (1 to 65535). Used by `firLinearPhase`. Default 513. */
        firKernelSize?: number;
      };
      /**
       * Per-band compressor settings by band index. An omitted band or field
       * keeps the base (preset) value, or the compressor default for a band the
       * base does not have.
       */
      bands?: MultibandCompressorBand[];
    };
  };
  /** Saturation modules (tape, exciter). */
  saturation?: {
    /** Magnetic tape saturation model. */
    tape?: {
      /** Turns the tape saturation stage on. Default false. */
      enabled?: boolean;
      /** Input drive in dB before saturation; about -12 to +24 is useful. Default 3. */
      driveDb?: number;
      /** Shape of the anhysteretic magnetization curve (0 to 1); higher saturates at a lower level. Default 0.5. */
      saturation?: number;
      /** Width of the magnetic hysteresis loop (0 to 1); wider thickens the low end. Default 0.2. */
      hysteresis?: number;
      /** Output trim in dB after saturation. Default 0. */
      outputGainDb?: number;
      /** Simulated tape speed in inches per second (above 0), which sets the head-bump and gap-loss filters. Default 15. */
      speedIps?: number;
      /** Low-frequency head-bump boost in dB (0 or more). Default 1.5. */
      headBumpDb?: number;
      /** Tape bias offset that adds asymmetry to the saturation. Default 0. */
      bias?: number;
      /** Strength of the high-frequency loss from the playback head gap (0 to 1). Default 0.2. */
      gapLoss?: number;
      /** Jiles-Atherton core oversampling: 1 (default), 2, or 4. */
      oversampleFactor?: number;
    };
    /** Harmonic exciter for a high-frequency band. */
    exciter?: {
      /** Turns the exciter stage on. Default false. */
      enabled?: boolean;
      /** Centre of the band from which harmonics are generated, in Hz (above 0). Default 3000. */
      frequencyHz?: number;
      /** Drive in dB into the harmonic generator. Default 6. */
      driveDb?: number;
      /** Level of the generated harmonics mixed back in (0 or more); 0 adds nothing. Default 0.25. */
      amount?: number;
      /** Q of the excited band (above 0); higher is narrower. Default 1. */
      q?: number;
      /** Balance of harmonic content from 0 (even harmonics) to 1 (odd harmonics). Default 0.5. */
      evenOddMix?: number;
      /**
       * Antialiasing mode: `'none'`, or `'oversample4x'`, which adds 48 samples
       * of latency at any host rate. `'adaa1'` and `'adaa2'` name modes the
       * exciter does not implement and are refused. Default `'none'`.
       */
      aliasing?: 'none' | 'oversample4x';
    };
  };
  /** Spectral modules (airBand). */
  spectral?: {
    /** Level-dependent high-shelf lift. */
    airBand?: {
      /** Turns the air band stage on. Default false. */
      enabled?: boolean;
      /** Strength of the high-frequency lift and added harmonic air (0 to 1). Default 0.25. */
      amount?: number;
      /** Corner frequency of the high shelf in Hz. Default 12000. */
      shelfFrequencyHz?: number;
      /** Level in dB of the high band above which the shelf gain starts to rise. Default -36. */
      dynamicThresholdDb?: number;
      /** Largest shelf gain in dB (0 or more) before `amount` scaling. Default 3. */
      dynamicRangeDb?: number;
    };
  };
  /** Stereo modules (imager, monoMaker); applied on the stereo path only. */
  stereo?: {
    /** Mid/side stereo width control. */
    imager?: {
      /** Turns the stereo imager stage on. Default false. */
      enabled?: boolean;
      /** Stereo width (0 to 2): 0 is mono, 1 leaves the image unchanged, 2 doubles the side signal. Default 1. */
      width?: number;
      /** Output gain in dB applied after widening. Default 0. */
      outputGainDb?: number;
      /** Share of decorrelated side signal blended in when `width` is above 1 (0 to 1). Default 0. */
      decorrelationAmount?: number;
      /** Compensates the level so widening does not change overall loudness. Default true. */
      preserveEnergy?: boolean;
    };
    /** Folds the low end to mono below a cutoff. */
    monoMaker?: {
      /** Turns the mono maker stage on. Default false. */
      enabled?: boolean;
      /** How much of the low band is collapsed to mono (0 to 1); 1 is fully mono. Default 1. */
      amount?: number;
      /** Frequency in Hz below which the signal is made mono. Default 120. */
      frequencyHz?: number;
    };
  };
  /** Maximizer modules (true-peak limiter). */
  maximizer?: {
    /** Oversampled true-peak limiter. */
    truePeakLimiter?: {
      /** Turns the true-peak limiter stage on. Default false. */
      enabled?: boolean;
      /** True-peak ceiling in dBTP (0 or below) that the output does not exceed. Default -1. */
      ceilingDbtp?: number;
      /** Look-ahead in ms that lets the gain reduction start before a peak; adds latency. Default 1. */
      lookaheadMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      releaseMs?: number;
      /** Oversampling used to detect inter-sample peaks: 1, 2, 4, 8 or 16. Default 4. */
      oversampleFactor?: number;
      /** Applies the gain at the input rate instead of reconstructing at the oversampled rate, which shortens latency. Default false. */
      applyGainAtInputRate?: boolean;
    };
  };
  /** Loudness stage: integrated-loudness target and true-peak ceiling. */
  loudness?: {
    /** Turns the loudness stage on. Default false. */
    enabled?: boolean;
    /** Integrated-loudness target in LUFS. Default -14. */
    targetLufs?: number;
    /** Ceiling of the loudness stage; `maximizer.truePeakLimiter.ceilingDbtp` bounds the limiter, each applies to its own stage. */
    ceilingDbtp?: number;
    /** Oversampling factor of the true-peak limiter and meter in this stage: 1, 2, 4, 8 or 16. Default 4. */
    truePeakOversample?: number;
    /** Release in ms: one time constant, about 63% of the way at the set time. */
    releaseMs?: number;
    /** Applies the limiter gain at the input rate instead of the oversampled rate, which shortens latency. Default false. */
    applyGainAtInputRate?: boolean;
    /**
     * How deep, in dB, the stage may drive its post-gain true-peak limiter to
     * reach {@link targetLufs}. Default 12. The static normalization gain may
     * exceed the peak headroom toward `ceilingDbtp` by this much; `0` restores a
     * strict headroom clamp, under which peak-normalized input keeps its input
     * loudness whatever target is asked for. `ceilingDbtp` holds at every setting.
     */
    maxLimiterGainReductionDb?: number;
  };
}

/** Every `[dotted path, value]` pair below a node of the nested chain config; an array is one value. */
type ChainConfigPathEntry<T, P extends string> = {
  [K in keyof T & string]-?: NonNullable<T[K]> extends readonly unknown[]
    ? [`${P}${K}`, NonNullable<T[K]>]
    : NonNullable<T[K]> extends object
      ? [`${P}${K}`, NonNullable<T[K]>] | ChainConfigPathEntry<NonNullable<T[K]>, `${P}${K}.`>
      : [`${P}${K}`, NonNullable<T[K]>];
}[keyof T & string];

/**
 * A chain config as an input takes it. A key is a dotted path
 * (`'loudness.targetLufs'`) and a nested object is shorthand for its prefix;
 * giving one path both ways is refused with a TypeError, as is a path the chain
 * does not have. The nested {@link MasteringChainConfig}, the flat form (the
 * `params` of a saved chain document, whose `'dynamics.multibandComp'` carries
 * the nested object below that path) and an object mixing the two for different
 * paths are all accepted, and a dotted path is typed by the value the nested
 * config holds there. An array is typed whole at its path; the chain also reads
 * one element as `'<path>.<index>'`, which this type does not name. Results
 * always return the nested form.
 */
export type MasteringChainConfigInput = MasteringChainConfig & {
  [E in ChainConfigPathEntry<MasteringChainConfig, ''> as E[0] extends `${string}.${string}`
    ? E[0]
    : never]?: E[1];
};

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
  /**
   * Integrated loudness in LUFS. `-Infinity` for silence or when nothing passes the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
  /** Maximum momentary loudness in LUFS. `-Infinity` for silence; absent when the signal is shorter than one window. */
  maxMomentaryLufs?: number;
  /** Maximum short-term loudness in LUFS. `-Infinity` for silence; absent when the signal is shorter than one window. */
  maxShortTermLufs?: number;
  /** True peak in dBTP. `-Infinity` for silence. Never absent: every input sample measures it. */
  truePeakDbtp: number;
  /**
   * EBU Tech 3342 loudness range, in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
}

/** Compact explanation of how an offline mastering chain changed a program. */
export interface MasteringReport {
  /** Measurements of the input. */
  before: MasteringLoudnessSummary;
  /** Measurements of the output. */
  after: MasteringLoudnessSummary;
  /** Sum of the static gains the stages applied, in dB; limiter gain reduction is not included. */
  appliedGainDb: number;
  /** Most-negative final dynamics/limiter gain reduction, or zero when none ran. */
  maxGainReductionDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * Static gain the loudness stage applied, in dB. Absent when the stage is off or
   * its input has no measurable loudness (silence, or nothing above the absolute
   * gate); the stage then applies no gain.
   */
  loudnessGainDb?: number;
  /**
   * 32 logarithmically-spaced after-minus-before spectral energy deltas (dB). NaN in
   * a band where either spectrum has no level.
   */
  bandEnergyDeltaDb: Float32Array;
}

/** Result of the offline mastering chain on a mono signal. */
export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * Integrated loudness of the input in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  inputLufs: number;
  /**
   * Integrated loudness of the output in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  outputLufs: number;
  /** Sum of the static gains the stages applied, in dB; limiter gain reduction is not included. */
  appliedGainDb: number;
  /** Names of the stages that ran, in order. */
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
   * `-Infinity` for silence.
   */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 Loudness Range of the output (LU). Absent when not measured. */
  outputLoudnessRangeLu?: number;
  /**
   * Static gain the loudness stage applied, in dB. Absent when the stage is off or
   * its input has no measurable loudness (silence, or nothing above the absolute
   * gate); the stage then applies no gain.
   */
  loudnessGainDb?: number;
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
   * const result = masterAudio({ samples, sampleRate: 44100, preset: 'pop' });
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
  /** Per-stage gain reductions for the dynamics/maximizer stages (a subset of `stages`). */
  stageGainReductions: StageGainReduction[];
  /** Before/after measurements of the run. */
  report: MasteringReport;
}

/** Result of the offline mastering chain on a stereo signal. */
export interface MasteringChainStereoResult {
  /** Left channel of the mastered audio. */
  left: Float32Array;
  /** Right channel of the mastered audio. */
  right: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /**
   * Integrated loudness of the input in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  inputLufs: number;
  /**
   * Integrated loudness of the output in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  outputLufs: number;
  /** Sum of the static gains the stages applied, in dB; limiter gain reduction is not included. */
  appliedGainDb: number;
  /** Names of the stages that ran, in order. */
  stages: string[];
  /** See {@link MasteringChainResult} for field semantics. `-Infinity` for silence. */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 loudness range of the output, in LU. Absent when not measured. */
  outputLoudnessRangeLu?: number;
  /** See {@link MasteringChainResult.loudnessGainDb}. */
  loudnessGainDb?: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /** See {@link MasteringChainResult} for field semantics. Aggregated over both channels. */
  nonFiniteSubstitutionCount: number;
  /** Per-stage gain reductions for the dynamics/maximizer stages (a subset of `stages`). */
  stageGainReductions: StageGainReduction[];
  /** Before/after measurements of the run. */
  report: MasteringReport;
}
