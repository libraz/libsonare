import type { CompressorDetector } from './mastering_dynamics.js';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  HumDetection,
  NoiseDetection,
  ReverbDetection,
} from './public_types_repair.js';
import type { ValidateOptions } from './validation.js';

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
  ceilingDb: number;
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
  'effects.reverb.dattorro',
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
  'effects.reverb.dattorro',
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

export type MasteringInsertName = (typeof MASTERING_INSERT_NAMES)[number];
// END GENERATED MasteringInsertName

// BEGIN GENERATED MasteringProcessorParamMap (make processor-types)
/**
 * Parameters of each catalog processor id, keyed as in `masteringInsertParamInfo`.
 * Every key is optional; an enum-valued key takes one of its `choices` names.
 */
export interface MasteringProcessorParamMap {
  'dynamics.brickwallLimiter': {
    ceilingDb?: number;
    releaseMs?: number;
    lookaheadMs?: number;
  };
  'dynamics.compressor': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    makeupGainDb?: number;
    kneeDb?: number;
    autoMakeup?: boolean;
    detector?: 'peak' | 'rms' | 'logRms';
    sidechainHpfEnabled?: boolean;
    sidechainHpfHz?: number;
    pdrTimeMs?: number;
    pdrReleaseScale?: number;
  };
  'dynamics.deesser': {
    frequencyHz?: number;
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
    bandpassQ?: number;
  };
  'dynamics.duckingProcessor': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
    lookaheadMs?: number;
  };
  'dynamics.expander': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
  };
  'dynamics.gate': {
    thresholdDb?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
    closeThresholdDb?: number;
    holdMs?: number;
    keyHpfHz?: number;
  };
  'dynamics.limiter': {
    thresholdDb?: number;
    releaseMs?: number;
    ratio?: number;
    postGainDb?: number;
    lookaheadMs?: number;
  };
  'dynamics.parallelComp': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    makeupGainDb?: number;
    mix?: number;
    outputCeilingDb?: number;
    linkedDetection?: boolean;
    outputLimiter?: boolean;
  };
  'dynamics.sidechainRouter': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
    keyListen?: boolean;
    lookaheadMs?: number;
    monoSumming?: boolean;
    sidechainHpfEnabled?: boolean;
    sidechainHpfHz?: number;
  };
  'dynamics.transientShaper': {
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
  'dynamics.upwardCompressor': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
  };
  'dynamics.upwardExpander': {
    thresholdDb?: number;
    ratio?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
  };
  'dynamics.vocalRider': {
    targetDb?: number;
    maxBoostDb?: number;
    maxCutDb?: number;
    attackMs?: number;
    releaseMs?: number;
    outputGainDb?: number;
    gainSmoothingMs?: number;
    noiseFloorDb?: number;
    linkedDetection?: boolean;
  };
  'effects.acoustic.roomMorph': {
    dryWet?: number;
    sourceTailSuppression?: number;
    absorption?: number;
    airAbsorptionEnabled?: boolean;
    airHumidityPercent?: number;
    airTemperatureC?: number;
    bandAbsorption?: readonly number[];
    bandScattering?: readonly number[];
    crossfadeMs?: number;
    heightM?: number;
    ismOrder?: number;
    lengthM?: number;
    listenerX?: number;
    listenerY?: number;
    listenerZ?: number;
    materialPreset?: number;
    maxSeconds?: number;
    mixingTimeMs?: number;
    preferEyring?: boolean;
    receiverSpacingM?: number;
    seed?: number;
    sourceX?: number;
    sourceY?: number;
    sourceZ?: number;
    widthM?: number;
  };
  'effects.delay.stereo': {
    delayTimeLMs?: number;
    delayTimeRMs?: number;
    feedback?: number;
    pingPong?: number;
    dryWet?: number;
    dampingHz?: number;
    tap3Ms?: number;
    tap4Ms?: number;
    tap1LevelDb?: number;
    tap2LevelDb?: number;
    tap3LevelDb?: number;
    tap4LevelDb?: number;
    tap3Pan?: number;
    tap4Pan?: number;
    invertL?: boolean;
    invertR?: boolean;
    modRateHz?: number;
    modDepthMs?: number;
    modPhaseDeg?: number;
    glideMs?: number;
    crossMode?: 'normal' | 'pingPong' | 'cross';
    mixLaw?: 'crossfade' | 'twoRamps';
    interpolation?: 'linear' | 'lagrange3';
  };
  'effects.filter.vowel': {
    vowel?: number;
    accelMs?: number;
    drive?: number;
    driveOn?: boolean;
    dryWet?: number;
  };
  'effects.gsEfx': {
    byte0?: number;
    byte1?: number;
    byte2?: number;
    byte3?: number;
    byte4?: number;
    byte5?: number;
    byte6?: number;
    byte7?: number;
    byte8?: number;
    byte9?: number;
    byte10?: number;
    byte11?: number;
    byte12?: number;
    byte13?: number;
    byte14?: number;
    byte15?: number;
    byte16?: number;
    byte17?: number;
    byte18?: number;
    byte19?: number;
    realization?: 'modern' | 'classic';
    typeLsb?: number;
    typeMsb?: number;
  };
  'effects.modulation.autoWah': {
    sensitivity?: number;
    minHz?: number;
    maxHz?: number;
    resonance?: number;
    dryWet?: number;
    lfoRateHz?: number;
    lfoDepth?: number;
    filterType?: 'bandpass' | 'lowpass';
    direction?: 'up' | 'down';
    sweepLaw?: 'linearHz' | 'linearOctave';
    attackMs?: number;
    releaseMs?: number;
  };
  'effects.modulation.chorus': {
    rateHz?: number;
    depthMs?: number;
    centerDelayMs?: number;
    dryWet?: number;
    preFilterHz?: number;
    feedback?: number;
    phaseDeg?: number;
    preFilterMode?: 'off' | 'lowPass' | 'highPass';
    interpolation?: 'linear' | 'lagrange3';
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  'effects.modulation.ensemble': {
    rateSlowHz?: number;
    rateFastHz?: number;
    depthSlowMs?: number;
    depthFastMs?: number;
    centerDelayMs?: number;
    toneHz?: number;
    dryWet?: number;
    rateHz?: number;
    preDelayDevMs?: number;
    depthDev?: number;
    panDev?: number;
    interpolation?: 'linear' | 'lagrange3';
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  'effects.modulation.flanger': {
    rateHz?: number;
    depthMs?: number;
    centerDelayMs?: number;
    feedback?: number;
    dryWet?: number;
    preFilterHz?: number;
    phaseDeg?: number;
    stepRateHz?: number;
    preFilterMode?: 'off' | 'lowPass' | 'highPass';
    interpolation?: 'linear' | 'lagrange3';
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  'effects.modulation.phaser': {
    rateHz?: number;
    minHz?: number;
    maxHz?: number;
    dryWet?: number;
    feedback?: number;
    depth?: number;
    mixMode?: 'crossfade' | 'drySum';
    stages?: number;
  };
  'effects.modulation.pitchShifter': {
    semitones?: number;
    dryWet?: number;
    cents?: number;
    pan?: number;
    semitones2?: number;
    cents2?: number;
    level2?: number;
    pan2?: number;
    feedback?: number;
    mixLaw?: 'crossfade' | 'twoRamps';
    interpolation?: 'linear' | 'lagrange3';
    antiAlias?: boolean;
    windowMs?: number;
    preDelayMs?: number;
    preDelay2Ms?: number;
  };
  'effects.modulation.ringModulator': {
    carrierHz?: number;
    dryWet?: number;
    shape?: 'sine' | 'triangle' | 'square' | 'sawUp' | 'sawDown';
    phaseDeg?: number;
    stereoSpread?: number;
    mixLaw?: 'crossfade' | 'twoRamps';
  };
  'effects.modulation.rotary': {
    rateHz?: number;
    depthMs?: number;
    tremolo?: number;
    dryWet?: number;
    drumRateHz?: number;
    hornSlowHz?: number;
    hornFastHz?: number;
    drumSlowHz?: number;
    drumFastHz?: number;
    speed?: number;
    hornLevelDb?: number;
    drumLevelDb?: number;
    interpolation?: 'linear' | 'lagrange3';
    model?: 'classic' | 'geometric';
    stereoSpread?: number;
    accelTauS?: number;
    decelTauS?: number;
    undershootHz?: number;
    drumUndershootHz?: number;
  };
  'effects.modulation.wah': {
    rateHz?: number;
    minHz?: number;
    maxHz?: number;
    resonance?: number;
    dryWet?: number;
    filterType?: 'bandpass' | 'lowpass';
    sweepLaw?: 'linearHz' | 'linearOctave';
  };
  'effects.reverb.convolution': {
    dryWet?: number;
    decaySec?: number;
    irF32Base64?: string;
    preDelayMs?: number;
    seed?: number;
  };
  'effects.reverb.dattorro': {
    decay?: number;
    damping?: number;
    dryWet?: number;
    modRateHz?: number;
    modDepthSamples?: number;
    dampingHz?: number;
    gateThresholdDb?: number;
    gateHoldMs?: number;
    gateType?: 'normal' | 'reverse' | 'sweep1' | 'sweep2';
    preDelayMs?: number;
    character?: number;
    mixLaw?: 'crossfade' | 'twoRamps';
    decaySec?: number;
  };
  'effects.reverb.fdn': {
    decay?: number;
    damping?: number;
    dryWet?: number;
    decaySec?: number;
    hfDamping?: number;
  };
  'effects.reverb.plate': {
    decay?: number;
    damping?: number;
    dryWet?: number;
    modRateHz?: number;
    modDepthSamples?: number;
    dampingHz?: number;
    gateThresholdDb?: number;
    gateHoldMs?: number;
    gateType?: 'normal' | 'reverse' | 'sweep1' | 'sweep2';
    preDelayMs?: number;
    character?: number;
    mixLaw?: 'crossfade' | 'twoRamps';
    decaySec?: number;
  };
  'effects.reverb.room': {
    dryWet?: number;
    absorption?: number;
    airAbsorptionEnabled?: boolean;
    airHumidityPercent?: number;
    airTemperatureC?: number;
    heightM?: number;
    ismOrder?: number;
    lengthM?: number;
    listenerX?: number;
    listenerY?: number;
    listenerZ?: number;
    maxSeconds?: number;
    receiverSpacingM?: number;
    seed?: number;
    sourceX?: number;
    sourceY?: number;
    sourceZ?: number;
    widthM?: number;
  };
  'effects.reverb.velvet': {
    decay?: number;
    reverbTimeS?: number;
    dryWet?: number;
    densityHz?: number;
    decaySec?: number;
    enableShelf?: boolean;
  };
  'eq.bandPass': {
    bandPassFrequencyHz?: number;
    bandPassQ?: number;
    notchFrequencyHz?: number;
    notchQ?: number;
    bandPassEnabled?: boolean;
    notchEnabled?: boolean;
  };
  'eq.cutFilter': {
    highPassFrequencyHz?: number;
    highPassQ?: number;
    lowPassFrequencyHz?: number;
    lowPassQ?: number;
    highPassEnabled?: boolean;
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
    lowPassEnabled?: boolean;
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
  'eq.dynamic': {
    'band0.frequencyHz'?: number;
    'band0.staticGainDb'?: number;
    'band0.q'?: number;
    'band0.thresholdDb'?: number;
    'band0.ratio'?: number;
    'band0.rangeDb'?: number;
    'band0.sidechainQ'?: number;
    'band0.sidechainFreqHz'?: number;
    'band0.attackMs'?: number;
    'band0.releaseMs'?: number;
    'band0.detectorDelayMs'?: number;
    'band1.frequencyHz'?: number;
    'band1.staticGainDb'?: number;
    'band1.q'?: number;
    'band1.thresholdDb'?: number;
    'band1.ratio'?: number;
    'band1.rangeDb'?: number;
    'band1.sidechainQ'?: number;
    'band1.sidechainFreqHz'?: number;
    'band1.attackMs'?: number;
    'band1.releaseMs'?: number;
    'band1.detectorDelayMs'?: number;
    'band2.frequencyHz'?: number;
    'band2.staticGainDb'?: number;
    'band2.q'?: number;
    'band2.thresholdDb'?: number;
    'band2.ratio'?: number;
    'band2.rangeDb'?: number;
    'band2.sidechainQ'?: number;
    'band2.sidechainFreqHz'?: number;
    'band2.attackMs'?: number;
    'band2.releaseMs'?: number;
    'band2.detectorDelayMs'?: number;
    'band3.frequencyHz'?: number;
    'band3.staticGainDb'?: number;
    'band3.q'?: number;
    'band3.thresholdDb'?: number;
    'band3.ratio'?: number;
    'band3.rangeDb'?: number;
    'band3.sidechainQ'?: number;
    'band3.sidechainFreqHz'?: number;
    'band3.attackMs'?: number;
    'band3.releaseMs'?: number;
    'band3.detectorDelayMs'?: number;
    'band4.frequencyHz'?: number;
    'band4.staticGainDb'?: number;
    'band4.q'?: number;
    'band4.thresholdDb'?: number;
    'band4.ratio'?: number;
    'band4.rangeDb'?: number;
    'band4.sidechainQ'?: number;
    'band4.sidechainFreqHz'?: number;
    'band4.attackMs'?: number;
    'band4.releaseMs'?: number;
    'band4.detectorDelayMs'?: number;
    'band5.frequencyHz'?: number;
    'band5.staticGainDb'?: number;
    'band5.q'?: number;
    'band5.thresholdDb'?: number;
    'band5.ratio'?: number;
    'band5.rangeDb'?: number;
    'band5.sidechainQ'?: number;
    'band5.sidechainFreqHz'?: number;
    'band5.attackMs'?: number;
    'band5.releaseMs'?: number;
    'band5.detectorDelayMs'?: number;
    'band6.frequencyHz'?: number;
    'band6.staticGainDb'?: number;
    'band6.q'?: number;
    'band6.thresholdDb'?: number;
    'band6.ratio'?: number;
    'band6.rangeDb'?: number;
    'band6.sidechainQ'?: number;
    'band6.sidechainFreqHz'?: number;
    'band6.attackMs'?: number;
    'band6.releaseMs'?: number;
    'band6.detectorDelayMs'?: number;
    'band7.frequencyHz'?: number;
    'band7.staticGainDb'?: number;
    'band7.q'?: number;
    'band7.thresholdDb'?: number;
    'band7.ratio'?: number;
    'band7.rangeDb'?: number;
    'band7.sidechainQ'?: number;
    'band7.sidechainFreqHz'?: number;
    'band7.attackMs'?: number;
    'band7.releaseMs'?: number;
    'band7.detectorDelayMs'?: number;
    'band0.enabled'?: boolean;
    'band0.lookaheadMs'?: number;
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.enabled'?: boolean;
    'band1.lookaheadMs'?: number;
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.enabled'?: boolean;
    'band2.lookaheadMs'?: number;
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.enabled'?: boolean;
    'band3.lookaheadMs'?: number;
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.enabled'?: boolean;
    'band4.lookaheadMs'?: number;
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.enabled'?: boolean;
    'band5.lookaheadMs'?: number;
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.enabled'?: boolean;
    'band6.lookaheadMs'?: number;
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.enabled'?: boolean;
    'band7.lookaheadMs'?: number;
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
  'eq.equalizer': {
    'band0.frequencyHz'?: number;
    'band0.gainDb'?: number;
    'band0.q'?: number;
    'band1.frequencyHz'?: number;
    'band1.gainDb'?: number;
    'band1.q'?: number;
    'band2.frequencyHz'?: number;
    'band2.gainDb'?: number;
    'band2.q'?: number;
    'band3.frequencyHz'?: number;
    'band3.gainDb'?: number;
    'band3.q'?: number;
    'band4.frequencyHz'?: number;
    'band4.gainDb'?: number;
    'band4.q'?: number;
    'band5.frequencyHz'?: number;
    'band5.gainDb'?: number;
    'band5.q'?: number;
    'band6.frequencyHz'?: number;
    'band6.gainDb'?: number;
    'band6.q'?: number;
    'band7.frequencyHz'?: number;
    'band7.gainDb'?: number;
    'band7.q'?: number;
    'band8.frequencyHz'?: number;
    'band8.gainDb'?: number;
    'band8.q'?: number;
    'band9.frequencyHz'?: number;
    'band9.gainDb'?: number;
    'band9.q'?: number;
    'band10.frequencyHz'?: number;
    'band10.gainDb'?: number;
    'band10.q'?: number;
    'band11.frequencyHz'?: number;
    'band11.gainDb'?: number;
    'band11.q'?: number;
    'band12.frequencyHz'?: number;
    'band12.gainDb'?: number;
    'band12.q'?: number;
    'band13.frequencyHz'?: number;
    'band13.gainDb'?: number;
    'band13.q'?: number;
    'band14.frequencyHz'?: number;
    'band14.gainDb'?: number;
    'band14.q'?: number;
    'band15.frequencyHz'?: number;
    'band15.gainDb'?: number;
    'band15.q'?: number;
    'band16.frequencyHz'?: number;
    'band16.gainDb'?: number;
    'band16.q'?: number;
    'band17.frequencyHz'?: number;
    'band17.gainDb'?: number;
    'band17.q'?: number;
    'band18.frequencyHz'?: number;
    'band18.gainDb'?: number;
    'band18.q'?: number;
    'band19.frequencyHz'?: number;
    'band19.gainDb'?: number;
    'band19.q'?: number;
    'band20.frequencyHz'?: number;
    'band20.gainDb'?: number;
    'band20.q'?: number;
    'band21.frequencyHz'?: number;
    'band21.gainDb'?: number;
    'band21.q'?: number;
    'band22.frequencyHz'?: number;
    'band22.gainDb'?: number;
    'band22.q'?: number;
    'band23.frequencyHz'?: number;
    'band23.gainDb'?: number;
    'band23.q'?: number;
    autoGain?: boolean;
    'band0.attackMs'?: number;
    'band0.autoThreshold'?: boolean;
    'band0.bypassed'?: boolean;
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    'band0.detectorDelayMs'?: number;
    'band0.dynamic'?: boolean;
    'band0.enabled'?: boolean;
    'band0.lookaheadMs'?: number;
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band0.proportionalQ'?: boolean;
    'band0.proportionalQStrength'?: number;
    'band0.rangeDb'?: number;
    'band0.ratio'?: number;
    'band0.releaseMs'?: number;
    'band0.sidechainFreqHz'?: number;
    'band0.sidechainQ'?: number;
    'band0.slopeDbOct'?: number;
    'band0.soloed'?: boolean;
    'band0.thresholdDb'?: number;
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
    'band1.attackMs'?: number;
    'band1.autoThreshold'?: boolean;
    'band1.bypassed'?: boolean;
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    'band1.detectorDelayMs'?: number;
    'band1.dynamic'?: boolean;
    'band1.enabled'?: boolean;
    'band1.lookaheadMs'?: number;
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band1.proportionalQ'?: boolean;
    'band1.proportionalQStrength'?: number;
    'band1.rangeDb'?: number;
    'band1.ratio'?: number;
    'band1.releaseMs'?: number;
    'band1.sidechainFreqHz'?: number;
    'band1.sidechainQ'?: number;
    'band1.slopeDbOct'?: number;
    'band1.soloed'?: boolean;
    'band1.thresholdDb'?: number;
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
    'band10.attackMs'?: number;
    'band10.autoThreshold'?: boolean;
    'band10.bypassed'?: boolean;
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    'band10.detectorDelayMs'?: number;
    'band10.dynamic'?: boolean;
    'band10.enabled'?: boolean;
    'band10.lookaheadMs'?: number;
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band10.proportionalQ'?: boolean;
    'band10.proportionalQStrength'?: number;
    'band10.rangeDb'?: number;
    'band10.ratio'?: number;
    'band10.releaseMs'?: number;
    'band10.sidechainFreqHz'?: number;
    'band10.sidechainQ'?: number;
    'band10.slopeDbOct'?: number;
    'band10.soloed'?: boolean;
    'band10.thresholdDb'?: number;
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
    'band11.attackMs'?: number;
    'band11.autoThreshold'?: boolean;
    'band11.bypassed'?: boolean;
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    'band11.detectorDelayMs'?: number;
    'band11.dynamic'?: boolean;
    'band11.enabled'?: boolean;
    'band11.lookaheadMs'?: number;
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band11.proportionalQ'?: boolean;
    'band11.proportionalQStrength'?: number;
    'band11.rangeDb'?: number;
    'band11.ratio'?: number;
    'band11.releaseMs'?: number;
    'band11.sidechainFreqHz'?: number;
    'band11.sidechainQ'?: number;
    'band11.slopeDbOct'?: number;
    'band11.soloed'?: boolean;
    'band11.thresholdDb'?: number;
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
    'band12.attackMs'?: number;
    'band12.autoThreshold'?: boolean;
    'band12.bypassed'?: boolean;
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    'band12.detectorDelayMs'?: number;
    'band12.dynamic'?: boolean;
    'band12.enabled'?: boolean;
    'band12.lookaheadMs'?: number;
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band12.proportionalQ'?: boolean;
    'band12.proportionalQStrength'?: number;
    'band12.rangeDb'?: number;
    'band12.ratio'?: number;
    'band12.releaseMs'?: number;
    'band12.sidechainFreqHz'?: number;
    'band12.sidechainQ'?: number;
    'band12.slopeDbOct'?: number;
    'band12.soloed'?: boolean;
    'band12.thresholdDb'?: number;
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
    'band13.attackMs'?: number;
    'band13.autoThreshold'?: boolean;
    'band13.bypassed'?: boolean;
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    'band13.detectorDelayMs'?: number;
    'band13.dynamic'?: boolean;
    'band13.enabled'?: boolean;
    'band13.lookaheadMs'?: number;
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band13.proportionalQ'?: boolean;
    'band13.proportionalQStrength'?: number;
    'band13.rangeDb'?: number;
    'band13.ratio'?: number;
    'band13.releaseMs'?: number;
    'band13.sidechainFreqHz'?: number;
    'band13.sidechainQ'?: number;
    'band13.slopeDbOct'?: number;
    'band13.soloed'?: boolean;
    'band13.thresholdDb'?: number;
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
    'band14.attackMs'?: number;
    'band14.autoThreshold'?: boolean;
    'band14.bypassed'?: boolean;
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    'band14.detectorDelayMs'?: number;
    'band14.dynamic'?: boolean;
    'band14.enabled'?: boolean;
    'band14.lookaheadMs'?: number;
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band14.proportionalQ'?: boolean;
    'band14.proportionalQStrength'?: number;
    'band14.rangeDb'?: number;
    'band14.ratio'?: number;
    'band14.releaseMs'?: number;
    'band14.sidechainFreqHz'?: number;
    'band14.sidechainQ'?: number;
    'band14.slopeDbOct'?: number;
    'band14.soloed'?: boolean;
    'band14.thresholdDb'?: number;
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
    'band15.attackMs'?: number;
    'band15.autoThreshold'?: boolean;
    'band15.bypassed'?: boolean;
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    'band15.detectorDelayMs'?: number;
    'band15.dynamic'?: boolean;
    'band15.enabled'?: boolean;
    'band15.lookaheadMs'?: number;
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band15.proportionalQ'?: boolean;
    'band15.proportionalQStrength'?: number;
    'band15.rangeDb'?: number;
    'band15.ratio'?: number;
    'band15.releaseMs'?: number;
    'band15.sidechainFreqHz'?: number;
    'band15.sidechainQ'?: number;
    'band15.slopeDbOct'?: number;
    'band15.soloed'?: boolean;
    'band15.thresholdDb'?: number;
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
    'band16.attackMs'?: number;
    'band16.autoThreshold'?: boolean;
    'band16.bypassed'?: boolean;
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    'band16.detectorDelayMs'?: number;
    'band16.dynamic'?: boolean;
    'band16.enabled'?: boolean;
    'band16.lookaheadMs'?: number;
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band16.proportionalQ'?: boolean;
    'band16.proportionalQStrength'?: number;
    'band16.rangeDb'?: number;
    'band16.ratio'?: number;
    'band16.releaseMs'?: number;
    'band16.sidechainFreqHz'?: number;
    'band16.sidechainQ'?: number;
    'band16.slopeDbOct'?: number;
    'band16.soloed'?: boolean;
    'band16.thresholdDb'?: number;
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
    'band17.attackMs'?: number;
    'band17.autoThreshold'?: boolean;
    'band17.bypassed'?: boolean;
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    'band17.detectorDelayMs'?: number;
    'band17.dynamic'?: boolean;
    'band17.enabled'?: boolean;
    'band17.lookaheadMs'?: number;
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band17.proportionalQ'?: boolean;
    'band17.proportionalQStrength'?: number;
    'band17.rangeDb'?: number;
    'band17.ratio'?: number;
    'band17.releaseMs'?: number;
    'band17.sidechainFreqHz'?: number;
    'band17.sidechainQ'?: number;
    'band17.slopeDbOct'?: number;
    'band17.soloed'?: boolean;
    'band17.thresholdDb'?: number;
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
    'band18.attackMs'?: number;
    'band18.autoThreshold'?: boolean;
    'band18.bypassed'?: boolean;
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    'band18.detectorDelayMs'?: number;
    'band18.dynamic'?: boolean;
    'band18.enabled'?: boolean;
    'band18.lookaheadMs'?: number;
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band18.proportionalQ'?: boolean;
    'band18.proportionalQStrength'?: number;
    'band18.rangeDb'?: number;
    'band18.ratio'?: number;
    'band18.releaseMs'?: number;
    'band18.sidechainFreqHz'?: number;
    'band18.sidechainQ'?: number;
    'band18.slopeDbOct'?: number;
    'band18.soloed'?: boolean;
    'band18.thresholdDb'?: number;
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
    'band19.attackMs'?: number;
    'band19.autoThreshold'?: boolean;
    'band19.bypassed'?: boolean;
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    'band19.detectorDelayMs'?: number;
    'band19.dynamic'?: boolean;
    'band19.enabled'?: boolean;
    'band19.lookaheadMs'?: number;
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band19.proportionalQ'?: boolean;
    'band19.proportionalQStrength'?: number;
    'band19.rangeDb'?: number;
    'band19.ratio'?: number;
    'band19.releaseMs'?: number;
    'band19.sidechainFreqHz'?: number;
    'band19.sidechainQ'?: number;
    'band19.slopeDbOct'?: number;
    'band19.soloed'?: boolean;
    'band19.thresholdDb'?: number;
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
    'band2.attackMs'?: number;
    'band2.autoThreshold'?: boolean;
    'band2.bypassed'?: boolean;
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    'band2.detectorDelayMs'?: number;
    'band2.dynamic'?: boolean;
    'band2.enabled'?: boolean;
    'band2.lookaheadMs'?: number;
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band2.proportionalQ'?: boolean;
    'band2.proportionalQStrength'?: number;
    'band2.rangeDb'?: number;
    'band2.ratio'?: number;
    'band2.releaseMs'?: number;
    'band2.sidechainFreqHz'?: number;
    'band2.sidechainQ'?: number;
    'band2.slopeDbOct'?: number;
    'band2.soloed'?: boolean;
    'band2.thresholdDb'?: number;
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
    'band20.attackMs'?: number;
    'band20.autoThreshold'?: boolean;
    'band20.bypassed'?: boolean;
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    'band20.detectorDelayMs'?: number;
    'band20.dynamic'?: boolean;
    'band20.enabled'?: boolean;
    'band20.lookaheadMs'?: number;
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band20.proportionalQ'?: boolean;
    'band20.proportionalQStrength'?: number;
    'band20.rangeDb'?: number;
    'band20.ratio'?: number;
    'band20.releaseMs'?: number;
    'band20.sidechainFreqHz'?: number;
    'band20.sidechainQ'?: number;
    'band20.slopeDbOct'?: number;
    'band20.soloed'?: boolean;
    'band20.thresholdDb'?: number;
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
    'band21.attackMs'?: number;
    'band21.autoThreshold'?: boolean;
    'band21.bypassed'?: boolean;
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    'band21.detectorDelayMs'?: number;
    'band21.dynamic'?: boolean;
    'band21.enabled'?: boolean;
    'band21.lookaheadMs'?: number;
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band21.proportionalQ'?: boolean;
    'band21.proportionalQStrength'?: number;
    'band21.rangeDb'?: number;
    'band21.ratio'?: number;
    'band21.releaseMs'?: number;
    'band21.sidechainFreqHz'?: number;
    'band21.sidechainQ'?: number;
    'band21.slopeDbOct'?: number;
    'band21.soloed'?: boolean;
    'band21.thresholdDb'?: number;
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
    'band22.attackMs'?: number;
    'band22.autoThreshold'?: boolean;
    'band22.bypassed'?: boolean;
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    'band22.detectorDelayMs'?: number;
    'band22.dynamic'?: boolean;
    'band22.enabled'?: boolean;
    'band22.lookaheadMs'?: number;
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band22.proportionalQ'?: boolean;
    'band22.proportionalQStrength'?: number;
    'band22.rangeDb'?: number;
    'band22.ratio'?: number;
    'band22.releaseMs'?: number;
    'band22.sidechainFreqHz'?: number;
    'band22.sidechainQ'?: number;
    'band22.slopeDbOct'?: number;
    'band22.soloed'?: boolean;
    'band22.thresholdDb'?: number;
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
    'band23.attackMs'?: number;
    'band23.autoThreshold'?: boolean;
    'band23.bypassed'?: boolean;
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    'band23.detectorDelayMs'?: number;
    'band23.dynamic'?: boolean;
    'band23.enabled'?: boolean;
    'band23.lookaheadMs'?: number;
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band23.proportionalQ'?: boolean;
    'band23.proportionalQStrength'?: number;
    'band23.rangeDb'?: number;
    'band23.ratio'?: number;
    'band23.releaseMs'?: number;
    'band23.sidechainFreqHz'?: number;
    'band23.sidechainQ'?: number;
    'band23.slopeDbOct'?: number;
    'band23.soloed'?: boolean;
    'band23.thresholdDb'?: number;
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
    'band3.attackMs'?: number;
    'band3.autoThreshold'?: boolean;
    'band3.bypassed'?: boolean;
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    'band3.detectorDelayMs'?: number;
    'band3.dynamic'?: boolean;
    'band3.enabled'?: boolean;
    'band3.lookaheadMs'?: number;
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band3.proportionalQ'?: boolean;
    'band3.proportionalQStrength'?: number;
    'band3.rangeDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.sidechainFreqHz'?: number;
    'band3.sidechainQ'?: number;
    'band3.slopeDbOct'?: number;
    'band3.soloed'?: boolean;
    'band3.thresholdDb'?: number;
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
    'band4.attackMs'?: number;
    'band4.autoThreshold'?: boolean;
    'band4.bypassed'?: boolean;
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    'band4.detectorDelayMs'?: number;
    'band4.dynamic'?: boolean;
    'band4.enabled'?: boolean;
    'band4.lookaheadMs'?: number;
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band4.proportionalQ'?: boolean;
    'band4.proportionalQStrength'?: number;
    'band4.rangeDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.sidechainFreqHz'?: number;
    'band4.sidechainQ'?: number;
    'band4.slopeDbOct'?: number;
    'band4.soloed'?: boolean;
    'band4.thresholdDb'?: number;
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
    'band5.attackMs'?: number;
    'band5.autoThreshold'?: boolean;
    'band5.bypassed'?: boolean;
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    'band5.detectorDelayMs'?: number;
    'band5.dynamic'?: boolean;
    'band5.enabled'?: boolean;
    'band5.lookaheadMs'?: number;
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band5.proportionalQ'?: boolean;
    'band5.proportionalQStrength'?: number;
    'band5.rangeDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.sidechainFreqHz'?: number;
    'band5.sidechainQ'?: number;
    'band5.slopeDbOct'?: number;
    'band5.soloed'?: boolean;
    'band5.thresholdDb'?: number;
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
    'band6.attackMs'?: number;
    'band6.autoThreshold'?: boolean;
    'band6.bypassed'?: boolean;
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    'band6.detectorDelayMs'?: number;
    'band6.dynamic'?: boolean;
    'band6.enabled'?: boolean;
    'band6.lookaheadMs'?: number;
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band6.proportionalQ'?: boolean;
    'band6.proportionalQStrength'?: number;
    'band6.rangeDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.sidechainFreqHz'?: number;
    'band6.sidechainQ'?: number;
    'band6.slopeDbOct'?: number;
    'band6.soloed'?: boolean;
    'band6.thresholdDb'?: number;
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
    'band7.attackMs'?: number;
    'band7.autoThreshold'?: boolean;
    'band7.bypassed'?: boolean;
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    'band7.detectorDelayMs'?: number;
    'band7.dynamic'?: boolean;
    'band7.enabled'?: boolean;
    'band7.lookaheadMs'?: number;
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band7.proportionalQ'?: boolean;
    'band7.proportionalQStrength'?: number;
    'band7.rangeDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.sidechainFreqHz'?: number;
    'band7.sidechainQ'?: number;
    'band7.slopeDbOct'?: number;
    'band7.soloed'?: boolean;
    'band7.thresholdDb'?: number;
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
    'band8.attackMs'?: number;
    'band8.autoThreshold'?: boolean;
    'band8.bypassed'?: boolean;
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    'band8.detectorDelayMs'?: number;
    'band8.dynamic'?: boolean;
    'band8.enabled'?: boolean;
    'band8.lookaheadMs'?: number;
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band8.proportionalQ'?: boolean;
    'band8.proportionalQStrength'?: number;
    'band8.rangeDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.sidechainFreqHz'?: number;
    'band8.sidechainQ'?: number;
    'band8.slopeDbOct'?: number;
    'band8.soloed'?: boolean;
    'band8.thresholdDb'?: number;
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
    'band9.attackMs'?: number;
    'band9.autoThreshold'?: boolean;
    'band9.bypassed'?: boolean;
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    'band9.detectorDelayMs'?: number;
    'band9.dynamic'?: boolean;
    'band9.enabled'?: boolean;
    'band9.lookaheadMs'?: number;
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band9.proportionalQ'?: boolean;
    'band9.proportionalQStrength'?: number;
    'band9.rangeDb'?: number;
    'band9.ratio'?: number;
    'band9.releaseMs'?: number;
    'band9.sidechainFreqHz'?: number;
    'band9.sidechainQ'?: number;
    'band9.slopeDbOct'?: number;
    'band9.soloed'?: boolean;
    'band9.thresholdDb'?: number;
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
    fftSize?: number;
    gainScale?: number;
    kernelSize?: number;
    outputGainDb?: number;
    outputPan?: number;
    partitionSize?: number;
    phaseMode?: 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    resolution?: 'custom' | 'low' | 'medium' | 'high' | 'veryHigh' | 'maximum';
    usePartitionedConvolution?: boolean;
  };
  'eq.graphic': {
    band0GainDb?: number;
    band1GainDb?: number;
    band2GainDb?: number;
    band3GainDb?: number;
    band4GainDb?: number;
    band5GainDb?: number;
    band6GainDb?: number;
    band7GainDb?: number;
    band8GainDb?: number;
    band9GainDb?: number;
    band10GainDb?: number;
    band11GainDb?: number;
    band12GainDb?: number;
    band13GainDb?: number;
    band14GainDb?: number;
    band15GainDb?: number;
    band16GainDb?: number;
    band17GainDb?: number;
    band18GainDb?: number;
    band19GainDb?: number;
    band20GainDb?: number;
    band21GainDb?: number;
    band22GainDb?: number;
    band23GainDb?: number;
    band24GainDb?: number;
    band25GainDb?: number;
    band26GainDb?: number;
    band27GainDb?: number;
    band28GainDb?: number;
    band29GainDb?: number;
    band30GainDb?: number;
    q?: number;
  };
  'eq.linearPhase': {
    'band0.frequencyHz'?: number;
    'band0.gainDb'?: number;
    'band0.q'?: number;
    'band1.frequencyHz'?: number;
    'band1.gainDb'?: number;
    'band1.q'?: number;
    'band2.frequencyHz'?: number;
    'band2.gainDb'?: number;
    'band2.q'?: number;
    'band3.frequencyHz'?: number;
    'band3.gainDb'?: number;
    'band3.q'?: number;
    'band4.frequencyHz'?: number;
    'band4.gainDb'?: number;
    'band4.q'?: number;
    'band5.frequencyHz'?: number;
    'band5.gainDb'?: number;
    'band5.q'?: number;
    'band6.frequencyHz'?: number;
    'band6.gainDb'?: number;
    'band6.q'?: number;
    'band7.frequencyHz'?: number;
    'band7.gainDb'?: number;
    'band7.q'?: number;
    'band8.frequencyHz'?: number;
    'band8.gainDb'?: number;
    'band8.q'?: number;
    'band9.frequencyHz'?: number;
    'band9.gainDb'?: number;
    'band9.q'?: number;
    'band10.frequencyHz'?: number;
    'band10.gainDb'?: number;
    'band10.q'?: number;
    'band11.frequencyHz'?: number;
    'band11.gainDb'?: number;
    'band11.q'?: number;
    'band12.frequencyHz'?: number;
    'band12.gainDb'?: number;
    'band12.q'?: number;
    'band13.frequencyHz'?: number;
    'band13.gainDb'?: number;
    'band13.q'?: number;
    'band14.frequencyHz'?: number;
    'band14.gainDb'?: number;
    'band14.q'?: number;
    'band15.frequencyHz'?: number;
    'band15.gainDb'?: number;
    'band15.q'?: number;
    'band16.frequencyHz'?: number;
    'band16.gainDb'?: number;
    'band16.q'?: number;
    'band17.frequencyHz'?: number;
    'band17.gainDb'?: number;
    'band17.q'?: number;
    'band18.frequencyHz'?: number;
    'band18.gainDb'?: number;
    'band18.q'?: number;
    'band19.frequencyHz'?: number;
    'band19.gainDb'?: number;
    'band19.q'?: number;
    'band20.frequencyHz'?: number;
    'band20.gainDb'?: number;
    'band20.q'?: number;
    'band21.frequencyHz'?: number;
    'band21.gainDb'?: number;
    'band21.q'?: number;
    'band22.frequencyHz'?: number;
    'band22.gainDb'?: number;
    'band22.q'?: number;
    'band23.frequencyHz'?: number;
    'band23.gainDb'?: number;
    'band23.q'?: number;
    'band0.attackMs'?: number;
    'band0.autoThreshold'?: boolean;
    'band0.bypassed'?: boolean;
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    'band0.detectorDelayMs'?: number;
    'band0.dynamic'?: boolean;
    'band0.enabled'?: boolean;
    'band0.lookaheadMs'?: number;
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band0.proportionalQ'?: boolean;
    'band0.proportionalQStrength'?: number;
    'band0.rangeDb'?: number;
    'band0.ratio'?: number;
    'band0.releaseMs'?: number;
    'band0.sidechainFreqHz'?: number;
    'band0.sidechainQ'?: number;
    'band0.slopeDbOct'?: number;
    'band0.soloed'?: boolean;
    'band0.thresholdDb'?: number;
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band1.attackMs'?: number;
    'band1.autoThreshold'?: boolean;
    'band1.bypassed'?: boolean;
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    'band1.detectorDelayMs'?: number;
    'band1.dynamic'?: boolean;
    'band1.enabled'?: boolean;
    'band1.lookaheadMs'?: number;
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band1.proportionalQ'?: boolean;
    'band1.proportionalQStrength'?: number;
    'band1.rangeDb'?: number;
    'band1.ratio'?: number;
    'band1.releaseMs'?: number;
    'band1.sidechainFreqHz'?: number;
    'band1.sidechainQ'?: number;
    'band1.slopeDbOct'?: number;
    'band1.soloed'?: boolean;
    'band1.thresholdDb'?: number;
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band10.attackMs'?: number;
    'band10.autoThreshold'?: boolean;
    'band10.bypassed'?: boolean;
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    'band10.detectorDelayMs'?: number;
    'band10.dynamic'?: boolean;
    'band10.enabled'?: boolean;
    'band10.lookaheadMs'?: number;
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band10.proportionalQ'?: boolean;
    'band10.proportionalQStrength'?: number;
    'band10.rangeDb'?: number;
    'band10.ratio'?: number;
    'band10.releaseMs'?: number;
    'band10.sidechainFreqHz'?: number;
    'band10.sidechainQ'?: number;
    'band10.slopeDbOct'?: number;
    'band10.soloed'?: boolean;
    'band10.thresholdDb'?: number;
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band11.attackMs'?: number;
    'band11.autoThreshold'?: boolean;
    'band11.bypassed'?: boolean;
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    'band11.detectorDelayMs'?: number;
    'band11.dynamic'?: boolean;
    'band11.enabled'?: boolean;
    'band11.lookaheadMs'?: number;
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band11.proportionalQ'?: boolean;
    'band11.proportionalQStrength'?: number;
    'band11.rangeDb'?: number;
    'band11.ratio'?: number;
    'band11.releaseMs'?: number;
    'band11.sidechainFreqHz'?: number;
    'band11.sidechainQ'?: number;
    'band11.slopeDbOct'?: number;
    'band11.soloed'?: boolean;
    'band11.thresholdDb'?: number;
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band12.attackMs'?: number;
    'band12.autoThreshold'?: boolean;
    'band12.bypassed'?: boolean;
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    'band12.detectorDelayMs'?: number;
    'band12.dynamic'?: boolean;
    'band12.enabled'?: boolean;
    'band12.lookaheadMs'?: number;
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band12.proportionalQ'?: boolean;
    'band12.proportionalQStrength'?: number;
    'band12.rangeDb'?: number;
    'band12.ratio'?: number;
    'band12.releaseMs'?: number;
    'band12.sidechainFreqHz'?: number;
    'band12.sidechainQ'?: number;
    'band12.slopeDbOct'?: number;
    'band12.soloed'?: boolean;
    'band12.thresholdDb'?: number;
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band13.attackMs'?: number;
    'band13.autoThreshold'?: boolean;
    'band13.bypassed'?: boolean;
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    'band13.detectorDelayMs'?: number;
    'band13.dynamic'?: boolean;
    'band13.enabled'?: boolean;
    'band13.lookaheadMs'?: number;
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band13.proportionalQ'?: boolean;
    'band13.proportionalQStrength'?: number;
    'band13.rangeDb'?: number;
    'band13.ratio'?: number;
    'band13.releaseMs'?: number;
    'band13.sidechainFreqHz'?: number;
    'band13.sidechainQ'?: number;
    'band13.slopeDbOct'?: number;
    'band13.soloed'?: boolean;
    'band13.thresholdDb'?: number;
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band14.attackMs'?: number;
    'band14.autoThreshold'?: boolean;
    'band14.bypassed'?: boolean;
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    'band14.detectorDelayMs'?: number;
    'band14.dynamic'?: boolean;
    'band14.enabled'?: boolean;
    'band14.lookaheadMs'?: number;
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band14.proportionalQ'?: boolean;
    'band14.proportionalQStrength'?: number;
    'band14.rangeDb'?: number;
    'band14.ratio'?: number;
    'band14.releaseMs'?: number;
    'band14.sidechainFreqHz'?: number;
    'band14.sidechainQ'?: number;
    'band14.slopeDbOct'?: number;
    'band14.soloed'?: boolean;
    'band14.thresholdDb'?: number;
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band15.attackMs'?: number;
    'band15.autoThreshold'?: boolean;
    'band15.bypassed'?: boolean;
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    'band15.detectorDelayMs'?: number;
    'band15.dynamic'?: boolean;
    'band15.enabled'?: boolean;
    'band15.lookaheadMs'?: number;
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band15.proportionalQ'?: boolean;
    'band15.proportionalQStrength'?: number;
    'band15.rangeDb'?: number;
    'band15.ratio'?: number;
    'band15.releaseMs'?: number;
    'band15.sidechainFreqHz'?: number;
    'band15.sidechainQ'?: number;
    'band15.slopeDbOct'?: number;
    'band15.soloed'?: boolean;
    'band15.thresholdDb'?: number;
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band16.attackMs'?: number;
    'band16.autoThreshold'?: boolean;
    'band16.bypassed'?: boolean;
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    'band16.detectorDelayMs'?: number;
    'band16.dynamic'?: boolean;
    'band16.enabled'?: boolean;
    'band16.lookaheadMs'?: number;
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band16.proportionalQ'?: boolean;
    'band16.proportionalQStrength'?: number;
    'band16.rangeDb'?: number;
    'band16.ratio'?: number;
    'band16.releaseMs'?: number;
    'band16.sidechainFreqHz'?: number;
    'band16.sidechainQ'?: number;
    'band16.slopeDbOct'?: number;
    'band16.soloed'?: boolean;
    'band16.thresholdDb'?: number;
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band17.attackMs'?: number;
    'band17.autoThreshold'?: boolean;
    'band17.bypassed'?: boolean;
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    'band17.detectorDelayMs'?: number;
    'band17.dynamic'?: boolean;
    'band17.enabled'?: boolean;
    'band17.lookaheadMs'?: number;
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band17.proportionalQ'?: boolean;
    'band17.proportionalQStrength'?: number;
    'band17.rangeDb'?: number;
    'band17.ratio'?: number;
    'band17.releaseMs'?: number;
    'band17.sidechainFreqHz'?: number;
    'band17.sidechainQ'?: number;
    'band17.slopeDbOct'?: number;
    'band17.soloed'?: boolean;
    'band17.thresholdDb'?: number;
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band18.attackMs'?: number;
    'band18.autoThreshold'?: boolean;
    'band18.bypassed'?: boolean;
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    'band18.detectorDelayMs'?: number;
    'band18.dynamic'?: boolean;
    'band18.enabled'?: boolean;
    'band18.lookaheadMs'?: number;
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band18.proportionalQ'?: boolean;
    'band18.proportionalQStrength'?: number;
    'band18.rangeDb'?: number;
    'band18.ratio'?: number;
    'band18.releaseMs'?: number;
    'band18.sidechainFreqHz'?: number;
    'band18.sidechainQ'?: number;
    'band18.slopeDbOct'?: number;
    'band18.soloed'?: boolean;
    'band18.thresholdDb'?: number;
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band19.attackMs'?: number;
    'band19.autoThreshold'?: boolean;
    'band19.bypassed'?: boolean;
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    'band19.detectorDelayMs'?: number;
    'band19.dynamic'?: boolean;
    'band19.enabled'?: boolean;
    'band19.lookaheadMs'?: number;
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band19.proportionalQ'?: boolean;
    'band19.proportionalQStrength'?: number;
    'band19.rangeDb'?: number;
    'band19.ratio'?: number;
    'band19.releaseMs'?: number;
    'band19.sidechainFreqHz'?: number;
    'band19.sidechainQ'?: number;
    'band19.slopeDbOct'?: number;
    'band19.soloed'?: boolean;
    'band19.thresholdDb'?: number;
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band2.attackMs'?: number;
    'band2.autoThreshold'?: boolean;
    'band2.bypassed'?: boolean;
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    'band2.detectorDelayMs'?: number;
    'band2.dynamic'?: boolean;
    'band2.enabled'?: boolean;
    'band2.lookaheadMs'?: number;
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band2.proportionalQ'?: boolean;
    'band2.proportionalQStrength'?: number;
    'band2.rangeDb'?: number;
    'band2.ratio'?: number;
    'band2.releaseMs'?: number;
    'band2.sidechainFreqHz'?: number;
    'band2.sidechainQ'?: number;
    'band2.slopeDbOct'?: number;
    'band2.soloed'?: boolean;
    'band2.thresholdDb'?: number;
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band20.attackMs'?: number;
    'band20.autoThreshold'?: boolean;
    'band20.bypassed'?: boolean;
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    'band20.detectorDelayMs'?: number;
    'band20.dynamic'?: boolean;
    'band20.enabled'?: boolean;
    'band20.lookaheadMs'?: number;
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band20.proportionalQ'?: boolean;
    'band20.proportionalQStrength'?: number;
    'band20.rangeDb'?: number;
    'band20.ratio'?: number;
    'band20.releaseMs'?: number;
    'band20.sidechainFreqHz'?: number;
    'band20.sidechainQ'?: number;
    'band20.slopeDbOct'?: number;
    'band20.soloed'?: boolean;
    'band20.thresholdDb'?: number;
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band21.attackMs'?: number;
    'band21.autoThreshold'?: boolean;
    'band21.bypassed'?: boolean;
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    'band21.detectorDelayMs'?: number;
    'band21.dynamic'?: boolean;
    'band21.enabled'?: boolean;
    'band21.lookaheadMs'?: number;
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band21.proportionalQ'?: boolean;
    'band21.proportionalQStrength'?: number;
    'band21.rangeDb'?: number;
    'band21.ratio'?: number;
    'band21.releaseMs'?: number;
    'band21.sidechainFreqHz'?: number;
    'band21.sidechainQ'?: number;
    'band21.slopeDbOct'?: number;
    'band21.soloed'?: boolean;
    'band21.thresholdDb'?: number;
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band22.attackMs'?: number;
    'band22.autoThreshold'?: boolean;
    'band22.bypassed'?: boolean;
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    'band22.detectorDelayMs'?: number;
    'band22.dynamic'?: boolean;
    'band22.enabled'?: boolean;
    'band22.lookaheadMs'?: number;
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band22.proportionalQ'?: boolean;
    'band22.proportionalQStrength'?: number;
    'band22.rangeDb'?: number;
    'band22.ratio'?: number;
    'band22.releaseMs'?: number;
    'band22.sidechainFreqHz'?: number;
    'band22.sidechainQ'?: number;
    'band22.slopeDbOct'?: number;
    'band22.soloed'?: boolean;
    'band22.thresholdDb'?: number;
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band23.attackMs'?: number;
    'band23.autoThreshold'?: boolean;
    'band23.bypassed'?: boolean;
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    'band23.detectorDelayMs'?: number;
    'band23.dynamic'?: boolean;
    'band23.enabled'?: boolean;
    'band23.lookaheadMs'?: number;
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band23.proportionalQ'?: boolean;
    'band23.proportionalQStrength'?: number;
    'band23.rangeDb'?: number;
    'band23.ratio'?: number;
    'band23.releaseMs'?: number;
    'band23.sidechainFreqHz'?: number;
    'band23.sidechainQ'?: number;
    'band23.slopeDbOct'?: number;
    'band23.soloed'?: boolean;
    'band23.thresholdDb'?: number;
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band3.attackMs'?: number;
    'band3.autoThreshold'?: boolean;
    'band3.bypassed'?: boolean;
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    'band3.detectorDelayMs'?: number;
    'band3.dynamic'?: boolean;
    'band3.enabled'?: boolean;
    'band3.lookaheadMs'?: number;
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band3.proportionalQ'?: boolean;
    'band3.proportionalQStrength'?: number;
    'band3.rangeDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.sidechainFreqHz'?: number;
    'band3.sidechainQ'?: number;
    'band3.slopeDbOct'?: number;
    'band3.soloed'?: boolean;
    'band3.thresholdDb'?: number;
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band4.attackMs'?: number;
    'band4.autoThreshold'?: boolean;
    'band4.bypassed'?: boolean;
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    'band4.detectorDelayMs'?: number;
    'band4.dynamic'?: boolean;
    'band4.enabled'?: boolean;
    'band4.lookaheadMs'?: number;
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band4.proportionalQ'?: boolean;
    'band4.proportionalQStrength'?: number;
    'band4.rangeDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.sidechainFreqHz'?: number;
    'band4.sidechainQ'?: number;
    'band4.slopeDbOct'?: number;
    'band4.soloed'?: boolean;
    'band4.thresholdDb'?: number;
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band5.attackMs'?: number;
    'band5.autoThreshold'?: boolean;
    'band5.bypassed'?: boolean;
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    'band5.detectorDelayMs'?: number;
    'band5.dynamic'?: boolean;
    'band5.enabled'?: boolean;
    'band5.lookaheadMs'?: number;
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band5.proportionalQ'?: boolean;
    'band5.proportionalQStrength'?: number;
    'band5.rangeDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.sidechainFreqHz'?: number;
    'band5.sidechainQ'?: number;
    'band5.slopeDbOct'?: number;
    'band5.soloed'?: boolean;
    'band5.thresholdDb'?: number;
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band6.attackMs'?: number;
    'band6.autoThreshold'?: boolean;
    'band6.bypassed'?: boolean;
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    'band6.detectorDelayMs'?: number;
    'band6.dynamic'?: boolean;
    'band6.enabled'?: boolean;
    'band6.lookaheadMs'?: number;
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band6.proportionalQ'?: boolean;
    'band6.proportionalQStrength'?: number;
    'band6.rangeDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.sidechainFreqHz'?: number;
    'band6.sidechainQ'?: number;
    'band6.slopeDbOct'?: number;
    'band6.soloed'?: boolean;
    'band6.thresholdDb'?: number;
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band7.attackMs'?: number;
    'band7.autoThreshold'?: boolean;
    'band7.bypassed'?: boolean;
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    'band7.detectorDelayMs'?: number;
    'band7.dynamic'?: boolean;
    'band7.enabled'?: boolean;
    'band7.lookaheadMs'?: number;
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band7.proportionalQ'?: boolean;
    'band7.proportionalQStrength'?: number;
    'band7.rangeDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.sidechainFreqHz'?: number;
    'band7.sidechainQ'?: number;
    'band7.slopeDbOct'?: number;
    'band7.soloed'?: boolean;
    'band7.thresholdDb'?: number;
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band8.attackMs'?: number;
    'band8.autoThreshold'?: boolean;
    'band8.bypassed'?: boolean;
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    'band8.detectorDelayMs'?: number;
    'band8.dynamic'?: boolean;
    'band8.enabled'?: boolean;
    'band8.lookaheadMs'?: number;
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band8.proportionalQ'?: boolean;
    'band8.proportionalQStrength'?: number;
    'band8.rangeDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.sidechainFreqHz'?: number;
    'band8.sidechainQ'?: number;
    'band8.slopeDbOct'?: number;
    'band8.soloed'?: boolean;
    'band8.thresholdDb'?: number;
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    'band9.attackMs'?: number;
    'band9.autoThreshold'?: boolean;
    'band9.bypassed'?: boolean;
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    'band9.detectorDelayMs'?: number;
    'band9.dynamic'?: boolean;
    'band9.enabled'?: boolean;
    'band9.lookaheadMs'?: number;
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band9.proportionalQ'?: boolean;
    'band9.proportionalQStrength'?: number;
    'band9.rangeDb'?: number;
    'band9.ratio'?: number;
    'band9.releaseMs'?: number;
    'band9.sidechainFreqHz'?: number;
    'band9.sidechainQ'?: number;
    'band9.slopeDbOct'?: number;
    'band9.soloed'?: boolean;
    'band9.thresholdDb'?: number;
    'band9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch';
    fftSize?: number;
    kernelSize?: number;
    partitionSize?: number;
    resolution?: 'custom' | 'low' | 'medium' | 'high' | 'veryHigh' | 'maximum';
    usePartitionedConvolution?: boolean;
  };
  'eq.midSide': {
    'midBand0.frequencyHz'?: number;
    'midBand0.gainDb'?: number;
    'midBand0.q'?: number;
    'midBand1.frequencyHz'?: number;
    'midBand1.gainDb'?: number;
    'midBand1.q'?: number;
    'midBand2.frequencyHz'?: number;
    'midBand2.gainDb'?: number;
    'midBand2.q'?: number;
    'midBand3.frequencyHz'?: number;
    'midBand3.gainDb'?: number;
    'midBand3.q'?: number;
    'midBand4.frequencyHz'?: number;
    'midBand4.gainDb'?: number;
    'midBand4.q'?: number;
    'midBand5.frequencyHz'?: number;
    'midBand5.gainDb'?: number;
    'midBand5.q'?: number;
    'midBand6.frequencyHz'?: number;
    'midBand6.gainDb'?: number;
    'midBand6.q'?: number;
    'midBand7.frequencyHz'?: number;
    'midBand7.gainDb'?: number;
    'midBand7.q'?: number;
    'midBand8.frequencyHz'?: number;
    'midBand8.gainDb'?: number;
    'midBand8.q'?: number;
    'midBand9.frequencyHz'?: number;
    'midBand9.gainDb'?: number;
    'midBand9.q'?: number;
    'midBand10.frequencyHz'?: number;
    'midBand10.gainDb'?: number;
    'midBand10.q'?: number;
    'midBand11.frequencyHz'?: number;
    'midBand11.gainDb'?: number;
    'midBand11.q'?: number;
    'midBand12.frequencyHz'?: number;
    'midBand12.gainDb'?: number;
    'midBand12.q'?: number;
    'midBand13.frequencyHz'?: number;
    'midBand13.gainDb'?: number;
    'midBand13.q'?: number;
    'midBand14.frequencyHz'?: number;
    'midBand14.gainDb'?: number;
    'midBand14.q'?: number;
    'midBand15.frequencyHz'?: number;
    'midBand15.gainDb'?: number;
    'midBand15.q'?: number;
    'midBand16.frequencyHz'?: number;
    'midBand16.gainDb'?: number;
    'midBand16.q'?: number;
    'midBand17.frequencyHz'?: number;
    'midBand17.gainDb'?: number;
    'midBand17.q'?: number;
    'midBand18.frequencyHz'?: number;
    'midBand18.gainDb'?: number;
    'midBand18.q'?: number;
    'midBand19.frequencyHz'?: number;
    'midBand19.gainDb'?: number;
    'midBand19.q'?: number;
    'midBand20.frequencyHz'?: number;
    'midBand20.gainDb'?: number;
    'midBand20.q'?: number;
    'midBand21.frequencyHz'?: number;
    'midBand21.gainDb'?: number;
    'midBand21.q'?: number;
    'midBand22.frequencyHz'?: number;
    'midBand22.gainDb'?: number;
    'midBand22.q'?: number;
    'midBand23.frequencyHz'?: number;
    'midBand23.gainDb'?: number;
    'midBand23.q'?: number;
    'sideBand0.frequencyHz'?: number;
    'sideBand0.gainDb'?: number;
    'sideBand0.q'?: number;
    'sideBand1.frequencyHz'?: number;
    'sideBand1.gainDb'?: number;
    'sideBand1.q'?: number;
    'sideBand2.frequencyHz'?: number;
    'sideBand2.gainDb'?: number;
    'sideBand2.q'?: number;
    'sideBand3.frequencyHz'?: number;
    'sideBand3.gainDb'?: number;
    'sideBand3.q'?: number;
    'sideBand4.frequencyHz'?: number;
    'sideBand4.gainDb'?: number;
    'sideBand4.q'?: number;
    'sideBand5.frequencyHz'?: number;
    'sideBand5.gainDb'?: number;
    'sideBand5.q'?: number;
    'sideBand6.frequencyHz'?: number;
    'sideBand6.gainDb'?: number;
    'sideBand6.q'?: number;
    'sideBand7.frequencyHz'?: number;
    'sideBand7.gainDb'?: number;
    'sideBand7.q'?: number;
    'sideBand8.frequencyHz'?: number;
    'sideBand8.gainDb'?: number;
    'sideBand8.q'?: number;
    'sideBand9.frequencyHz'?: number;
    'sideBand9.gainDb'?: number;
    'sideBand9.q'?: number;
    'sideBand10.frequencyHz'?: number;
    'sideBand10.gainDb'?: number;
    'sideBand10.q'?: number;
    'sideBand11.frequencyHz'?: number;
    'sideBand11.gainDb'?: number;
    'sideBand11.q'?: number;
    'sideBand12.frequencyHz'?: number;
    'sideBand12.gainDb'?: number;
    'sideBand12.q'?: number;
    'sideBand13.frequencyHz'?: number;
    'sideBand13.gainDb'?: number;
    'sideBand13.q'?: number;
    'sideBand14.frequencyHz'?: number;
    'sideBand14.gainDb'?: number;
    'sideBand14.q'?: number;
    'sideBand15.frequencyHz'?: number;
    'sideBand15.gainDb'?: number;
    'sideBand15.q'?: number;
    'sideBand16.frequencyHz'?: number;
    'sideBand16.gainDb'?: number;
    'sideBand16.q'?: number;
    'sideBand17.frequencyHz'?: number;
    'sideBand17.gainDb'?: number;
    'sideBand17.q'?: number;
    'sideBand18.frequencyHz'?: number;
    'sideBand18.gainDb'?: number;
    'sideBand18.q'?: number;
    'sideBand19.frequencyHz'?: number;
    'sideBand19.gainDb'?: number;
    'sideBand19.q'?: number;
    'sideBand20.frequencyHz'?: number;
    'sideBand20.gainDb'?: number;
    'sideBand20.q'?: number;
    'sideBand21.frequencyHz'?: number;
    'sideBand21.gainDb'?: number;
    'sideBand21.q'?: number;
    'sideBand22.frequencyHz'?: number;
    'sideBand22.gainDb'?: number;
    'sideBand22.q'?: number;
    'sideBand23.frequencyHz'?: number;
    'sideBand23.gainDb'?: number;
    'sideBand23.q'?: number;
    'midBand0.attackMs'?: number;
    'midBand0.autoThreshold'?: boolean;
    'midBand0.bypassed'?: boolean;
    'midBand0.coeffMode'?: 'rbj' | 'vicanek';
    'midBand0.detectorDelayMs'?: number;
    'midBand0.dynamic'?: boolean;
    'midBand0.enabled'?: boolean;
    'midBand0.lookaheadMs'?: number;
    'midBand0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand0.proportionalQ'?: boolean;
    'midBand0.proportionalQStrength'?: number;
    'midBand0.rangeDb'?: number;
    'midBand0.ratio'?: number;
    'midBand0.releaseMs'?: number;
    'midBand0.sidechainFreqHz'?: number;
    'midBand0.sidechainQ'?: number;
    'midBand0.slopeDbOct'?: number;
    'midBand0.soloed'?: boolean;
    'midBand0.thresholdDb'?: number;
    'midBand0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand1.attackMs'?: number;
    'midBand1.autoThreshold'?: boolean;
    'midBand1.bypassed'?: boolean;
    'midBand1.coeffMode'?: 'rbj' | 'vicanek';
    'midBand1.detectorDelayMs'?: number;
    'midBand1.dynamic'?: boolean;
    'midBand1.enabled'?: boolean;
    'midBand1.lookaheadMs'?: number;
    'midBand1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand1.proportionalQ'?: boolean;
    'midBand1.proportionalQStrength'?: number;
    'midBand1.rangeDb'?: number;
    'midBand1.ratio'?: number;
    'midBand1.releaseMs'?: number;
    'midBand1.sidechainFreqHz'?: number;
    'midBand1.sidechainQ'?: number;
    'midBand1.slopeDbOct'?: number;
    'midBand1.soloed'?: boolean;
    'midBand1.thresholdDb'?: number;
    'midBand1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand10.attackMs'?: number;
    'midBand10.autoThreshold'?: boolean;
    'midBand10.bypassed'?: boolean;
    'midBand10.coeffMode'?: 'rbj' | 'vicanek';
    'midBand10.detectorDelayMs'?: number;
    'midBand10.dynamic'?: boolean;
    'midBand10.enabled'?: boolean;
    'midBand10.lookaheadMs'?: number;
    'midBand10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand10.proportionalQ'?: boolean;
    'midBand10.proportionalQStrength'?: number;
    'midBand10.rangeDb'?: number;
    'midBand10.ratio'?: number;
    'midBand10.releaseMs'?: number;
    'midBand10.sidechainFreqHz'?: number;
    'midBand10.sidechainQ'?: number;
    'midBand10.slopeDbOct'?: number;
    'midBand10.soloed'?: boolean;
    'midBand10.thresholdDb'?: number;
    'midBand10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand11.attackMs'?: number;
    'midBand11.autoThreshold'?: boolean;
    'midBand11.bypassed'?: boolean;
    'midBand11.coeffMode'?: 'rbj' | 'vicanek';
    'midBand11.detectorDelayMs'?: number;
    'midBand11.dynamic'?: boolean;
    'midBand11.enabled'?: boolean;
    'midBand11.lookaheadMs'?: number;
    'midBand11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand11.proportionalQ'?: boolean;
    'midBand11.proportionalQStrength'?: number;
    'midBand11.rangeDb'?: number;
    'midBand11.ratio'?: number;
    'midBand11.releaseMs'?: number;
    'midBand11.sidechainFreqHz'?: number;
    'midBand11.sidechainQ'?: number;
    'midBand11.slopeDbOct'?: number;
    'midBand11.soloed'?: boolean;
    'midBand11.thresholdDb'?: number;
    'midBand11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand12.attackMs'?: number;
    'midBand12.autoThreshold'?: boolean;
    'midBand12.bypassed'?: boolean;
    'midBand12.coeffMode'?: 'rbj' | 'vicanek';
    'midBand12.detectorDelayMs'?: number;
    'midBand12.dynamic'?: boolean;
    'midBand12.enabled'?: boolean;
    'midBand12.lookaheadMs'?: number;
    'midBand12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand12.proportionalQ'?: boolean;
    'midBand12.proportionalQStrength'?: number;
    'midBand12.rangeDb'?: number;
    'midBand12.ratio'?: number;
    'midBand12.releaseMs'?: number;
    'midBand12.sidechainFreqHz'?: number;
    'midBand12.sidechainQ'?: number;
    'midBand12.slopeDbOct'?: number;
    'midBand12.soloed'?: boolean;
    'midBand12.thresholdDb'?: number;
    'midBand12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand13.attackMs'?: number;
    'midBand13.autoThreshold'?: boolean;
    'midBand13.bypassed'?: boolean;
    'midBand13.coeffMode'?: 'rbj' | 'vicanek';
    'midBand13.detectorDelayMs'?: number;
    'midBand13.dynamic'?: boolean;
    'midBand13.enabled'?: boolean;
    'midBand13.lookaheadMs'?: number;
    'midBand13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand13.proportionalQ'?: boolean;
    'midBand13.proportionalQStrength'?: number;
    'midBand13.rangeDb'?: number;
    'midBand13.ratio'?: number;
    'midBand13.releaseMs'?: number;
    'midBand13.sidechainFreqHz'?: number;
    'midBand13.sidechainQ'?: number;
    'midBand13.slopeDbOct'?: number;
    'midBand13.soloed'?: boolean;
    'midBand13.thresholdDb'?: number;
    'midBand13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand14.attackMs'?: number;
    'midBand14.autoThreshold'?: boolean;
    'midBand14.bypassed'?: boolean;
    'midBand14.coeffMode'?: 'rbj' | 'vicanek';
    'midBand14.detectorDelayMs'?: number;
    'midBand14.dynamic'?: boolean;
    'midBand14.enabled'?: boolean;
    'midBand14.lookaheadMs'?: number;
    'midBand14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand14.proportionalQ'?: boolean;
    'midBand14.proportionalQStrength'?: number;
    'midBand14.rangeDb'?: number;
    'midBand14.ratio'?: number;
    'midBand14.releaseMs'?: number;
    'midBand14.sidechainFreqHz'?: number;
    'midBand14.sidechainQ'?: number;
    'midBand14.slopeDbOct'?: number;
    'midBand14.soloed'?: boolean;
    'midBand14.thresholdDb'?: number;
    'midBand14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand15.attackMs'?: number;
    'midBand15.autoThreshold'?: boolean;
    'midBand15.bypassed'?: boolean;
    'midBand15.coeffMode'?: 'rbj' | 'vicanek';
    'midBand15.detectorDelayMs'?: number;
    'midBand15.dynamic'?: boolean;
    'midBand15.enabled'?: boolean;
    'midBand15.lookaheadMs'?: number;
    'midBand15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand15.proportionalQ'?: boolean;
    'midBand15.proportionalQStrength'?: number;
    'midBand15.rangeDb'?: number;
    'midBand15.ratio'?: number;
    'midBand15.releaseMs'?: number;
    'midBand15.sidechainFreqHz'?: number;
    'midBand15.sidechainQ'?: number;
    'midBand15.slopeDbOct'?: number;
    'midBand15.soloed'?: boolean;
    'midBand15.thresholdDb'?: number;
    'midBand15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand16.attackMs'?: number;
    'midBand16.autoThreshold'?: boolean;
    'midBand16.bypassed'?: boolean;
    'midBand16.coeffMode'?: 'rbj' | 'vicanek';
    'midBand16.detectorDelayMs'?: number;
    'midBand16.dynamic'?: boolean;
    'midBand16.enabled'?: boolean;
    'midBand16.lookaheadMs'?: number;
    'midBand16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand16.proportionalQ'?: boolean;
    'midBand16.proportionalQStrength'?: number;
    'midBand16.rangeDb'?: number;
    'midBand16.ratio'?: number;
    'midBand16.releaseMs'?: number;
    'midBand16.sidechainFreqHz'?: number;
    'midBand16.sidechainQ'?: number;
    'midBand16.slopeDbOct'?: number;
    'midBand16.soloed'?: boolean;
    'midBand16.thresholdDb'?: number;
    'midBand16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand17.attackMs'?: number;
    'midBand17.autoThreshold'?: boolean;
    'midBand17.bypassed'?: boolean;
    'midBand17.coeffMode'?: 'rbj' | 'vicanek';
    'midBand17.detectorDelayMs'?: number;
    'midBand17.dynamic'?: boolean;
    'midBand17.enabled'?: boolean;
    'midBand17.lookaheadMs'?: number;
    'midBand17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand17.proportionalQ'?: boolean;
    'midBand17.proportionalQStrength'?: number;
    'midBand17.rangeDb'?: number;
    'midBand17.ratio'?: number;
    'midBand17.releaseMs'?: number;
    'midBand17.sidechainFreqHz'?: number;
    'midBand17.sidechainQ'?: number;
    'midBand17.slopeDbOct'?: number;
    'midBand17.soloed'?: boolean;
    'midBand17.thresholdDb'?: number;
    'midBand17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand18.attackMs'?: number;
    'midBand18.autoThreshold'?: boolean;
    'midBand18.bypassed'?: boolean;
    'midBand18.coeffMode'?: 'rbj' | 'vicanek';
    'midBand18.detectorDelayMs'?: number;
    'midBand18.dynamic'?: boolean;
    'midBand18.enabled'?: boolean;
    'midBand18.lookaheadMs'?: number;
    'midBand18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand18.proportionalQ'?: boolean;
    'midBand18.proportionalQStrength'?: number;
    'midBand18.rangeDb'?: number;
    'midBand18.ratio'?: number;
    'midBand18.releaseMs'?: number;
    'midBand18.sidechainFreqHz'?: number;
    'midBand18.sidechainQ'?: number;
    'midBand18.slopeDbOct'?: number;
    'midBand18.soloed'?: boolean;
    'midBand18.thresholdDb'?: number;
    'midBand18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand19.attackMs'?: number;
    'midBand19.autoThreshold'?: boolean;
    'midBand19.bypassed'?: boolean;
    'midBand19.coeffMode'?: 'rbj' | 'vicanek';
    'midBand19.detectorDelayMs'?: number;
    'midBand19.dynamic'?: boolean;
    'midBand19.enabled'?: boolean;
    'midBand19.lookaheadMs'?: number;
    'midBand19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand19.proportionalQ'?: boolean;
    'midBand19.proportionalQStrength'?: number;
    'midBand19.rangeDb'?: number;
    'midBand19.ratio'?: number;
    'midBand19.releaseMs'?: number;
    'midBand19.sidechainFreqHz'?: number;
    'midBand19.sidechainQ'?: number;
    'midBand19.slopeDbOct'?: number;
    'midBand19.soloed'?: boolean;
    'midBand19.thresholdDb'?: number;
    'midBand19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand2.attackMs'?: number;
    'midBand2.autoThreshold'?: boolean;
    'midBand2.bypassed'?: boolean;
    'midBand2.coeffMode'?: 'rbj' | 'vicanek';
    'midBand2.detectorDelayMs'?: number;
    'midBand2.dynamic'?: boolean;
    'midBand2.enabled'?: boolean;
    'midBand2.lookaheadMs'?: number;
    'midBand2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand2.proportionalQ'?: boolean;
    'midBand2.proportionalQStrength'?: number;
    'midBand2.rangeDb'?: number;
    'midBand2.ratio'?: number;
    'midBand2.releaseMs'?: number;
    'midBand2.sidechainFreqHz'?: number;
    'midBand2.sidechainQ'?: number;
    'midBand2.slopeDbOct'?: number;
    'midBand2.soloed'?: boolean;
    'midBand2.thresholdDb'?: number;
    'midBand2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand20.attackMs'?: number;
    'midBand20.autoThreshold'?: boolean;
    'midBand20.bypassed'?: boolean;
    'midBand20.coeffMode'?: 'rbj' | 'vicanek';
    'midBand20.detectorDelayMs'?: number;
    'midBand20.dynamic'?: boolean;
    'midBand20.enabled'?: boolean;
    'midBand20.lookaheadMs'?: number;
    'midBand20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand20.proportionalQ'?: boolean;
    'midBand20.proportionalQStrength'?: number;
    'midBand20.rangeDb'?: number;
    'midBand20.ratio'?: number;
    'midBand20.releaseMs'?: number;
    'midBand20.sidechainFreqHz'?: number;
    'midBand20.sidechainQ'?: number;
    'midBand20.slopeDbOct'?: number;
    'midBand20.soloed'?: boolean;
    'midBand20.thresholdDb'?: number;
    'midBand20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand21.attackMs'?: number;
    'midBand21.autoThreshold'?: boolean;
    'midBand21.bypassed'?: boolean;
    'midBand21.coeffMode'?: 'rbj' | 'vicanek';
    'midBand21.detectorDelayMs'?: number;
    'midBand21.dynamic'?: boolean;
    'midBand21.enabled'?: boolean;
    'midBand21.lookaheadMs'?: number;
    'midBand21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand21.proportionalQ'?: boolean;
    'midBand21.proportionalQStrength'?: number;
    'midBand21.rangeDb'?: number;
    'midBand21.ratio'?: number;
    'midBand21.releaseMs'?: number;
    'midBand21.sidechainFreqHz'?: number;
    'midBand21.sidechainQ'?: number;
    'midBand21.slopeDbOct'?: number;
    'midBand21.soloed'?: boolean;
    'midBand21.thresholdDb'?: number;
    'midBand21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand22.attackMs'?: number;
    'midBand22.autoThreshold'?: boolean;
    'midBand22.bypassed'?: boolean;
    'midBand22.coeffMode'?: 'rbj' | 'vicanek';
    'midBand22.detectorDelayMs'?: number;
    'midBand22.dynamic'?: boolean;
    'midBand22.enabled'?: boolean;
    'midBand22.lookaheadMs'?: number;
    'midBand22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand22.proportionalQ'?: boolean;
    'midBand22.proportionalQStrength'?: number;
    'midBand22.rangeDb'?: number;
    'midBand22.ratio'?: number;
    'midBand22.releaseMs'?: number;
    'midBand22.sidechainFreqHz'?: number;
    'midBand22.sidechainQ'?: number;
    'midBand22.slopeDbOct'?: number;
    'midBand22.soloed'?: boolean;
    'midBand22.thresholdDb'?: number;
    'midBand22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand23.attackMs'?: number;
    'midBand23.autoThreshold'?: boolean;
    'midBand23.bypassed'?: boolean;
    'midBand23.coeffMode'?: 'rbj' | 'vicanek';
    'midBand23.detectorDelayMs'?: number;
    'midBand23.dynamic'?: boolean;
    'midBand23.enabled'?: boolean;
    'midBand23.lookaheadMs'?: number;
    'midBand23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand23.proportionalQ'?: boolean;
    'midBand23.proportionalQStrength'?: number;
    'midBand23.rangeDb'?: number;
    'midBand23.ratio'?: number;
    'midBand23.releaseMs'?: number;
    'midBand23.sidechainFreqHz'?: number;
    'midBand23.sidechainQ'?: number;
    'midBand23.slopeDbOct'?: number;
    'midBand23.soloed'?: boolean;
    'midBand23.thresholdDb'?: number;
    'midBand23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand3.attackMs'?: number;
    'midBand3.autoThreshold'?: boolean;
    'midBand3.bypassed'?: boolean;
    'midBand3.coeffMode'?: 'rbj' | 'vicanek';
    'midBand3.detectorDelayMs'?: number;
    'midBand3.dynamic'?: boolean;
    'midBand3.enabled'?: boolean;
    'midBand3.lookaheadMs'?: number;
    'midBand3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand3.proportionalQ'?: boolean;
    'midBand3.proportionalQStrength'?: number;
    'midBand3.rangeDb'?: number;
    'midBand3.ratio'?: number;
    'midBand3.releaseMs'?: number;
    'midBand3.sidechainFreqHz'?: number;
    'midBand3.sidechainQ'?: number;
    'midBand3.slopeDbOct'?: number;
    'midBand3.soloed'?: boolean;
    'midBand3.thresholdDb'?: number;
    'midBand3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand4.attackMs'?: number;
    'midBand4.autoThreshold'?: boolean;
    'midBand4.bypassed'?: boolean;
    'midBand4.coeffMode'?: 'rbj' | 'vicanek';
    'midBand4.detectorDelayMs'?: number;
    'midBand4.dynamic'?: boolean;
    'midBand4.enabled'?: boolean;
    'midBand4.lookaheadMs'?: number;
    'midBand4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand4.proportionalQ'?: boolean;
    'midBand4.proportionalQStrength'?: number;
    'midBand4.rangeDb'?: number;
    'midBand4.ratio'?: number;
    'midBand4.releaseMs'?: number;
    'midBand4.sidechainFreqHz'?: number;
    'midBand4.sidechainQ'?: number;
    'midBand4.slopeDbOct'?: number;
    'midBand4.soloed'?: boolean;
    'midBand4.thresholdDb'?: number;
    'midBand4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand5.attackMs'?: number;
    'midBand5.autoThreshold'?: boolean;
    'midBand5.bypassed'?: boolean;
    'midBand5.coeffMode'?: 'rbj' | 'vicanek';
    'midBand5.detectorDelayMs'?: number;
    'midBand5.dynamic'?: boolean;
    'midBand5.enabled'?: boolean;
    'midBand5.lookaheadMs'?: number;
    'midBand5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand5.proportionalQ'?: boolean;
    'midBand5.proportionalQStrength'?: number;
    'midBand5.rangeDb'?: number;
    'midBand5.ratio'?: number;
    'midBand5.releaseMs'?: number;
    'midBand5.sidechainFreqHz'?: number;
    'midBand5.sidechainQ'?: number;
    'midBand5.slopeDbOct'?: number;
    'midBand5.soloed'?: boolean;
    'midBand5.thresholdDb'?: number;
    'midBand5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand6.attackMs'?: number;
    'midBand6.autoThreshold'?: boolean;
    'midBand6.bypassed'?: boolean;
    'midBand6.coeffMode'?: 'rbj' | 'vicanek';
    'midBand6.detectorDelayMs'?: number;
    'midBand6.dynamic'?: boolean;
    'midBand6.enabled'?: boolean;
    'midBand6.lookaheadMs'?: number;
    'midBand6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand6.proportionalQ'?: boolean;
    'midBand6.proportionalQStrength'?: number;
    'midBand6.rangeDb'?: number;
    'midBand6.ratio'?: number;
    'midBand6.releaseMs'?: number;
    'midBand6.sidechainFreqHz'?: number;
    'midBand6.sidechainQ'?: number;
    'midBand6.slopeDbOct'?: number;
    'midBand6.soloed'?: boolean;
    'midBand6.thresholdDb'?: number;
    'midBand6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand7.attackMs'?: number;
    'midBand7.autoThreshold'?: boolean;
    'midBand7.bypassed'?: boolean;
    'midBand7.coeffMode'?: 'rbj' | 'vicanek';
    'midBand7.detectorDelayMs'?: number;
    'midBand7.dynamic'?: boolean;
    'midBand7.enabled'?: boolean;
    'midBand7.lookaheadMs'?: number;
    'midBand7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand7.proportionalQ'?: boolean;
    'midBand7.proportionalQStrength'?: number;
    'midBand7.rangeDb'?: number;
    'midBand7.ratio'?: number;
    'midBand7.releaseMs'?: number;
    'midBand7.sidechainFreqHz'?: number;
    'midBand7.sidechainQ'?: number;
    'midBand7.slopeDbOct'?: number;
    'midBand7.soloed'?: boolean;
    'midBand7.thresholdDb'?: number;
    'midBand7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand8.attackMs'?: number;
    'midBand8.autoThreshold'?: boolean;
    'midBand8.bypassed'?: boolean;
    'midBand8.coeffMode'?: 'rbj' | 'vicanek';
    'midBand8.detectorDelayMs'?: number;
    'midBand8.dynamic'?: boolean;
    'midBand8.enabled'?: boolean;
    'midBand8.lookaheadMs'?: number;
    'midBand8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand8.proportionalQ'?: boolean;
    'midBand8.proportionalQStrength'?: number;
    'midBand8.rangeDb'?: number;
    'midBand8.ratio'?: number;
    'midBand8.releaseMs'?: number;
    'midBand8.sidechainFreqHz'?: number;
    'midBand8.sidechainQ'?: number;
    'midBand8.slopeDbOct'?: number;
    'midBand8.soloed'?: boolean;
    'midBand8.thresholdDb'?: number;
    'midBand8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'midBand9.attackMs'?: number;
    'midBand9.autoThreshold'?: boolean;
    'midBand9.bypassed'?: boolean;
    'midBand9.coeffMode'?: 'rbj' | 'vicanek';
    'midBand9.detectorDelayMs'?: number;
    'midBand9.dynamic'?: boolean;
    'midBand9.enabled'?: boolean;
    'midBand9.lookaheadMs'?: number;
    'midBand9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'midBand9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'midBand9.proportionalQ'?: boolean;
    'midBand9.proportionalQStrength'?: number;
    'midBand9.rangeDb'?: number;
    'midBand9.ratio'?: number;
    'midBand9.releaseMs'?: number;
    'midBand9.sidechainFreqHz'?: number;
    'midBand9.sidechainQ'?: number;
    'midBand9.slopeDbOct'?: number;
    'midBand9.soloed'?: boolean;
    'midBand9.thresholdDb'?: number;
    'midBand9.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand0.attackMs'?: number;
    'sideBand0.autoThreshold'?: boolean;
    'sideBand0.bypassed'?: boolean;
    'sideBand0.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand0.detectorDelayMs'?: number;
    'sideBand0.dynamic'?: boolean;
    'sideBand0.enabled'?: boolean;
    'sideBand0.lookaheadMs'?: number;
    'sideBand0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand0.proportionalQ'?: boolean;
    'sideBand0.proportionalQStrength'?: number;
    'sideBand0.rangeDb'?: number;
    'sideBand0.ratio'?: number;
    'sideBand0.releaseMs'?: number;
    'sideBand0.sidechainFreqHz'?: number;
    'sideBand0.sidechainQ'?: number;
    'sideBand0.slopeDbOct'?: number;
    'sideBand0.soloed'?: boolean;
    'sideBand0.thresholdDb'?: number;
    'sideBand0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand1.attackMs'?: number;
    'sideBand1.autoThreshold'?: boolean;
    'sideBand1.bypassed'?: boolean;
    'sideBand1.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand1.detectorDelayMs'?: number;
    'sideBand1.dynamic'?: boolean;
    'sideBand1.enabled'?: boolean;
    'sideBand1.lookaheadMs'?: number;
    'sideBand1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand1.proportionalQ'?: boolean;
    'sideBand1.proportionalQStrength'?: number;
    'sideBand1.rangeDb'?: number;
    'sideBand1.ratio'?: number;
    'sideBand1.releaseMs'?: number;
    'sideBand1.sidechainFreqHz'?: number;
    'sideBand1.sidechainQ'?: number;
    'sideBand1.slopeDbOct'?: number;
    'sideBand1.soloed'?: boolean;
    'sideBand1.thresholdDb'?: number;
    'sideBand1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand10.attackMs'?: number;
    'sideBand10.autoThreshold'?: boolean;
    'sideBand10.bypassed'?: boolean;
    'sideBand10.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand10.detectorDelayMs'?: number;
    'sideBand10.dynamic'?: boolean;
    'sideBand10.enabled'?: boolean;
    'sideBand10.lookaheadMs'?: number;
    'sideBand10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand10.proportionalQ'?: boolean;
    'sideBand10.proportionalQStrength'?: number;
    'sideBand10.rangeDb'?: number;
    'sideBand10.ratio'?: number;
    'sideBand10.releaseMs'?: number;
    'sideBand10.sidechainFreqHz'?: number;
    'sideBand10.sidechainQ'?: number;
    'sideBand10.slopeDbOct'?: number;
    'sideBand10.soloed'?: boolean;
    'sideBand10.thresholdDb'?: number;
    'sideBand10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand11.attackMs'?: number;
    'sideBand11.autoThreshold'?: boolean;
    'sideBand11.bypassed'?: boolean;
    'sideBand11.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand11.detectorDelayMs'?: number;
    'sideBand11.dynamic'?: boolean;
    'sideBand11.enabled'?: boolean;
    'sideBand11.lookaheadMs'?: number;
    'sideBand11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand11.proportionalQ'?: boolean;
    'sideBand11.proportionalQStrength'?: number;
    'sideBand11.rangeDb'?: number;
    'sideBand11.ratio'?: number;
    'sideBand11.releaseMs'?: number;
    'sideBand11.sidechainFreqHz'?: number;
    'sideBand11.sidechainQ'?: number;
    'sideBand11.slopeDbOct'?: number;
    'sideBand11.soloed'?: boolean;
    'sideBand11.thresholdDb'?: number;
    'sideBand11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand12.attackMs'?: number;
    'sideBand12.autoThreshold'?: boolean;
    'sideBand12.bypassed'?: boolean;
    'sideBand12.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand12.detectorDelayMs'?: number;
    'sideBand12.dynamic'?: boolean;
    'sideBand12.enabled'?: boolean;
    'sideBand12.lookaheadMs'?: number;
    'sideBand12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand12.proportionalQ'?: boolean;
    'sideBand12.proportionalQStrength'?: number;
    'sideBand12.rangeDb'?: number;
    'sideBand12.ratio'?: number;
    'sideBand12.releaseMs'?: number;
    'sideBand12.sidechainFreqHz'?: number;
    'sideBand12.sidechainQ'?: number;
    'sideBand12.slopeDbOct'?: number;
    'sideBand12.soloed'?: boolean;
    'sideBand12.thresholdDb'?: number;
    'sideBand12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand13.attackMs'?: number;
    'sideBand13.autoThreshold'?: boolean;
    'sideBand13.bypassed'?: boolean;
    'sideBand13.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand13.detectorDelayMs'?: number;
    'sideBand13.dynamic'?: boolean;
    'sideBand13.enabled'?: boolean;
    'sideBand13.lookaheadMs'?: number;
    'sideBand13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand13.proportionalQ'?: boolean;
    'sideBand13.proportionalQStrength'?: number;
    'sideBand13.rangeDb'?: number;
    'sideBand13.ratio'?: number;
    'sideBand13.releaseMs'?: number;
    'sideBand13.sidechainFreqHz'?: number;
    'sideBand13.sidechainQ'?: number;
    'sideBand13.slopeDbOct'?: number;
    'sideBand13.soloed'?: boolean;
    'sideBand13.thresholdDb'?: number;
    'sideBand13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand14.attackMs'?: number;
    'sideBand14.autoThreshold'?: boolean;
    'sideBand14.bypassed'?: boolean;
    'sideBand14.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand14.detectorDelayMs'?: number;
    'sideBand14.dynamic'?: boolean;
    'sideBand14.enabled'?: boolean;
    'sideBand14.lookaheadMs'?: number;
    'sideBand14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand14.proportionalQ'?: boolean;
    'sideBand14.proportionalQStrength'?: number;
    'sideBand14.rangeDb'?: number;
    'sideBand14.ratio'?: number;
    'sideBand14.releaseMs'?: number;
    'sideBand14.sidechainFreqHz'?: number;
    'sideBand14.sidechainQ'?: number;
    'sideBand14.slopeDbOct'?: number;
    'sideBand14.soloed'?: boolean;
    'sideBand14.thresholdDb'?: number;
    'sideBand14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand15.attackMs'?: number;
    'sideBand15.autoThreshold'?: boolean;
    'sideBand15.bypassed'?: boolean;
    'sideBand15.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand15.detectorDelayMs'?: number;
    'sideBand15.dynamic'?: boolean;
    'sideBand15.enabled'?: boolean;
    'sideBand15.lookaheadMs'?: number;
    'sideBand15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand15.proportionalQ'?: boolean;
    'sideBand15.proportionalQStrength'?: number;
    'sideBand15.rangeDb'?: number;
    'sideBand15.ratio'?: number;
    'sideBand15.releaseMs'?: number;
    'sideBand15.sidechainFreqHz'?: number;
    'sideBand15.sidechainQ'?: number;
    'sideBand15.slopeDbOct'?: number;
    'sideBand15.soloed'?: boolean;
    'sideBand15.thresholdDb'?: number;
    'sideBand15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand16.attackMs'?: number;
    'sideBand16.autoThreshold'?: boolean;
    'sideBand16.bypassed'?: boolean;
    'sideBand16.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand16.detectorDelayMs'?: number;
    'sideBand16.dynamic'?: boolean;
    'sideBand16.enabled'?: boolean;
    'sideBand16.lookaheadMs'?: number;
    'sideBand16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand16.proportionalQ'?: boolean;
    'sideBand16.proportionalQStrength'?: number;
    'sideBand16.rangeDb'?: number;
    'sideBand16.ratio'?: number;
    'sideBand16.releaseMs'?: number;
    'sideBand16.sidechainFreqHz'?: number;
    'sideBand16.sidechainQ'?: number;
    'sideBand16.slopeDbOct'?: number;
    'sideBand16.soloed'?: boolean;
    'sideBand16.thresholdDb'?: number;
    'sideBand16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand17.attackMs'?: number;
    'sideBand17.autoThreshold'?: boolean;
    'sideBand17.bypassed'?: boolean;
    'sideBand17.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand17.detectorDelayMs'?: number;
    'sideBand17.dynamic'?: boolean;
    'sideBand17.enabled'?: boolean;
    'sideBand17.lookaheadMs'?: number;
    'sideBand17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand17.proportionalQ'?: boolean;
    'sideBand17.proportionalQStrength'?: number;
    'sideBand17.rangeDb'?: number;
    'sideBand17.ratio'?: number;
    'sideBand17.releaseMs'?: number;
    'sideBand17.sidechainFreqHz'?: number;
    'sideBand17.sidechainQ'?: number;
    'sideBand17.slopeDbOct'?: number;
    'sideBand17.soloed'?: boolean;
    'sideBand17.thresholdDb'?: number;
    'sideBand17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand18.attackMs'?: number;
    'sideBand18.autoThreshold'?: boolean;
    'sideBand18.bypassed'?: boolean;
    'sideBand18.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand18.detectorDelayMs'?: number;
    'sideBand18.dynamic'?: boolean;
    'sideBand18.enabled'?: boolean;
    'sideBand18.lookaheadMs'?: number;
    'sideBand18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand18.proportionalQ'?: boolean;
    'sideBand18.proportionalQStrength'?: number;
    'sideBand18.rangeDb'?: number;
    'sideBand18.ratio'?: number;
    'sideBand18.releaseMs'?: number;
    'sideBand18.sidechainFreqHz'?: number;
    'sideBand18.sidechainQ'?: number;
    'sideBand18.slopeDbOct'?: number;
    'sideBand18.soloed'?: boolean;
    'sideBand18.thresholdDb'?: number;
    'sideBand18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand19.attackMs'?: number;
    'sideBand19.autoThreshold'?: boolean;
    'sideBand19.bypassed'?: boolean;
    'sideBand19.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand19.detectorDelayMs'?: number;
    'sideBand19.dynamic'?: boolean;
    'sideBand19.enabled'?: boolean;
    'sideBand19.lookaheadMs'?: number;
    'sideBand19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand19.proportionalQ'?: boolean;
    'sideBand19.proportionalQStrength'?: number;
    'sideBand19.rangeDb'?: number;
    'sideBand19.ratio'?: number;
    'sideBand19.releaseMs'?: number;
    'sideBand19.sidechainFreqHz'?: number;
    'sideBand19.sidechainQ'?: number;
    'sideBand19.slopeDbOct'?: number;
    'sideBand19.soloed'?: boolean;
    'sideBand19.thresholdDb'?: number;
    'sideBand19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand2.attackMs'?: number;
    'sideBand2.autoThreshold'?: boolean;
    'sideBand2.bypassed'?: boolean;
    'sideBand2.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand2.detectorDelayMs'?: number;
    'sideBand2.dynamic'?: boolean;
    'sideBand2.enabled'?: boolean;
    'sideBand2.lookaheadMs'?: number;
    'sideBand2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand2.proportionalQ'?: boolean;
    'sideBand2.proportionalQStrength'?: number;
    'sideBand2.rangeDb'?: number;
    'sideBand2.ratio'?: number;
    'sideBand2.releaseMs'?: number;
    'sideBand2.sidechainFreqHz'?: number;
    'sideBand2.sidechainQ'?: number;
    'sideBand2.slopeDbOct'?: number;
    'sideBand2.soloed'?: boolean;
    'sideBand2.thresholdDb'?: number;
    'sideBand2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand20.attackMs'?: number;
    'sideBand20.autoThreshold'?: boolean;
    'sideBand20.bypassed'?: boolean;
    'sideBand20.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand20.detectorDelayMs'?: number;
    'sideBand20.dynamic'?: boolean;
    'sideBand20.enabled'?: boolean;
    'sideBand20.lookaheadMs'?: number;
    'sideBand20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand20.proportionalQ'?: boolean;
    'sideBand20.proportionalQStrength'?: number;
    'sideBand20.rangeDb'?: number;
    'sideBand20.ratio'?: number;
    'sideBand20.releaseMs'?: number;
    'sideBand20.sidechainFreqHz'?: number;
    'sideBand20.sidechainQ'?: number;
    'sideBand20.slopeDbOct'?: number;
    'sideBand20.soloed'?: boolean;
    'sideBand20.thresholdDb'?: number;
    'sideBand20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand21.attackMs'?: number;
    'sideBand21.autoThreshold'?: boolean;
    'sideBand21.bypassed'?: boolean;
    'sideBand21.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand21.detectorDelayMs'?: number;
    'sideBand21.dynamic'?: boolean;
    'sideBand21.enabled'?: boolean;
    'sideBand21.lookaheadMs'?: number;
    'sideBand21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand21.proportionalQ'?: boolean;
    'sideBand21.proportionalQStrength'?: number;
    'sideBand21.rangeDb'?: number;
    'sideBand21.ratio'?: number;
    'sideBand21.releaseMs'?: number;
    'sideBand21.sidechainFreqHz'?: number;
    'sideBand21.sidechainQ'?: number;
    'sideBand21.slopeDbOct'?: number;
    'sideBand21.soloed'?: boolean;
    'sideBand21.thresholdDb'?: number;
    'sideBand21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand22.attackMs'?: number;
    'sideBand22.autoThreshold'?: boolean;
    'sideBand22.bypassed'?: boolean;
    'sideBand22.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand22.detectorDelayMs'?: number;
    'sideBand22.dynamic'?: boolean;
    'sideBand22.enabled'?: boolean;
    'sideBand22.lookaheadMs'?: number;
    'sideBand22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand22.proportionalQ'?: boolean;
    'sideBand22.proportionalQStrength'?: number;
    'sideBand22.rangeDb'?: number;
    'sideBand22.ratio'?: number;
    'sideBand22.releaseMs'?: number;
    'sideBand22.sidechainFreqHz'?: number;
    'sideBand22.sidechainQ'?: number;
    'sideBand22.slopeDbOct'?: number;
    'sideBand22.soloed'?: boolean;
    'sideBand22.thresholdDb'?: number;
    'sideBand22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand23.attackMs'?: number;
    'sideBand23.autoThreshold'?: boolean;
    'sideBand23.bypassed'?: boolean;
    'sideBand23.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand23.detectorDelayMs'?: number;
    'sideBand23.dynamic'?: boolean;
    'sideBand23.enabled'?: boolean;
    'sideBand23.lookaheadMs'?: number;
    'sideBand23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand23.proportionalQ'?: boolean;
    'sideBand23.proportionalQStrength'?: number;
    'sideBand23.rangeDb'?: number;
    'sideBand23.ratio'?: number;
    'sideBand23.releaseMs'?: number;
    'sideBand23.sidechainFreqHz'?: number;
    'sideBand23.sidechainQ'?: number;
    'sideBand23.slopeDbOct'?: number;
    'sideBand23.soloed'?: boolean;
    'sideBand23.thresholdDb'?: number;
    'sideBand23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand3.attackMs'?: number;
    'sideBand3.autoThreshold'?: boolean;
    'sideBand3.bypassed'?: boolean;
    'sideBand3.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand3.detectorDelayMs'?: number;
    'sideBand3.dynamic'?: boolean;
    'sideBand3.enabled'?: boolean;
    'sideBand3.lookaheadMs'?: number;
    'sideBand3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand3.proportionalQ'?: boolean;
    'sideBand3.proportionalQStrength'?: number;
    'sideBand3.rangeDb'?: number;
    'sideBand3.ratio'?: number;
    'sideBand3.releaseMs'?: number;
    'sideBand3.sidechainFreqHz'?: number;
    'sideBand3.sidechainQ'?: number;
    'sideBand3.slopeDbOct'?: number;
    'sideBand3.soloed'?: boolean;
    'sideBand3.thresholdDb'?: number;
    'sideBand3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand4.attackMs'?: number;
    'sideBand4.autoThreshold'?: boolean;
    'sideBand4.bypassed'?: boolean;
    'sideBand4.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand4.detectorDelayMs'?: number;
    'sideBand4.dynamic'?: boolean;
    'sideBand4.enabled'?: boolean;
    'sideBand4.lookaheadMs'?: number;
    'sideBand4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand4.proportionalQ'?: boolean;
    'sideBand4.proportionalQStrength'?: number;
    'sideBand4.rangeDb'?: number;
    'sideBand4.ratio'?: number;
    'sideBand4.releaseMs'?: number;
    'sideBand4.sidechainFreqHz'?: number;
    'sideBand4.sidechainQ'?: number;
    'sideBand4.slopeDbOct'?: number;
    'sideBand4.soloed'?: boolean;
    'sideBand4.thresholdDb'?: number;
    'sideBand4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand5.attackMs'?: number;
    'sideBand5.autoThreshold'?: boolean;
    'sideBand5.bypassed'?: boolean;
    'sideBand5.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand5.detectorDelayMs'?: number;
    'sideBand5.dynamic'?: boolean;
    'sideBand5.enabled'?: boolean;
    'sideBand5.lookaheadMs'?: number;
    'sideBand5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand5.proportionalQ'?: boolean;
    'sideBand5.proportionalQStrength'?: number;
    'sideBand5.rangeDb'?: number;
    'sideBand5.ratio'?: number;
    'sideBand5.releaseMs'?: number;
    'sideBand5.sidechainFreqHz'?: number;
    'sideBand5.sidechainQ'?: number;
    'sideBand5.slopeDbOct'?: number;
    'sideBand5.soloed'?: boolean;
    'sideBand5.thresholdDb'?: number;
    'sideBand5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand6.attackMs'?: number;
    'sideBand6.autoThreshold'?: boolean;
    'sideBand6.bypassed'?: boolean;
    'sideBand6.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand6.detectorDelayMs'?: number;
    'sideBand6.dynamic'?: boolean;
    'sideBand6.enabled'?: boolean;
    'sideBand6.lookaheadMs'?: number;
    'sideBand6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand6.proportionalQ'?: boolean;
    'sideBand6.proportionalQStrength'?: number;
    'sideBand6.rangeDb'?: number;
    'sideBand6.ratio'?: number;
    'sideBand6.releaseMs'?: number;
    'sideBand6.sidechainFreqHz'?: number;
    'sideBand6.sidechainQ'?: number;
    'sideBand6.slopeDbOct'?: number;
    'sideBand6.soloed'?: boolean;
    'sideBand6.thresholdDb'?: number;
    'sideBand6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand7.attackMs'?: number;
    'sideBand7.autoThreshold'?: boolean;
    'sideBand7.bypassed'?: boolean;
    'sideBand7.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand7.detectorDelayMs'?: number;
    'sideBand7.dynamic'?: boolean;
    'sideBand7.enabled'?: boolean;
    'sideBand7.lookaheadMs'?: number;
    'sideBand7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand7.proportionalQ'?: boolean;
    'sideBand7.proportionalQStrength'?: number;
    'sideBand7.rangeDb'?: number;
    'sideBand7.ratio'?: number;
    'sideBand7.releaseMs'?: number;
    'sideBand7.sidechainFreqHz'?: number;
    'sideBand7.sidechainQ'?: number;
    'sideBand7.slopeDbOct'?: number;
    'sideBand7.soloed'?: boolean;
    'sideBand7.thresholdDb'?: number;
    'sideBand7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand8.attackMs'?: number;
    'sideBand8.autoThreshold'?: boolean;
    'sideBand8.bypassed'?: boolean;
    'sideBand8.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand8.detectorDelayMs'?: number;
    'sideBand8.dynamic'?: boolean;
    'sideBand8.enabled'?: boolean;
    'sideBand8.lookaheadMs'?: number;
    'sideBand8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand8.proportionalQ'?: boolean;
    'sideBand8.proportionalQStrength'?: number;
    'sideBand8.rangeDb'?: number;
    'sideBand8.ratio'?: number;
    'sideBand8.releaseMs'?: number;
    'sideBand8.sidechainFreqHz'?: number;
    'sideBand8.sidechainQ'?: number;
    'sideBand8.slopeDbOct'?: number;
    'sideBand8.soloed'?: boolean;
    'sideBand8.thresholdDb'?: number;
    'sideBand8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'sideBand9.attackMs'?: number;
    'sideBand9.autoThreshold'?: boolean;
    'sideBand9.bypassed'?: boolean;
    'sideBand9.coeffMode'?: 'rbj' | 'vicanek';
    'sideBand9.detectorDelayMs'?: number;
    'sideBand9.dynamic'?: boolean;
    'sideBand9.enabled'?: boolean;
    'sideBand9.lookaheadMs'?: number;
    'sideBand9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'sideBand9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'sideBand9.proportionalQ'?: boolean;
    'sideBand9.proportionalQStrength'?: number;
    'sideBand9.rangeDb'?: number;
    'sideBand9.ratio'?: number;
    'sideBand9.releaseMs'?: number;
    'sideBand9.sidechainFreqHz'?: number;
    'sideBand9.sidechainQ'?: number;
    'sideBand9.slopeDbOct'?: number;
    'sideBand9.soloed'?: boolean;
    'sideBand9.thresholdDb'?: number;
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
  'eq.minimumPhase': {
    'band0.frequencyHz'?: number;
    'band0.gainDb'?: number;
    'band0.q'?: number;
    'band1.frequencyHz'?: number;
    'band1.gainDb'?: number;
    'band1.q'?: number;
    'band2.frequencyHz'?: number;
    'band2.gainDb'?: number;
    'band2.q'?: number;
    'band3.frequencyHz'?: number;
    'band3.gainDb'?: number;
    'band3.q'?: number;
    'band4.frequencyHz'?: number;
    'band4.gainDb'?: number;
    'band4.q'?: number;
    'band5.frequencyHz'?: number;
    'band5.gainDb'?: number;
    'band5.q'?: number;
    'band6.frequencyHz'?: number;
    'band6.gainDb'?: number;
    'band6.q'?: number;
    'band7.frequencyHz'?: number;
    'band7.gainDb'?: number;
    'band7.q'?: number;
    'band8.frequencyHz'?: number;
    'band8.gainDb'?: number;
    'band8.q'?: number;
    'band9.frequencyHz'?: number;
    'band9.gainDb'?: number;
    'band9.q'?: number;
    'band10.frequencyHz'?: number;
    'band10.gainDb'?: number;
    'band10.q'?: number;
    'band11.frequencyHz'?: number;
    'band11.gainDb'?: number;
    'band11.q'?: number;
    'band12.frequencyHz'?: number;
    'band12.gainDb'?: number;
    'band12.q'?: number;
    'band13.frequencyHz'?: number;
    'band13.gainDb'?: number;
    'band13.q'?: number;
    'band14.frequencyHz'?: number;
    'band14.gainDb'?: number;
    'band14.q'?: number;
    'band15.frequencyHz'?: number;
    'band15.gainDb'?: number;
    'band15.q'?: number;
    'band16.frequencyHz'?: number;
    'band16.gainDb'?: number;
    'band16.q'?: number;
    'band17.frequencyHz'?: number;
    'band17.gainDb'?: number;
    'band17.q'?: number;
    'band18.frequencyHz'?: number;
    'band18.gainDb'?: number;
    'band18.q'?: number;
    'band19.frequencyHz'?: number;
    'band19.gainDb'?: number;
    'band19.q'?: number;
    'band20.frequencyHz'?: number;
    'band20.gainDb'?: number;
    'band20.q'?: number;
    'band21.frequencyHz'?: number;
    'band21.gainDb'?: number;
    'band21.q'?: number;
    'band22.frequencyHz'?: number;
    'band22.gainDb'?: number;
    'band22.q'?: number;
    'band23.frequencyHz'?: number;
    'band23.gainDb'?: number;
    'band23.q'?: number;
    'band0.attackMs'?: number;
    'band0.autoThreshold'?: boolean;
    'band0.bypassed'?: boolean;
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    'band0.detectorDelayMs'?: number;
    'band0.dynamic'?: boolean;
    'band0.enabled'?: boolean;
    'band0.lookaheadMs'?: number;
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band0.proportionalQ'?: boolean;
    'band0.proportionalQStrength'?: number;
    'band0.rangeDb'?: number;
    'band0.ratio'?: number;
    'band0.releaseMs'?: number;
    'band0.sidechainFreqHz'?: number;
    'band0.sidechainQ'?: number;
    'band0.slopeDbOct'?: number;
    'band0.soloed'?: boolean;
    'band0.thresholdDb'?: number;
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.attackMs'?: number;
    'band1.autoThreshold'?: boolean;
    'band1.bypassed'?: boolean;
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    'band1.detectorDelayMs'?: number;
    'band1.dynamic'?: boolean;
    'band1.enabled'?: boolean;
    'band1.lookaheadMs'?: number;
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band1.proportionalQ'?: boolean;
    'band1.proportionalQStrength'?: number;
    'band1.rangeDb'?: number;
    'band1.ratio'?: number;
    'band1.releaseMs'?: number;
    'band1.sidechainFreqHz'?: number;
    'band1.sidechainQ'?: number;
    'band1.slopeDbOct'?: number;
    'band1.soloed'?: boolean;
    'band1.thresholdDb'?: number;
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band10.attackMs'?: number;
    'band10.autoThreshold'?: boolean;
    'band10.bypassed'?: boolean;
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    'band10.detectorDelayMs'?: number;
    'band10.dynamic'?: boolean;
    'band10.enabled'?: boolean;
    'band10.lookaheadMs'?: number;
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band10.proportionalQ'?: boolean;
    'band10.proportionalQStrength'?: number;
    'band10.rangeDb'?: number;
    'band10.ratio'?: number;
    'band10.releaseMs'?: number;
    'band10.sidechainFreqHz'?: number;
    'band10.sidechainQ'?: number;
    'band10.slopeDbOct'?: number;
    'band10.soloed'?: boolean;
    'band10.thresholdDb'?: number;
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band11.attackMs'?: number;
    'band11.autoThreshold'?: boolean;
    'band11.bypassed'?: boolean;
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    'band11.detectorDelayMs'?: number;
    'band11.dynamic'?: boolean;
    'band11.enabled'?: boolean;
    'band11.lookaheadMs'?: number;
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band11.proportionalQ'?: boolean;
    'band11.proportionalQStrength'?: number;
    'band11.rangeDb'?: number;
    'band11.ratio'?: number;
    'band11.releaseMs'?: number;
    'band11.sidechainFreqHz'?: number;
    'band11.sidechainQ'?: number;
    'band11.slopeDbOct'?: number;
    'band11.soloed'?: boolean;
    'band11.thresholdDb'?: number;
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band12.attackMs'?: number;
    'band12.autoThreshold'?: boolean;
    'band12.bypassed'?: boolean;
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    'band12.detectorDelayMs'?: number;
    'band12.dynamic'?: boolean;
    'band12.enabled'?: boolean;
    'band12.lookaheadMs'?: number;
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band12.proportionalQ'?: boolean;
    'band12.proportionalQStrength'?: number;
    'band12.rangeDb'?: number;
    'band12.ratio'?: number;
    'band12.releaseMs'?: number;
    'band12.sidechainFreqHz'?: number;
    'band12.sidechainQ'?: number;
    'band12.slopeDbOct'?: number;
    'band12.soloed'?: boolean;
    'band12.thresholdDb'?: number;
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band13.attackMs'?: number;
    'band13.autoThreshold'?: boolean;
    'band13.bypassed'?: boolean;
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    'band13.detectorDelayMs'?: number;
    'band13.dynamic'?: boolean;
    'band13.enabled'?: boolean;
    'band13.lookaheadMs'?: number;
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band13.proportionalQ'?: boolean;
    'band13.proportionalQStrength'?: number;
    'band13.rangeDb'?: number;
    'band13.ratio'?: number;
    'band13.releaseMs'?: number;
    'band13.sidechainFreqHz'?: number;
    'band13.sidechainQ'?: number;
    'band13.slopeDbOct'?: number;
    'band13.soloed'?: boolean;
    'band13.thresholdDb'?: number;
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band14.attackMs'?: number;
    'band14.autoThreshold'?: boolean;
    'band14.bypassed'?: boolean;
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    'band14.detectorDelayMs'?: number;
    'band14.dynamic'?: boolean;
    'band14.enabled'?: boolean;
    'band14.lookaheadMs'?: number;
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band14.proportionalQ'?: boolean;
    'band14.proportionalQStrength'?: number;
    'band14.rangeDb'?: number;
    'band14.ratio'?: number;
    'band14.releaseMs'?: number;
    'band14.sidechainFreqHz'?: number;
    'band14.sidechainQ'?: number;
    'band14.slopeDbOct'?: number;
    'band14.soloed'?: boolean;
    'band14.thresholdDb'?: number;
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band15.attackMs'?: number;
    'band15.autoThreshold'?: boolean;
    'band15.bypassed'?: boolean;
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    'band15.detectorDelayMs'?: number;
    'band15.dynamic'?: boolean;
    'band15.enabled'?: boolean;
    'band15.lookaheadMs'?: number;
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band15.proportionalQ'?: boolean;
    'band15.proportionalQStrength'?: number;
    'band15.rangeDb'?: number;
    'band15.ratio'?: number;
    'band15.releaseMs'?: number;
    'band15.sidechainFreqHz'?: number;
    'band15.sidechainQ'?: number;
    'band15.slopeDbOct'?: number;
    'band15.soloed'?: boolean;
    'band15.thresholdDb'?: number;
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band16.attackMs'?: number;
    'band16.autoThreshold'?: boolean;
    'band16.bypassed'?: boolean;
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    'band16.detectorDelayMs'?: number;
    'band16.dynamic'?: boolean;
    'band16.enabled'?: boolean;
    'band16.lookaheadMs'?: number;
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band16.proportionalQ'?: boolean;
    'band16.proportionalQStrength'?: number;
    'band16.rangeDb'?: number;
    'band16.ratio'?: number;
    'band16.releaseMs'?: number;
    'band16.sidechainFreqHz'?: number;
    'band16.sidechainQ'?: number;
    'band16.slopeDbOct'?: number;
    'band16.soloed'?: boolean;
    'band16.thresholdDb'?: number;
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band17.attackMs'?: number;
    'band17.autoThreshold'?: boolean;
    'band17.bypassed'?: boolean;
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    'band17.detectorDelayMs'?: number;
    'band17.dynamic'?: boolean;
    'band17.enabled'?: boolean;
    'band17.lookaheadMs'?: number;
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band17.proportionalQ'?: boolean;
    'band17.proportionalQStrength'?: number;
    'band17.rangeDb'?: number;
    'band17.ratio'?: number;
    'band17.releaseMs'?: number;
    'band17.sidechainFreqHz'?: number;
    'band17.sidechainQ'?: number;
    'band17.slopeDbOct'?: number;
    'band17.soloed'?: boolean;
    'band17.thresholdDb'?: number;
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band18.attackMs'?: number;
    'band18.autoThreshold'?: boolean;
    'band18.bypassed'?: boolean;
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    'band18.detectorDelayMs'?: number;
    'band18.dynamic'?: boolean;
    'band18.enabled'?: boolean;
    'band18.lookaheadMs'?: number;
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band18.proportionalQ'?: boolean;
    'band18.proportionalQStrength'?: number;
    'band18.rangeDb'?: number;
    'band18.ratio'?: number;
    'band18.releaseMs'?: number;
    'band18.sidechainFreqHz'?: number;
    'band18.sidechainQ'?: number;
    'band18.slopeDbOct'?: number;
    'band18.soloed'?: boolean;
    'band18.thresholdDb'?: number;
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band19.attackMs'?: number;
    'band19.autoThreshold'?: boolean;
    'band19.bypassed'?: boolean;
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    'band19.detectorDelayMs'?: number;
    'band19.dynamic'?: boolean;
    'band19.enabled'?: boolean;
    'band19.lookaheadMs'?: number;
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band19.proportionalQ'?: boolean;
    'band19.proportionalQStrength'?: number;
    'band19.rangeDb'?: number;
    'band19.ratio'?: number;
    'band19.releaseMs'?: number;
    'band19.sidechainFreqHz'?: number;
    'band19.sidechainQ'?: number;
    'band19.slopeDbOct'?: number;
    'band19.soloed'?: boolean;
    'band19.thresholdDb'?: number;
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.attackMs'?: number;
    'band2.autoThreshold'?: boolean;
    'band2.bypassed'?: boolean;
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    'band2.detectorDelayMs'?: number;
    'band2.dynamic'?: boolean;
    'band2.enabled'?: boolean;
    'band2.lookaheadMs'?: number;
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band2.proportionalQ'?: boolean;
    'band2.proportionalQStrength'?: number;
    'band2.rangeDb'?: number;
    'band2.ratio'?: number;
    'band2.releaseMs'?: number;
    'band2.sidechainFreqHz'?: number;
    'band2.sidechainQ'?: number;
    'band2.slopeDbOct'?: number;
    'band2.soloed'?: boolean;
    'band2.thresholdDb'?: number;
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band20.attackMs'?: number;
    'band20.autoThreshold'?: boolean;
    'band20.bypassed'?: boolean;
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    'band20.detectorDelayMs'?: number;
    'band20.dynamic'?: boolean;
    'band20.enabled'?: boolean;
    'band20.lookaheadMs'?: number;
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band20.proportionalQ'?: boolean;
    'band20.proportionalQStrength'?: number;
    'band20.rangeDb'?: number;
    'band20.ratio'?: number;
    'band20.releaseMs'?: number;
    'band20.sidechainFreqHz'?: number;
    'band20.sidechainQ'?: number;
    'band20.slopeDbOct'?: number;
    'band20.soloed'?: boolean;
    'band20.thresholdDb'?: number;
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band21.attackMs'?: number;
    'band21.autoThreshold'?: boolean;
    'band21.bypassed'?: boolean;
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    'band21.detectorDelayMs'?: number;
    'band21.dynamic'?: boolean;
    'band21.enabled'?: boolean;
    'band21.lookaheadMs'?: number;
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band21.proportionalQ'?: boolean;
    'band21.proportionalQStrength'?: number;
    'band21.rangeDb'?: number;
    'band21.ratio'?: number;
    'band21.releaseMs'?: number;
    'band21.sidechainFreqHz'?: number;
    'band21.sidechainQ'?: number;
    'band21.slopeDbOct'?: number;
    'band21.soloed'?: boolean;
    'band21.thresholdDb'?: number;
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band22.attackMs'?: number;
    'band22.autoThreshold'?: boolean;
    'band22.bypassed'?: boolean;
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    'band22.detectorDelayMs'?: number;
    'band22.dynamic'?: boolean;
    'band22.enabled'?: boolean;
    'band22.lookaheadMs'?: number;
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band22.proportionalQ'?: boolean;
    'band22.proportionalQStrength'?: number;
    'band22.rangeDb'?: number;
    'band22.ratio'?: number;
    'band22.releaseMs'?: number;
    'band22.sidechainFreqHz'?: number;
    'band22.sidechainQ'?: number;
    'band22.slopeDbOct'?: number;
    'band22.soloed'?: boolean;
    'band22.thresholdDb'?: number;
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band23.attackMs'?: number;
    'band23.autoThreshold'?: boolean;
    'band23.bypassed'?: boolean;
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    'band23.detectorDelayMs'?: number;
    'band23.dynamic'?: boolean;
    'band23.enabled'?: boolean;
    'band23.lookaheadMs'?: number;
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band23.proportionalQ'?: boolean;
    'band23.proportionalQStrength'?: number;
    'band23.rangeDb'?: number;
    'band23.ratio'?: number;
    'band23.releaseMs'?: number;
    'band23.sidechainFreqHz'?: number;
    'band23.sidechainQ'?: number;
    'band23.slopeDbOct'?: number;
    'band23.soloed'?: boolean;
    'band23.thresholdDb'?: number;
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.attackMs'?: number;
    'band3.autoThreshold'?: boolean;
    'band3.bypassed'?: boolean;
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    'band3.detectorDelayMs'?: number;
    'band3.dynamic'?: boolean;
    'band3.enabled'?: boolean;
    'band3.lookaheadMs'?: number;
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band3.proportionalQ'?: boolean;
    'band3.proportionalQStrength'?: number;
    'band3.rangeDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.sidechainFreqHz'?: number;
    'band3.sidechainQ'?: number;
    'band3.slopeDbOct'?: number;
    'band3.soloed'?: boolean;
    'band3.thresholdDb'?: number;
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.attackMs'?: number;
    'band4.autoThreshold'?: boolean;
    'band4.bypassed'?: boolean;
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    'band4.detectorDelayMs'?: number;
    'band4.dynamic'?: boolean;
    'band4.enabled'?: boolean;
    'band4.lookaheadMs'?: number;
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band4.proportionalQ'?: boolean;
    'band4.proportionalQStrength'?: number;
    'band4.rangeDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.sidechainFreqHz'?: number;
    'band4.sidechainQ'?: number;
    'band4.slopeDbOct'?: number;
    'band4.soloed'?: boolean;
    'band4.thresholdDb'?: number;
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.attackMs'?: number;
    'band5.autoThreshold'?: boolean;
    'band5.bypassed'?: boolean;
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    'band5.detectorDelayMs'?: number;
    'band5.dynamic'?: boolean;
    'band5.enabled'?: boolean;
    'band5.lookaheadMs'?: number;
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band5.proportionalQ'?: boolean;
    'band5.proportionalQStrength'?: number;
    'band5.rangeDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.sidechainFreqHz'?: number;
    'band5.sidechainQ'?: number;
    'band5.slopeDbOct'?: number;
    'band5.soloed'?: boolean;
    'band5.thresholdDb'?: number;
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.attackMs'?: number;
    'band6.autoThreshold'?: boolean;
    'band6.bypassed'?: boolean;
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    'band6.detectorDelayMs'?: number;
    'band6.dynamic'?: boolean;
    'band6.enabled'?: boolean;
    'band6.lookaheadMs'?: number;
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band6.proportionalQ'?: boolean;
    'band6.proportionalQStrength'?: number;
    'band6.rangeDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.sidechainFreqHz'?: number;
    'band6.sidechainQ'?: number;
    'band6.slopeDbOct'?: number;
    'band6.soloed'?: boolean;
    'band6.thresholdDb'?: number;
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.attackMs'?: number;
    'band7.autoThreshold'?: boolean;
    'band7.bypassed'?: boolean;
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    'band7.detectorDelayMs'?: number;
    'band7.dynamic'?: boolean;
    'band7.enabled'?: boolean;
    'band7.lookaheadMs'?: number;
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band7.proportionalQ'?: boolean;
    'band7.proportionalQStrength'?: number;
    'band7.rangeDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.sidechainFreqHz'?: number;
    'band7.sidechainQ'?: number;
    'band7.slopeDbOct'?: number;
    'band7.soloed'?: boolean;
    'band7.thresholdDb'?: number;
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.attackMs'?: number;
    'band8.autoThreshold'?: boolean;
    'band8.bypassed'?: boolean;
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    'band8.detectorDelayMs'?: number;
    'band8.dynamic'?: boolean;
    'band8.enabled'?: boolean;
    'band8.lookaheadMs'?: number;
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band8.proportionalQ'?: boolean;
    'band8.proportionalQStrength'?: number;
    'band8.rangeDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.sidechainFreqHz'?: number;
    'band8.sidechainQ'?: number;
    'band8.slopeDbOct'?: number;
    'band8.soloed'?: boolean;
    'band8.thresholdDb'?: number;
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band9.attackMs'?: number;
    'band9.autoThreshold'?: boolean;
    'band9.bypassed'?: boolean;
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    'band9.detectorDelayMs'?: number;
    'band9.dynamic'?: boolean;
    'band9.enabled'?: boolean;
    'band9.lookaheadMs'?: number;
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band9.proportionalQ'?: boolean;
    'band9.proportionalQStrength'?: number;
    'band9.rangeDb'?: number;
    'band9.ratio'?: number;
    'band9.releaseMs'?: number;
    'band9.sidechainFreqHz'?: number;
    'band9.sidechainQ'?: number;
    'band9.slopeDbOct'?: number;
    'band9.soloed'?: boolean;
    'band9.thresholdDb'?: number;
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
  'eq.parametric': {
    'band0.frequencyHz'?: number;
    'band0.gainDb'?: number;
    'band0.q'?: number;
    'band1.frequencyHz'?: number;
    'band1.gainDb'?: number;
    'band1.q'?: number;
    'band2.frequencyHz'?: number;
    'band2.gainDb'?: number;
    'band2.q'?: number;
    'band3.frequencyHz'?: number;
    'band3.gainDb'?: number;
    'band3.q'?: number;
    'band4.frequencyHz'?: number;
    'band4.gainDb'?: number;
    'band4.q'?: number;
    'band5.frequencyHz'?: number;
    'band5.gainDb'?: number;
    'band5.q'?: number;
    'band6.frequencyHz'?: number;
    'band6.gainDb'?: number;
    'band6.q'?: number;
    'band7.frequencyHz'?: number;
    'band7.gainDb'?: number;
    'band7.q'?: number;
    'band8.frequencyHz'?: number;
    'band8.gainDb'?: number;
    'band8.q'?: number;
    'band9.frequencyHz'?: number;
    'band9.gainDb'?: number;
    'band9.q'?: number;
    'band10.frequencyHz'?: number;
    'band10.gainDb'?: number;
    'band10.q'?: number;
    'band11.frequencyHz'?: number;
    'band11.gainDb'?: number;
    'band11.q'?: number;
    'band12.frequencyHz'?: number;
    'band12.gainDb'?: number;
    'band12.q'?: number;
    'band13.frequencyHz'?: number;
    'band13.gainDb'?: number;
    'band13.q'?: number;
    'band14.frequencyHz'?: number;
    'band14.gainDb'?: number;
    'band14.q'?: number;
    'band15.frequencyHz'?: number;
    'band15.gainDb'?: number;
    'band15.q'?: number;
    'band16.frequencyHz'?: number;
    'band16.gainDb'?: number;
    'band16.q'?: number;
    'band17.frequencyHz'?: number;
    'band17.gainDb'?: number;
    'band17.q'?: number;
    'band18.frequencyHz'?: number;
    'band18.gainDb'?: number;
    'band18.q'?: number;
    'band19.frequencyHz'?: number;
    'band19.gainDb'?: number;
    'band19.q'?: number;
    'band20.frequencyHz'?: number;
    'band20.gainDb'?: number;
    'band20.q'?: number;
    'band21.frequencyHz'?: number;
    'band21.gainDb'?: number;
    'band21.q'?: number;
    'band22.frequencyHz'?: number;
    'band22.gainDb'?: number;
    'band22.q'?: number;
    'band23.frequencyHz'?: number;
    'band23.gainDb'?: number;
    'band23.q'?: number;
    'band0.attackMs'?: number;
    'band0.autoThreshold'?: boolean;
    'band0.bypassed'?: boolean;
    'band0.coeffMode'?: 'rbj' | 'vicanek';
    'band0.detectorDelayMs'?: number;
    'band0.dynamic'?: boolean;
    'band0.enabled'?: boolean;
    'band0.lookaheadMs'?: number;
    'band0.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band0.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band0.proportionalQ'?: boolean;
    'band0.proportionalQStrength'?: number;
    'band0.rangeDb'?: number;
    'band0.ratio'?: number;
    'band0.releaseMs'?: number;
    'band0.sidechainFreqHz'?: number;
    'band0.sidechainQ'?: number;
    'band0.slopeDbOct'?: number;
    'band0.soloed'?: boolean;
    'band0.thresholdDb'?: number;
    'band0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.attackMs'?: number;
    'band1.autoThreshold'?: boolean;
    'band1.bypassed'?: boolean;
    'band1.coeffMode'?: 'rbj' | 'vicanek';
    'band1.detectorDelayMs'?: number;
    'band1.dynamic'?: boolean;
    'band1.enabled'?: boolean;
    'band1.lookaheadMs'?: number;
    'band1.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band1.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band1.proportionalQ'?: boolean;
    'band1.proportionalQStrength'?: number;
    'band1.rangeDb'?: number;
    'band1.ratio'?: number;
    'band1.releaseMs'?: number;
    'band1.sidechainFreqHz'?: number;
    'band1.sidechainQ'?: number;
    'band1.slopeDbOct'?: number;
    'band1.soloed'?: boolean;
    'band1.thresholdDb'?: number;
    'band1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band10.attackMs'?: number;
    'band10.autoThreshold'?: boolean;
    'band10.bypassed'?: boolean;
    'band10.coeffMode'?: 'rbj' | 'vicanek';
    'band10.detectorDelayMs'?: number;
    'band10.dynamic'?: boolean;
    'band10.enabled'?: boolean;
    'band10.lookaheadMs'?: number;
    'band10.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band10.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band10.proportionalQ'?: boolean;
    'band10.proportionalQStrength'?: number;
    'band10.rangeDb'?: number;
    'band10.ratio'?: number;
    'band10.releaseMs'?: number;
    'band10.sidechainFreqHz'?: number;
    'band10.sidechainQ'?: number;
    'band10.slopeDbOct'?: number;
    'band10.soloed'?: boolean;
    'band10.thresholdDb'?: number;
    'band10.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band11.attackMs'?: number;
    'band11.autoThreshold'?: boolean;
    'band11.bypassed'?: boolean;
    'band11.coeffMode'?: 'rbj' | 'vicanek';
    'band11.detectorDelayMs'?: number;
    'band11.dynamic'?: boolean;
    'band11.enabled'?: boolean;
    'band11.lookaheadMs'?: number;
    'band11.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band11.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band11.proportionalQ'?: boolean;
    'band11.proportionalQStrength'?: number;
    'band11.rangeDb'?: number;
    'band11.ratio'?: number;
    'band11.releaseMs'?: number;
    'band11.sidechainFreqHz'?: number;
    'band11.sidechainQ'?: number;
    'band11.slopeDbOct'?: number;
    'band11.soloed'?: boolean;
    'band11.thresholdDb'?: number;
    'band11.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band12.attackMs'?: number;
    'band12.autoThreshold'?: boolean;
    'band12.bypassed'?: boolean;
    'band12.coeffMode'?: 'rbj' | 'vicanek';
    'band12.detectorDelayMs'?: number;
    'band12.dynamic'?: boolean;
    'band12.enabled'?: boolean;
    'band12.lookaheadMs'?: number;
    'band12.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band12.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band12.proportionalQ'?: boolean;
    'band12.proportionalQStrength'?: number;
    'band12.rangeDb'?: number;
    'band12.ratio'?: number;
    'band12.releaseMs'?: number;
    'band12.sidechainFreqHz'?: number;
    'band12.sidechainQ'?: number;
    'band12.slopeDbOct'?: number;
    'band12.soloed'?: boolean;
    'band12.thresholdDb'?: number;
    'band12.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band13.attackMs'?: number;
    'band13.autoThreshold'?: boolean;
    'band13.bypassed'?: boolean;
    'band13.coeffMode'?: 'rbj' | 'vicanek';
    'band13.detectorDelayMs'?: number;
    'band13.dynamic'?: boolean;
    'band13.enabled'?: boolean;
    'band13.lookaheadMs'?: number;
    'band13.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band13.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band13.proportionalQ'?: boolean;
    'band13.proportionalQStrength'?: number;
    'band13.rangeDb'?: number;
    'band13.ratio'?: number;
    'band13.releaseMs'?: number;
    'band13.sidechainFreqHz'?: number;
    'band13.sidechainQ'?: number;
    'band13.slopeDbOct'?: number;
    'band13.soloed'?: boolean;
    'band13.thresholdDb'?: number;
    'band13.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band14.attackMs'?: number;
    'band14.autoThreshold'?: boolean;
    'band14.bypassed'?: boolean;
    'band14.coeffMode'?: 'rbj' | 'vicanek';
    'band14.detectorDelayMs'?: number;
    'band14.dynamic'?: boolean;
    'band14.enabled'?: boolean;
    'band14.lookaheadMs'?: number;
    'band14.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band14.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band14.proportionalQ'?: boolean;
    'band14.proportionalQStrength'?: number;
    'band14.rangeDb'?: number;
    'band14.ratio'?: number;
    'band14.releaseMs'?: number;
    'band14.sidechainFreqHz'?: number;
    'band14.sidechainQ'?: number;
    'band14.slopeDbOct'?: number;
    'band14.soloed'?: boolean;
    'band14.thresholdDb'?: number;
    'band14.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band15.attackMs'?: number;
    'band15.autoThreshold'?: boolean;
    'band15.bypassed'?: boolean;
    'band15.coeffMode'?: 'rbj' | 'vicanek';
    'band15.detectorDelayMs'?: number;
    'band15.dynamic'?: boolean;
    'band15.enabled'?: boolean;
    'band15.lookaheadMs'?: number;
    'band15.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band15.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band15.proportionalQ'?: boolean;
    'band15.proportionalQStrength'?: number;
    'band15.rangeDb'?: number;
    'band15.ratio'?: number;
    'band15.releaseMs'?: number;
    'band15.sidechainFreqHz'?: number;
    'band15.sidechainQ'?: number;
    'band15.slopeDbOct'?: number;
    'band15.soloed'?: boolean;
    'band15.thresholdDb'?: number;
    'band15.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band16.attackMs'?: number;
    'band16.autoThreshold'?: boolean;
    'band16.bypassed'?: boolean;
    'band16.coeffMode'?: 'rbj' | 'vicanek';
    'band16.detectorDelayMs'?: number;
    'band16.dynamic'?: boolean;
    'band16.enabled'?: boolean;
    'band16.lookaheadMs'?: number;
    'band16.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band16.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band16.proportionalQ'?: boolean;
    'band16.proportionalQStrength'?: number;
    'band16.rangeDb'?: number;
    'band16.ratio'?: number;
    'band16.releaseMs'?: number;
    'band16.sidechainFreqHz'?: number;
    'band16.sidechainQ'?: number;
    'band16.slopeDbOct'?: number;
    'band16.soloed'?: boolean;
    'band16.thresholdDb'?: number;
    'band16.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band17.attackMs'?: number;
    'band17.autoThreshold'?: boolean;
    'band17.bypassed'?: boolean;
    'band17.coeffMode'?: 'rbj' | 'vicanek';
    'band17.detectorDelayMs'?: number;
    'band17.dynamic'?: boolean;
    'band17.enabled'?: boolean;
    'band17.lookaheadMs'?: number;
    'band17.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band17.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band17.proportionalQ'?: boolean;
    'band17.proportionalQStrength'?: number;
    'band17.rangeDb'?: number;
    'band17.ratio'?: number;
    'band17.releaseMs'?: number;
    'band17.sidechainFreqHz'?: number;
    'band17.sidechainQ'?: number;
    'band17.slopeDbOct'?: number;
    'band17.soloed'?: boolean;
    'band17.thresholdDb'?: number;
    'band17.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band18.attackMs'?: number;
    'band18.autoThreshold'?: boolean;
    'band18.bypassed'?: boolean;
    'band18.coeffMode'?: 'rbj' | 'vicanek';
    'band18.detectorDelayMs'?: number;
    'band18.dynamic'?: boolean;
    'band18.enabled'?: boolean;
    'band18.lookaheadMs'?: number;
    'band18.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band18.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band18.proportionalQ'?: boolean;
    'band18.proportionalQStrength'?: number;
    'band18.rangeDb'?: number;
    'band18.ratio'?: number;
    'band18.releaseMs'?: number;
    'band18.sidechainFreqHz'?: number;
    'band18.sidechainQ'?: number;
    'band18.slopeDbOct'?: number;
    'band18.soloed'?: boolean;
    'band18.thresholdDb'?: number;
    'band18.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band19.attackMs'?: number;
    'band19.autoThreshold'?: boolean;
    'band19.bypassed'?: boolean;
    'band19.coeffMode'?: 'rbj' | 'vicanek';
    'band19.detectorDelayMs'?: number;
    'band19.dynamic'?: boolean;
    'band19.enabled'?: boolean;
    'band19.lookaheadMs'?: number;
    'band19.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band19.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band19.proportionalQ'?: boolean;
    'band19.proportionalQStrength'?: number;
    'band19.rangeDb'?: number;
    'band19.ratio'?: number;
    'band19.releaseMs'?: number;
    'band19.sidechainFreqHz'?: number;
    'band19.sidechainQ'?: number;
    'band19.slopeDbOct'?: number;
    'band19.soloed'?: boolean;
    'band19.thresholdDb'?: number;
    'band19.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.attackMs'?: number;
    'band2.autoThreshold'?: boolean;
    'band2.bypassed'?: boolean;
    'band2.coeffMode'?: 'rbj' | 'vicanek';
    'band2.detectorDelayMs'?: number;
    'band2.dynamic'?: boolean;
    'band2.enabled'?: boolean;
    'band2.lookaheadMs'?: number;
    'band2.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band2.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band2.proportionalQ'?: boolean;
    'band2.proportionalQStrength'?: number;
    'band2.rangeDb'?: number;
    'band2.ratio'?: number;
    'band2.releaseMs'?: number;
    'band2.sidechainFreqHz'?: number;
    'band2.sidechainQ'?: number;
    'band2.slopeDbOct'?: number;
    'band2.soloed'?: boolean;
    'band2.thresholdDb'?: number;
    'band2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band20.attackMs'?: number;
    'band20.autoThreshold'?: boolean;
    'band20.bypassed'?: boolean;
    'band20.coeffMode'?: 'rbj' | 'vicanek';
    'band20.detectorDelayMs'?: number;
    'band20.dynamic'?: boolean;
    'band20.enabled'?: boolean;
    'band20.lookaheadMs'?: number;
    'band20.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band20.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band20.proportionalQ'?: boolean;
    'band20.proportionalQStrength'?: number;
    'band20.rangeDb'?: number;
    'band20.ratio'?: number;
    'band20.releaseMs'?: number;
    'band20.sidechainFreqHz'?: number;
    'band20.sidechainQ'?: number;
    'band20.slopeDbOct'?: number;
    'band20.soloed'?: boolean;
    'band20.thresholdDb'?: number;
    'band20.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band21.attackMs'?: number;
    'band21.autoThreshold'?: boolean;
    'band21.bypassed'?: boolean;
    'band21.coeffMode'?: 'rbj' | 'vicanek';
    'band21.detectorDelayMs'?: number;
    'band21.dynamic'?: boolean;
    'band21.enabled'?: boolean;
    'band21.lookaheadMs'?: number;
    'band21.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band21.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band21.proportionalQ'?: boolean;
    'band21.proportionalQStrength'?: number;
    'band21.rangeDb'?: number;
    'band21.ratio'?: number;
    'band21.releaseMs'?: number;
    'band21.sidechainFreqHz'?: number;
    'band21.sidechainQ'?: number;
    'band21.slopeDbOct'?: number;
    'band21.soloed'?: boolean;
    'band21.thresholdDb'?: number;
    'band21.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band22.attackMs'?: number;
    'band22.autoThreshold'?: boolean;
    'band22.bypassed'?: boolean;
    'band22.coeffMode'?: 'rbj' | 'vicanek';
    'band22.detectorDelayMs'?: number;
    'band22.dynamic'?: boolean;
    'band22.enabled'?: boolean;
    'band22.lookaheadMs'?: number;
    'band22.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band22.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band22.proportionalQ'?: boolean;
    'band22.proportionalQStrength'?: number;
    'band22.rangeDb'?: number;
    'band22.ratio'?: number;
    'band22.releaseMs'?: number;
    'band22.sidechainFreqHz'?: number;
    'band22.sidechainQ'?: number;
    'band22.slopeDbOct'?: number;
    'band22.soloed'?: boolean;
    'band22.thresholdDb'?: number;
    'band22.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band23.attackMs'?: number;
    'band23.autoThreshold'?: boolean;
    'band23.bypassed'?: boolean;
    'band23.coeffMode'?: 'rbj' | 'vicanek';
    'band23.detectorDelayMs'?: number;
    'band23.dynamic'?: boolean;
    'band23.enabled'?: boolean;
    'band23.lookaheadMs'?: number;
    'band23.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band23.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band23.proportionalQ'?: boolean;
    'band23.proportionalQStrength'?: number;
    'band23.rangeDb'?: number;
    'band23.ratio'?: number;
    'band23.releaseMs'?: number;
    'band23.sidechainFreqHz'?: number;
    'band23.sidechainQ'?: number;
    'band23.slopeDbOct'?: number;
    'band23.soloed'?: boolean;
    'band23.thresholdDb'?: number;
    'band23.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.attackMs'?: number;
    'band3.autoThreshold'?: boolean;
    'band3.bypassed'?: boolean;
    'band3.coeffMode'?: 'rbj' | 'vicanek';
    'band3.detectorDelayMs'?: number;
    'band3.dynamic'?: boolean;
    'band3.enabled'?: boolean;
    'band3.lookaheadMs'?: number;
    'band3.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band3.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band3.proportionalQ'?: boolean;
    'band3.proportionalQStrength'?: number;
    'band3.rangeDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.sidechainFreqHz'?: number;
    'band3.sidechainQ'?: number;
    'band3.slopeDbOct'?: number;
    'band3.soloed'?: boolean;
    'band3.thresholdDb'?: number;
    'band3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.attackMs'?: number;
    'band4.autoThreshold'?: boolean;
    'band4.bypassed'?: boolean;
    'band4.coeffMode'?: 'rbj' | 'vicanek';
    'band4.detectorDelayMs'?: number;
    'band4.dynamic'?: boolean;
    'band4.enabled'?: boolean;
    'band4.lookaheadMs'?: number;
    'band4.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band4.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band4.proportionalQ'?: boolean;
    'band4.proportionalQStrength'?: number;
    'band4.rangeDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.sidechainFreqHz'?: number;
    'band4.sidechainQ'?: number;
    'band4.slopeDbOct'?: number;
    'band4.soloed'?: boolean;
    'band4.thresholdDb'?: number;
    'band4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.attackMs'?: number;
    'band5.autoThreshold'?: boolean;
    'band5.bypassed'?: boolean;
    'band5.coeffMode'?: 'rbj' | 'vicanek';
    'band5.detectorDelayMs'?: number;
    'band5.dynamic'?: boolean;
    'band5.enabled'?: boolean;
    'band5.lookaheadMs'?: number;
    'band5.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band5.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band5.proportionalQ'?: boolean;
    'band5.proportionalQStrength'?: number;
    'band5.rangeDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.sidechainFreqHz'?: number;
    'band5.sidechainQ'?: number;
    'band5.slopeDbOct'?: number;
    'band5.soloed'?: boolean;
    'band5.thresholdDb'?: number;
    'band5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.attackMs'?: number;
    'band6.autoThreshold'?: boolean;
    'band6.bypassed'?: boolean;
    'band6.coeffMode'?: 'rbj' | 'vicanek';
    'band6.detectorDelayMs'?: number;
    'band6.dynamic'?: boolean;
    'band6.enabled'?: boolean;
    'band6.lookaheadMs'?: number;
    'band6.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band6.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band6.proportionalQ'?: boolean;
    'band6.proportionalQStrength'?: number;
    'band6.rangeDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.sidechainFreqHz'?: number;
    'band6.sidechainQ'?: number;
    'band6.slopeDbOct'?: number;
    'band6.soloed'?: boolean;
    'band6.thresholdDb'?: number;
    'band6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.attackMs'?: number;
    'band7.autoThreshold'?: boolean;
    'band7.bypassed'?: boolean;
    'band7.coeffMode'?: 'rbj' | 'vicanek';
    'band7.detectorDelayMs'?: number;
    'band7.dynamic'?: boolean;
    'band7.enabled'?: boolean;
    'band7.lookaheadMs'?: number;
    'band7.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band7.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band7.proportionalQ'?: boolean;
    'band7.proportionalQStrength'?: number;
    'band7.rangeDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.sidechainFreqHz'?: number;
    'band7.sidechainQ'?: number;
    'band7.slopeDbOct'?: number;
    'band7.soloed'?: boolean;
    'band7.thresholdDb'?: number;
    'band7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.attackMs'?: number;
    'band8.autoThreshold'?: boolean;
    'band8.bypassed'?: boolean;
    'band8.coeffMode'?: 'rbj' | 'vicanek';
    'band8.detectorDelayMs'?: number;
    'band8.dynamic'?: boolean;
    'band8.enabled'?: boolean;
    'band8.lookaheadMs'?: number;
    'band8.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band8.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band8.proportionalQ'?: boolean;
    'band8.proportionalQStrength'?: number;
    'band8.rangeDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.sidechainFreqHz'?: number;
    'band8.sidechainQ'?: number;
    'band8.slopeDbOct'?: number;
    'band8.soloed'?: boolean;
    'band8.thresholdDb'?: number;
    'band8.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band9.attackMs'?: number;
    'band9.autoThreshold'?: boolean;
    'band9.bypassed'?: boolean;
    'band9.coeffMode'?: 'rbj' | 'vicanek';
    'band9.detectorDelayMs'?: number;
    'band9.dynamic'?: boolean;
    'band9.enabled'?: boolean;
    'band9.lookaheadMs'?: number;
    'band9.phase'?: 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';
    'band9.placement'?: 'stereo' | 'left' | 'right' | 'mid' | 'side';
    'band9.proportionalQ'?: boolean;
    'band9.proportionalQStrength'?: number;
    'band9.rangeDb'?: number;
    'band9.ratio'?: number;
    'band9.releaseMs'?: number;
    'band9.sidechainFreqHz'?: number;
    'band9.sidechainQ'?: number;
    'band9.slopeDbOct'?: number;
    'band9.soloed'?: boolean;
    'band9.thresholdDb'?: number;
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
  'eq.passiveTube': {
    lowFrequencyHz?: number;
    lowBoost?: number;
    lowAttenuation?: number;
    highBoostFrequencyHz?: number;
    highBoost?: number;
    highBandwidth?: number;
    highAttenuationFrequencyHz?: number;
    highAttenuation?: number;
    outputDrive?: number;
    componentModel?: 'curveOnly' | 'wdfCircuit';
  };
  'eq.shelving': {
    lowFrequencyHz?: number;
    lowGainDb?: number;
    lowQ?: number;
    highFrequencyHz?: number;
    highGainDb?: number;
    highQ?: number;
    highEnabled?: boolean;
    lowEnabled?: boolean;
  };
  'eq.steppedProportionalQ': {
    lowFrequencyHz?: number;
    lowGainDb?: number;
    lowMidFrequencyHz?: number;
    lowMidGainDb?: number;
    highMidFrequencyHz?: number;
    highMidGainDb?: number;
    highFrequencyHz?: number;
    highGainDb?: number;
  };
  'eq.tilt': {
    tiltDb?: number;
    pivotHz?: number;
  };
  'final.bitDepth': Record<string, never>;
  'final.dither': Record<string, never>;
  'final.outputChain': Record<string, never>;
  'match.abCrossfade': Record<string, never>;
  'match.abSwitch': Record<string, never>;
  'match.alignReferenceToSource': Record<string, never>;
  'match.applyMatchEq': Record<string, never>;
  'maximizer.adaptiveRelease': {
    ceilingDb?: number;
    minReleaseMs?: number;
    maxReleaseMs?: number;
    crestWindowMs?: number;
    crestLow?: number;
    crestHigh?: number;
    releaseSmoothingMs?: number;
    lookaheadMs?: number;
  };
  'maximizer.loudnessOptimize': Record<string, never>;
  'maximizer.maximizer': {
    inputGainDb?: number;
    ceilingDb?: number;
    releaseMs?: number;
    lookaheadMs?: number;
  };
  'maximizer.softKneeMax': {
    inputGainDb?: number;
    ceilingDb?: number;
    kneeDb?: number;
    releaseMs?: number;
  };
  'maximizer.truePeakLimiter': {
    ceilingDb?: number;
    releaseMs?: number;
    applyGainAtInputRate?: boolean;
    lookaheadMs?: number;
    oversampleFactor?: number;
  };
  'multiband.compressor': {
    'band0.thresholdDb'?: number;
    'band0.ratio'?: number;
    'band0.attackMs'?: number;
    'band0.releaseMs'?: number;
    'band0.makeupGainDb'?: number;
    'band1.thresholdDb'?: number;
    'band1.ratio'?: number;
    'band1.attackMs'?: number;
    'band1.releaseMs'?: number;
    'band1.makeupGainDb'?: number;
    'band2.thresholdDb'?: number;
    'band2.ratio'?: number;
    'band2.attackMs'?: number;
    'band2.releaseMs'?: number;
    'band2.makeupGainDb'?: number;
    'band0.kneeDb'?: number;
    'band1.kneeDb'?: number;
    'band2.kneeDb'?: number;
    'band3.attackMs'?: number;
    'band3.kneeDb'?: number;
    'band3.makeupGainDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.thresholdDb'?: number;
    'band4.attackMs'?: number;
    'band4.kneeDb'?: number;
    'band4.makeupGainDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.thresholdDb'?: number;
    'band5.attackMs'?: number;
    'band5.kneeDb'?: number;
    'band5.makeupGainDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.thresholdDb'?: number;
    'band6.attackMs'?: number;
    'band6.kneeDb'?: number;
    'band6.makeupGainDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.thresholdDb'?: number;
    'band7.attackMs'?: number;
    'band7.kneeDb'?: number;
    'band7.makeupGainDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.thresholdDb'?: number;
    'band8.attackMs'?: number;
    'band8.kneeDb'?: number;
    'band8.makeupGainDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.thresholdDb'?: number;
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'multiband.dynamicEq': {
    'band0.dyn0.frequencyHz'?: number;
    'band0.dyn0.staticGainDb'?: number;
    'band0.dyn0.q'?: number;
    'band0.dyn0.thresholdDb'?: number;
    'band0.dyn0.ratio'?: number;
    'band0.dyn0.rangeDb'?: number;
    'band0.dyn0.sidechainQ'?: number;
    'band0.dyn0.sidechainFreqHz'?: number;
    'band0.dyn0.attackMs'?: number;
    'band0.dyn0.releaseMs'?: number;
    'band0.dyn0.detectorDelayMs'?: number;
    'band0.dyn0.lookaheadMs'?: number;
    'band0.dyn1.frequencyHz'?: number;
    'band0.dyn1.staticGainDb'?: number;
    'band0.dyn1.q'?: number;
    'band0.dyn1.thresholdDb'?: number;
    'band0.dyn1.ratio'?: number;
    'band0.dyn1.rangeDb'?: number;
    'band0.dyn1.sidechainQ'?: number;
    'band0.dyn1.sidechainFreqHz'?: number;
    'band0.dyn1.attackMs'?: number;
    'band0.dyn1.releaseMs'?: number;
    'band0.dyn1.detectorDelayMs'?: number;
    'band0.dyn1.lookaheadMs'?: number;
    'band0.dyn2.frequencyHz'?: number;
    'band0.dyn2.staticGainDb'?: number;
    'band0.dyn2.q'?: number;
    'band0.dyn2.thresholdDb'?: number;
    'band0.dyn2.ratio'?: number;
    'band0.dyn2.rangeDb'?: number;
    'band0.dyn2.sidechainQ'?: number;
    'band0.dyn2.sidechainFreqHz'?: number;
    'band0.dyn2.attackMs'?: number;
    'band0.dyn2.releaseMs'?: number;
    'band0.dyn2.detectorDelayMs'?: number;
    'band0.dyn2.lookaheadMs'?: number;
    'band0.dyn3.frequencyHz'?: number;
    'band0.dyn3.staticGainDb'?: number;
    'band0.dyn3.q'?: number;
    'band0.dyn3.thresholdDb'?: number;
    'band0.dyn3.ratio'?: number;
    'band0.dyn3.rangeDb'?: number;
    'band0.dyn3.sidechainQ'?: number;
    'band0.dyn3.sidechainFreqHz'?: number;
    'band0.dyn3.attackMs'?: number;
    'band0.dyn3.releaseMs'?: number;
    'band0.dyn3.detectorDelayMs'?: number;
    'band0.dyn3.lookaheadMs'?: number;
    'band0.dyn4.frequencyHz'?: number;
    'band0.dyn4.staticGainDb'?: number;
    'band0.dyn4.q'?: number;
    'band0.dyn4.thresholdDb'?: number;
    'band0.dyn4.ratio'?: number;
    'band0.dyn4.rangeDb'?: number;
    'band0.dyn4.sidechainQ'?: number;
    'band0.dyn4.sidechainFreqHz'?: number;
    'band0.dyn4.attackMs'?: number;
    'band0.dyn4.releaseMs'?: number;
    'band0.dyn4.detectorDelayMs'?: number;
    'band0.dyn4.lookaheadMs'?: number;
    'band0.dyn5.frequencyHz'?: number;
    'band0.dyn5.staticGainDb'?: number;
    'band0.dyn5.q'?: number;
    'band0.dyn5.thresholdDb'?: number;
    'band0.dyn5.ratio'?: number;
    'band0.dyn5.rangeDb'?: number;
    'band0.dyn5.sidechainQ'?: number;
    'band0.dyn5.sidechainFreqHz'?: number;
    'band0.dyn5.attackMs'?: number;
    'band0.dyn5.releaseMs'?: number;
    'band0.dyn5.detectorDelayMs'?: number;
    'band0.dyn5.lookaheadMs'?: number;
    'band0.dyn6.frequencyHz'?: number;
    'band0.dyn6.staticGainDb'?: number;
    'band0.dyn6.q'?: number;
    'band0.dyn6.thresholdDb'?: number;
    'band0.dyn6.ratio'?: number;
    'band0.dyn6.rangeDb'?: number;
    'band0.dyn6.sidechainQ'?: number;
    'band0.dyn6.sidechainFreqHz'?: number;
    'band0.dyn6.attackMs'?: number;
    'band0.dyn6.releaseMs'?: number;
    'band0.dyn6.detectorDelayMs'?: number;
    'band0.dyn6.lookaheadMs'?: number;
    'band0.dyn7.frequencyHz'?: number;
    'band0.dyn7.staticGainDb'?: number;
    'band0.dyn7.q'?: number;
    'band0.dyn7.thresholdDb'?: number;
    'band0.dyn7.ratio'?: number;
    'band0.dyn7.rangeDb'?: number;
    'band0.dyn7.sidechainQ'?: number;
    'band0.dyn7.sidechainFreqHz'?: number;
    'band0.dyn7.attackMs'?: number;
    'band0.dyn7.releaseMs'?: number;
    'band0.dyn7.detectorDelayMs'?: number;
    'band0.dyn7.lookaheadMs'?: number;
    'band1.dyn0.frequencyHz'?: number;
    'band1.dyn0.staticGainDb'?: number;
    'band1.dyn0.q'?: number;
    'band1.dyn0.thresholdDb'?: number;
    'band1.dyn0.ratio'?: number;
    'band1.dyn0.rangeDb'?: number;
    'band1.dyn0.sidechainQ'?: number;
    'band1.dyn0.sidechainFreqHz'?: number;
    'band1.dyn0.attackMs'?: number;
    'band1.dyn0.releaseMs'?: number;
    'band1.dyn0.detectorDelayMs'?: number;
    'band1.dyn0.lookaheadMs'?: number;
    'band1.dyn1.frequencyHz'?: number;
    'band1.dyn1.staticGainDb'?: number;
    'band1.dyn1.q'?: number;
    'band1.dyn1.thresholdDb'?: number;
    'band1.dyn1.ratio'?: number;
    'band1.dyn1.rangeDb'?: number;
    'band1.dyn1.sidechainQ'?: number;
    'band1.dyn1.sidechainFreqHz'?: number;
    'band1.dyn1.attackMs'?: number;
    'band1.dyn1.releaseMs'?: number;
    'band1.dyn1.detectorDelayMs'?: number;
    'band1.dyn1.lookaheadMs'?: number;
    'band1.dyn2.frequencyHz'?: number;
    'band1.dyn2.staticGainDb'?: number;
    'band1.dyn2.q'?: number;
    'band1.dyn2.thresholdDb'?: number;
    'band1.dyn2.ratio'?: number;
    'band1.dyn2.rangeDb'?: number;
    'band1.dyn2.sidechainQ'?: number;
    'band1.dyn2.sidechainFreqHz'?: number;
    'band1.dyn2.attackMs'?: number;
    'band1.dyn2.releaseMs'?: number;
    'band1.dyn2.detectorDelayMs'?: number;
    'band1.dyn2.lookaheadMs'?: number;
    'band1.dyn3.frequencyHz'?: number;
    'band1.dyn3.staticGainDb'?: number;
    'band1.dyn3.q'?: number;
    'band1.dyn3.thresholdDb'?: number;
    'band1.dyn3.ratio'?: number;
    'band1.dyn3.rangeDb'?: number;
    'band1.dyn3.sidechainQ'?: number;
    'band1.dyn3.sidechainFreqHz'?: number;
    'band1.dyn3.attackMs'?: number;
    'band1.dyn3.releaseMs'?: number;
    'band1.dyn3.detectorDelayMs'?: number;
    'band1.dyn3.lookaheadMs'?: number;
    'band1.dyn4.frequencyHz'?: number;
    'band1.dyn4.staticGainDb'?: number;
    'band1.dyn4.q'?: number;
    'band1.dyn4.thresholdDb'?: number;
    'band1.dyn4.ratio'?: number;
    'band1.dyn4.rangeDb'?: number;
    'band1.dyn4.sidechainQ'?: number;
    'band1.dyn4.sidechainFreqHz'?: number;
    'band1.dyn4.attackMs'?: number;
    'band1.dyn4.releaseMs'?: number;
    'band1.dyn4.detectorDelayMs'?: number;
    'band1.dyn4.lookaheadMs'?: number;
    'band1.dyn5.frequencyHz'?: number;
    'band1.dyn5.staticGainDb'?: number;
    'band1.dyn5.q'?: number;
    'band1.dyn5.thresholdDb'?: number;
    'band1.dyn5.ratio'?: number;
    'band1.dyn5.rangeDb'?: number;
    'band1.dyn5.sidechainQ'?: number;
    'band1.dyn5.sidechainFreqHz'?: number;
    'band1.dyn5.attackMs'?: number;
    'band1.dyn5.releaseMs'?: number;
    'band1.dyn5.detectorDelayMs'?: number;
    'band1.dyn5.lookaheadMs'?: number;
    'band1.dyn6.frequencyHz'?: number;
    'band1.dyn6.staticGainDb'?: number;
    'band1.dyn6.q'?: number;
    'band1.dyn6.thresholdDb'?: number;
    'band1.dyn6.ratio'?: number;
    'band1.dyn6.rangeDb'?: number;
    'band1.dyn6.sidechainQ'?: number;
    'band1.dyn6.sidechainFreqHz'?: number;
    'band1.dyn6.attackMs'?: number;
    'band1.dyn6.releaseMs'?: number;
    'band1.dyn6.detectorDelayMs'?: number;
    'band1.dyn6.lookaheadMs'?: number;
    'band1.dyn7.frequencyHz'?: number;
    'band1.dyn7.staticGainDb'?: number;
    'band1.dyn7.q'?: number;
    'band1.dyn7.thresholdDb'?: number;
    'band1.dyn7.ratio'?: number;
    'band1.dyn7.rangeDb'?: number;
    'band1.dyn7.sidechainQ'?: number;
    'band1.dyn7.sidechainFreqHz'?: number;
    'band1.dyn7.attackMs'?: number;
    'band1.dyn7.releaseMs'?: number;
    'band1.dyn7.detectorDelayMs'?: number;
    'band1.dyn7.lookaheadMs'?: number;
    'band2.dyn0.frequencyHz'?: number;
    'band2.dyn0.staticGainDb'?: number;
    'band2.dyn0.q'?: number;
    'band2.dyn0.thresholdDb'?: number;
    'band2.dyn0.ratio'?: number;
    'band2.dyn0.rangeDb'?: number;
    'band2.dyn0.sidechainQ'?: number;
    'band2.dyn0.sidechainFreqHz'?: number;
    'band2.dyn0.attackMs'?: number;
    'band2.dyn0.releaseMs'?: number;
    'band2.dyn0.detectorDelayMs'?: number;
    'band2.dyn0.lookaheadMs'?: number;
    'band2.dyn1.frequencyHz'?: number;
    'band2.dyn1.staticGainDb'?: number;
    'band2.dyn1.q'?: number;
    'band2.dyn1.thresholdDb'?: number;
    'band2.dyn1.ratio'?: number;
    'band2.dyn1.rangeDb'?: number;
    'band2.dyn1.sidechainQ'?: number;
    'band2.dyn1.sidechainFreqHz'?: number;
    'band2.dyn1.attackMs'?: number;
    'band2.dyn1.releaseMs'?: number;
    'band2.dyn1.detectorDelayMs'?: number;
    'band2.dyn1.lookaheadMs'?: number;
    'band2.dyn2.frequencyHz'?: number;
    'band2.dyn2.staticGainDb'?: number;
    'band2.dyn2.q'?: number;
    'band2.dyn2.thresholdDb'?: number;
    'band2.dyn2.ratio'?: number;
    'band2.dyn2.rangeDb'?: number;
    'band2.dyn2.sidechainQ'?: number;
    'band2.dyn2.sidechainFreqHz'?: number;
    'band2.dyn2.attackMs'?: number;
    'band2.dyn2.releaseMs'?: number;
    'band2.dyn2.detectorDelayMs'?: number;
    'band2.dyn2.lookaheadMs'?: number;
    'band2.dyn3.frequencyHz'?: number;
    'band2.dyn3.staticGainDb'?: number;
    'band2.dyn3.q'?: number;
    'band2.dyn3.thresholdDb'?: number;
    'band2.dyn3.ratio'?: number;
    'band2.dyn3.rangeDb'?: number;
    'band2.dyn3.sidechainQ'?: number;
    'band2.dyn3.sidechainFreqHz'?: number;
    'band2.dyn3.attackMs'?: number;
    'band2.dyn3.releaseMs'?: number;
    'band2.dyn3.detectorDelayMs'?: number;
    'band2.dyn3.lookaheadMs'?: number;
    'band2.dyn4.frequencyHz'?: number;
    'band2.dyn4.staticGainDb'?: number;
    'band2.dyn4.q'?: number;
    'band2.dyn4.thresholdDb'?: number;
    'band2.dyn4.ratio'?: number;
    'band2.dyn4.rangeDb'?: number;
    'band2.dyn4.sidechainQ'?: number;
    'band2.dyn4.sidechainFreqHz'?: number;
    'band2.dyn4.attackMs'?: number;
    'band2.dyn4.releaseMs'?: number;
    'band2.dyn4.detectorDelayMs'?: number;
    'band2.dyn4.lookaheadMs'?: number;
    'band2.dyn5.frequencyHz'?: number;
    'band2.dyn5.staticGainDb'?: number;
    'band2.dyn5.q'?: number;
    'band2.dyn5.thresholdDb'?: number;
    'band2.dyn5.ratio'?: number;
    'band2.dyn5.rangeDb'?: number;
    'band2.dyn5.sidechainQ'?: number;
    'band2.dyn5.sidechainFreqHz'?: number;
    'band2.dyn5.attackMs'?: number;
    'band2.dyn5.releaseMs'?: number;
    'band2.dyn5.detectorDelayMs'?: number;
    'band2.dyn5.lookaheadMs'?: number;
    'band2.dyn6.frequencyHz'?: number;
    'band2.dyn6.staticGainDb'?: number;
    'band2.dyn6.q'?: number;
    'band2.dyn6.thresholdDb'?: number;
    'band2.dyn6.ratio'?: number;
    'band2.dyn6.rangeDb'?: number;
    'band2.dyn6.sidechainQ'?: number;
    'band2.dyn6.sidechainFreqHz'?: number;
    'band2.dyn6.attackMs'?: number;
    'band2.dyn6.releaseMs'?: number;
    'band2.dyn6.detectorDelayMs'?: number;
    'band2.dyn6.lookaheadMs'?: number;
    'band2.dyn7.frequencyHz'?: number;
    'band2.dyn7.staticGainDb'?: number;
    'band2.dyn7.q'?: number;
    'band2.dyn7.thresholdDb'?: number;
    'band2.dyn7.ratio'?: number;
    'band2.dyn7.rangeDb'?: number;
    'band2.dyn7.sidechainQ'?: number;
    'band2.dyn7.sidechainFreqHz'?: number;
    'band2.dyn7.attackMs'?: number;
    'band2.dyn7.releaseMs'?: number;
    'band2.dyn7.detectorDelayMs'?: number;
    'band2.dyn7.lookaheadMs'?: number;
    'band0.dyn0.enabled'?: boolean;
    'band0.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn1.enabled'?: boolean;
    'band0.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn2.enabled'?: boolean;
    'band0.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn3.enabled'?: boolean;
    'band0.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn4.enabled'?: boolean;
    'band0.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn5.enabled'?: boolean;
    'band0.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn6.enabled'?: boolean;
    'band0.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band0.dyn7.enabled'?: boolean;
    'band0.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn0.enabled'?: boolean;
    'band1.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn1.enabled'?: boolean;
    'band1.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn2.enabled'?: boolean;
    'band1.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn3.enabled'?: boolean;
    'band1.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn4.enabled'?: boolean;
    'band1.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn5.enabled'?: boolean;
    'band1.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn6.enabled'?: boolean;
    'band1.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band1.dyn7.enabled'?: boolean;
    'band1.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn0.enabled'?: boolean;
    'band2.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn1.enabled'?: boolean;
    'band2.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn2.enabled'?: boolean;
    'band2.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn3.enabled'?: boolean;
    'band2.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn4.enabled'?: boolean;
    'band2.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn5.enabled'?: boolean;
    'band2.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn6.enabled'?: boolean;
    'band2.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band2.dyn7.enabled'?: boolean;
    'band2.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn0.attackMs'?: number;
    'band3.dyn0.detectorDelayMs'?: number;
    'band3.dyn0.enabled'?: boolean;
    'band3.dyn0.frequencyHz'?: number;
    'band3.dyn0.lookaheadMs'?: number;
    'band3.dyn0.q'?: number;
    'band3.dyn0.rangeDb'?: number;
    'band3.dyn0.ratio'?: number;
    'band3.dyn0.releaseMs'?: number;
    'band3.dyn0.sidechainFreqHz'?: number;
    'band3.dyn0.sidechainQ'?: number;
    'band3.dyn0.staticGainDb'?: number;
    'band3.dyn0.thresholdDb'?: number;
    'band3.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn1.attackMs'?: number;
    'band3.dyn1.detectorDelayMs'?: number;
    'band3.dyn1.enabled'?: boolean;
    'band3.dyn1.frequencyHz'?: number;
    'band3.dyn1.lookaheadMs'?: number;
    'band3.dyn1.q'?: number;
    'band3.dyn1.rangeDb'?: number;
    'band3.dyn1.ratio'?: number;
    'band3.dyn1.releaseMs'?: number;
    'band3.dyn1.sidechainFreqHz'?: number;
    'band3.dyn1.sidechainQ'?: number;
    'band3.dyn1.staticGainDb'?: number;
    'band3.dyn1.thresholdDb'?: number;
    'band3.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn2.attackMs'?: number;
    'band3.dyn2.detectorDelayMs'?: number;
    'band3.dyn2.enabled'?: boolean;
    'band3.dyn2.frequencyHz'?: number;
    'band3.dyn2.lookaheadMs'?: number;
    'band3.dyn2.q'?: number;
    'band3.dyn2.rangeDb'?: number;
    'band3.dyn2.ratio'?: number;
    'band3.dyn2.releaseMs'?: number;
    'band3.dyn2.sidechainFreqHz'?: number;
    'band3.dyn2.sidechainQ'?: number;
    'band3.dyn2.staticGainDb'?: number;
    'band3.dyn2.thresholdDb'?: number;
    'band3.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn3.attackMs'?: number;
    'band3.dyn3.detectorDelayMs'?: number;
    'band3.dyn3.enabled'?: boolean;
    'band3.dyn3.frequencyHz'?: number;
    'band3.dyn3.lookaheadMs'?: number;
    'band3.dyn3.q'?: number;
    'band3.dyn3.rangeDb'?: number;
    'band3.dyn3.ratio'?: number;
    'band3.dyn3.releaseMs'?: number;
    'band3.dyn3.sidechainFreqHz'?: number;
    'band3.dyn3.sidechainQ'?: number;
    'band3.dyn3.staticGainDb'?: number;
    'band3.dyn3.thresholdDb'?: number;
    'band3.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn4.attackMs'?: number;
    'band3.dyn4.detectorDelayMs'?: number;
    'band3.dyn4.enabled'?: boolean;
    'band3.dyn4.frequencyHz'?: number;
    'band3.dyn4.lookaheadMs'?: number;
    'band3.dyn4.q'?: number;
    'band3.dyn4.rangeDb'?: number;
    'band3.dyn4.ratio'?: number;
    'band3.dyn4.releaseMs'?: number;
    'band3.dyn4.sidechainFreqHz'?: number;
    'band3.dyn4.sidechainQ'?: number;
    'band3.dyn4.staticGainDb'?: number;
    'band3.dyn4.thresholdDb'?: number;
    'band3.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn5.attackMs'?: number;
    'band3.dyn5.detectorDelayMs'?: number;
    'band3.dyn5.enabled'?: boolean;
    'band3.dyn5.frequencyHz'?: number;
    'band3.dyn5.lookaheadMs'?: number;
    'band3.dyn5.q'?: number;
    'band3.dyn5.rangeDb'?: number;
    'band3.dyn5.ratio'?: number;
    'band3.dyn5.releaseMs'?: number;
    'band3.dyn5.sidechainFreqHz'?: number;
    'band3.dyn5.sidechainQ'?: number;
    'band3.dyn5.staticGainDb'?: number;
    'band3.dyn5.thresholdDb'?: number;
    'band3.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn6.attackMs'?: number;
    'band3.dyn6.detectorDelayMs'?: number;
    'band3.dyn6.enabled'?: boolean;
    'band3.dyn6.frequencyHz'?: number;
    'band3.dyn6.lookaheadMs'?: number;
    'band3.dyn6.q'?: number;
    'band3.dyn6.rangeDb'?: number;
    'band3.dyn6.ratio'?: number;
    'band3.dyn6.releaseMs'?: number;
    'band3.dyn6.sidechainFreqHz'?: number;
    'band3.dyn6.sidechainQ'?: number;
    'band3.dyn6.staticGainDb'?: number;
    'band3.dyn6.thresholdDb'?: number;
    'band3.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band3.dyn7.attackMs'?: number;
    'band3.dyn7.detectorDelayMs'?: number;
    'band3.dyn7.enabled'?: boolean;
    'band3.dyn7.frequencyHz'?: number;
    'band3.dyn7.lookaheadMs'?: number;
    'band3.dyn7.q'?: number;
    'band3.dyn7.rangeDb'?: number;
    'band3.dyn7.ratio'?: number;
    'band3.dyn7.releaseMs'?: number;
    'band3.dyn7.sidechainFreqHz'?: number;
    'band3.dyn7.sidechainQ'?: number;
    'band3.dyn7.staticGainDb'?: number;
    'band3.dyn7.thresholdDb'?: number;
    'band3.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn0.attackMs'?: number;
    'band4.dyn0.detectorDelayMs'?: number;
    'band4.dyn0.enabled'?: boolean;
    'band4.dyn0.frequencyHz'?: number;
    'band4.dyn0.lookaheadMs'?: number;
    'band4.dyn0.q'?: number;
    'band4.dyn0.rangeDb'?: number;
    'band4.dyn0.ratio'?: number;
    'band4.dyn0.releaseMs'?: number;
    'band4.dyn0.sidechainFreqHz'?: number;
    'band4.dyn0.sidechainQ'?: number;
    'band4.dyn0.staticGainDb'?: number;
    'band4.dyn0.thresholdDb'?: number;
    'band4.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn1.attackMs'?: number;
    'band4.dyn1.detectorDelayMs'?: number;
    'band4.dyn1.enabled'?: boolean;
    'band4.dyn1.frequencyHz'?: number;
    'band4.dyn1.lookaheadMs'?: number;
    'band4.dyn1.q'?: number;
    'band4.dyn1.rangeDb'?: number;
    'band4.dyn1.ratio'?: number;
    'band4.dyn1.releaseMs'?: number;
    'band4.dyn1.sidechainFreqHz'?: number;
    'band4.dyn1.sidechainQ'?: number;
    'band4.dyn1.staticGainDb'?: number;
    'band4.dyn1.thresholdDb'?: number;
    'band4.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn2.attackMs'?: number;
    'band4.dyn2.detectorDelayMs'?: number;
    'band4.dyn2.enabled'?: boolean;
    'band4.dyn2.frequencyHz'?: number;
    'band4.dyn2.lookaheadMs'?: number;
    'band4.dyn2.q'?: number;
    'band4.dyn2.rangeDb'?: number;
    'band4.dyn2.ratio'?: number;
    'band4.dyn2.releaseMs'?: number;
    'band4.dyn2.sidechainFreqHz'?: number;
    'band4.dyn2.sidechainQ'?: number;
    'band4.dyn2.staticGainDb'?: number;
    'band4.dyn2.thresholdDb'?: number;
    'band4.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn3.attackMs'?: number;
    'band4.dyn3.detectorDelayMs'?: number;
    'band4.dyn3.enabled'?: boolean;
    'band4.dyn3.frequencyHz'?: number;
    'band4.dyn3.lookaheadMs'?: number;
    'band4.dyn3.q'?: number;
    'band4.dyn3.rangeDb'?: number;
    'band4.dyn3.ratio'?: number;
    'band4.dyn3.releaseMs'?: number;
    'band4.dyn3.sidechainFreqHz'?: number;
    'band4.dyn3.sidechainQ'?: number;
    'band4.dyn3.staticGainDb'?: number;
    'band4.dyn3.thresholdDb'?: number;
    'band4.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn4.attackMs'?: number;
    'band4.dyn4.detectorDelayMs'?: number;
    'band4.dyn4.enabled'?: boolean;
    'band4.dyn4.frequencyHz'?: number;
    'band4.dyn4.lookaheadMs'?: number;
    'band4.dyn4.q'?: number;
    'band4.dyn4.rangeDb'?: number;
    'band4.dyn4.ratio'?: number;
    'band4.dyn4.releaseMs'?: number;
    'band4.dyn4.sidechainFreqHz'?: number;
    'band4.dyn4.sidechainQ'?: number;
    'band4.dyn4.staticGainDb'?: number;
    'band4.dyn4.thresholdDb'?: number;
    'band4.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn5.attackMs'?: number;
    'band4.dyn5.detectorDelayMs'?: number;
    'band4.dyn5.enabled'?: boolean;
    'band4.dyn5.frequencyHz'?: number;
    'band4.dyn5.lookaheadMs'?: number;
    'band4.dyn5.q'?: number;
    'band4.dyn5.rangeDb'?: number;
    'band4.dyn5.ratio'?: number;
    'band4.dyn5.releaseMs'?: number;
    'band4.dyn5.sidechainFreqHz'?: number;
    'band4.dyn5.sidechainQ'?: number;
    'band4.dyn5.staticGainDb'?: number;
    'band4.dyn5.thresholdDb'?: number;
    'band4.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn6.attackMs'?: number;
    'band4.dyn6.detectorDelayMs'?: number;
    'band4.dyn6.enabled'?: boolean;
    'band4.dyn6.frequencyHz'?: number;
    'band4.dyn6.lookaheadMs'?: number;
    'band4.dyn6.q'?: number;
    'band4.dyn6.rangeDb'?: number;
    'band4.dyn6.ratio'?: number;
    'band4.dyn6.releaseMs'?: number;
    'band4.dyn6.sidechainFreqHz'?: number;
    'band4.dyn6.sidechainQ'?: number;
    'band4.dyn6.staticGainDb'?: number;
    'band4.dyn6.thresholdDb'?: number;
    'band4.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band4.dyn7.attackMs'?: number;
    'band4.dyn7.detectorDelayMs'?: number;
    'band4.dyn7.enabled'?: boolean;
    'band4.dyn7.frequencyHz'?: number;
    'band4.dyn7.lookaheadMs'?: number;
    'band4.dyn7.q'?: number;
    'band4.dyn7.rangeDb'?: number;
    'band4.dyn7.ratio'?: number;
    'band4.dyn7.releaseMs'?: number;
    'band4.dyn7.sidechainFreqHz'?: number;
    'band4.dyn7.sidechainQ'?: number;
    'band4.dyn7.staticGainDb'?: number;
    'band4.dyn7.thresholdDb'?: number;
    'band4.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn0.attackMs'?: number;
    'band5.dyn0.detectorDelayMs'?: number;
    'band5.dyn0.enabled'?: boolean;
    'band5.dyn0.frequencyHz'?: number;
    'band5.dyn0.lookaheadMs'?: number;
    'band5.dyn0.q'?: number;
    'band5.dyn0.rangeDb'?: number;
    'band5.dyn0.ratio'?: number;
    'band5.dyn0.releaseMs'?: number;
    'band5.dyn0.sidechainFreqHz'?: number;
    'band5.dyn0.sidechainQ'?: number;
    'band5.dyn0.staticGainDb'?: number;
    'band5.dyn0.thresholdDb'?: number;
    'band5.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn1.attackMs'?: number;
    'band5.dyn1.detectorDelayMs'?: number;
    'band5.dyn1.enabled'?: boolean;
    'band5.dyn1.frequencyHz'?: number;
    'band5.dyn1.lookaheadMs'?: number;
    'band5.dyn1.q'?: number;
    'band5.dyn1.rangeDb'?: number;
    'band5.dyn1.ratio'?: number;
    'band5.dyn1.releaseMs'?: number;
    'band5.dyn1.sidechainFreqHz'?: number;
    'band5.dyn1.sidechainQ'?: number;
    'band5.dyn1.staticGainDb'?: number;
    'band5.dyn1.thresholdDb'?: number;
    'band5.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn2.attackMs'?: number;
    'band5.dyn2.detectorDelayMs'?: number;
    'band5.dyn2.enabled'?: boolean;
    'band5.dyn2.frequencyHz'?: number;
    'band5.dyn2.lookaheadMs'?: number;
    'band5.dyn2.q'?: number;
    'band5.dyn2.rangeDb'?: number;
    'band5.dyn2.ratio'?: number;
    'band5.dyn2.releaseMs'?: number;
    'band5.dyn2.sidechainFreqHz'?: number;
    'band5.dyn2.sidechainQ'?: number;
    'band5.dyn2.staticGainDb'?: number;
    'band5.dyn2.thresholdDb'?: number;
    'band5.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn3.attackMs'?: number;
    'band5.dyn3.detectorDelayMs'?: number;
    'band5.dyn3.enabled'?: boolean;
    'band5.dyn3.frequencyHz'?: number;
    'band5.dyn3.lookaheadMs'?: number;
    'band5.dyn3.q'?: number;
    'band5.dyn3.rangeDb'?: number;
    'band5.dyn3.ratio'?: number;
    'band5.dyn3.releaseMs'?: number;
    'band5.dyn3.sidechainFreqHz'?: number;
    'band5.dyn3.sidechainQ'?: number;
    'band5.dyn3.staticGainDb'?: number;
    'band5.dyn3.thresholdDb'?: number;
    'band5.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn4.attackMs'?: number;
    'band5.dyn4.detectorDelayMs'?: number;
    'band5.dyn4.enabled'?: boolean;
    'band5.dyn4.frequencyHz'?: number;
    'band5.dyn4.lookaheadMs'?: number;
    'band5.dyn4.q'?: number;
    'band5.dyn4.rangeDb'?: number;
    'band5.dyn4.ratio'?: number;
    'band5.dyn4.releaseMs'?: number;
    'band5.dyn4.sidechainFreqHz'?: number;
    'band5.dyn4.sidechainQ'?: number;
    'band5.dyn4.staticGainDb'?: number;
    'band5.dyn4.thresholdDb'?: number;
    'band5.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn5.attackMs'?: number;
    'band5.dyn5.detectorDelayMs'?: number;
    'band5.dyn5.enabled'?: boolean;
    'band5.dyn5.frequencyHz'?: number;
    'band5.dyn5.lookaheadMs'?: number;
    'band5.dyn5.q'?: number;
    'band5.dyn5.rangeDb'?: number;
    'band5.dyn5.ratio'?: number;
    'band5.dyn5.releaseMs'?: number;
    'band5.dyn5.sidechainFreqHz'?: number;
    'band5.dyn5.sidechainQ'?: number;
    'band5.dyn5.staticGainDb'?: number;
    'band5.dyn5.thresholdDb'?: number;
    'band5.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn6.attackMs'?: number;
    'band5.dyn6.detectorDelayMs'?: number;
    'band5.dyn6.enabled'?: boolean;
    'band5.dyn6.frequencyHz'?: number;
    'band5.dyn6.lookaheadMs'?: number;
    'band5.dyn6.q'?: number;
    'band5.dyn6.rangeDb'?: number;
    'band5.dyn6.ratio'?: number;
    'band5.dyn6.releaseMs'?: number;
    'band5.dyn6.sidechainFreqHz'?: number;
    'band5.dyn6.sidechainQ'?: number;
    'band5.dyn6.staticGainDb'?: number;
    'band5.dyn6.thresholdDb'?: number;
    'band5.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band5.dyn7.attackMs'?: number;
    'band5.dyn7.detectorDelayMs'?: number;
    'band5.dyn7.enabled'?: boolean;
    'band5.dyn7.frequencyHz'?: number;
    'band5.dyn7.lookaheadMs'?: number;
    'band5.dyn7.q'?: number;
    'band5.dyn7.rangeDb'?: number;
    'band5.dyn7.ratio'?: number;
    'band5.dyn7.releaseMs'?: number;
    'band5.dyn7.sidechainFreqHz'?: number;
    'band5.dyn7.sidechainQ'?: number;
    'band5.dyn7.staticGainDb'?: number;
    'band5.dyn7.thresholdDb'?: number;
    'band5.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn0.attackMs'?: number;
    'band6.dyn0.detectorDelayMs'?: number;
    'band6.dyn0.enabled'?: boolean;
    'band6.dyn0.frequencyHz'?: number;
    'band6.dyn0.lookaheadMs'?: number;
    'band6.dyn0.q'?: number;
    'band6.dyn0.rangeDb'?: number;
    'band6.dyn0.ratio'?: number;
    'band6.dyn0.releaseMs'?: number;
    'band6.dyn0.sidechainFreqHz'?: number;
    'band6.dyn0.sidechainQ'?: number;
    'band6.dyn0.staticGainDb'?: number;
    'band6.dyn0.thresholdDb'?: number;
    'band6.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn1.attackMs'?: number;
    'band6.dyn1.detectorDelayMs'?: number;
    'band6.dyn1.enabled'?: boolean;
    'band6.dyn1.frequencyHz'?: number;
    'band6.dyn1.lookaheadMs'?: number;
    'band6.dyn1.q'?: number;
    'band6.dyn1.rangeDb'?: number;
    'band6.dyn1.ratio'?: number;
    'band6.dyn1.releaseMs'?: number;
    'band6.dyn1.sidechainFreqHz'?: number;
    'band6.dyn1.sidechainQ'?: number;
    'band6.dyn1.staticGainDb'?: number;
    'band6.dyn1.thresholdDb'?: number;
    'band6.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn2.attackMs'?: number;
    'band6.dyn2.detectorDelayMs'?: number;
    'band6.dyn2.enabled'?: boolean;
    'band6.dyn2.frequencyHz'?: number;
    'band6.dyn2.lookaheadMs'?: number;
    'band6.dyn2.q'?: number;
    'band6.dyn2.rangeDb'?: number;
    'band6.dyn2.ratio'?: number;
    'band6.dyn2.releaseMs'?: number;
    'band6.dyn2.sidechainFreqHz'?: number;
    'band6.dyn2.sidechainQ'?: number;
    'band6.dyn2.staticGainDb'?: number;
    'band6.dyn2.thresholdDb'?: number;
    'band6.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn3.attackMs'?: number;
    'band6.dyn3.detectorDelayMs'?: number;
    'band6.dyn3.enabled'?: boolean;
    'band6.dyn3.frequencyHz'?: number;
    'band6.dyn3.lookaheadMs'?: number;
    'band6.dyn3.q'?: number;
    'band6.dyn3.rangeDb'?: number;
    'band6.dyn3.ratio'?: number;
    'band6.dyn3.releaseMs'?: number;
    'band6.dyn3.sidechainFreqHz'?: number;
    'band6.dyn3.sidechainQ'?: number;
    'band6.dyn3.staticGainDb'?: number;
    'band6.dyn3.thresholdDb'?: number;
    'band6.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn4.attackMs'?: number;
    'band6.dyn4.detectorDelayMs'?: number;
    'band6.dyn4.enabled'?: boolean;
    'band6.dyn4.frequencyHz'?: number;
    'band6.dyn4.lookaheadMs'?: number;
    'band6.dyn4.q'?: number;
    'band6.dyn4.rangeDb'?: number;
    'band6.dyn4.ratio'?: number;
    'band6.dyn4.releaseMs'?: number;
    'band6.dyn4.sidechainFreqHz'?: number;
    'band6.dyn4.sidechainQ'?: number;
    'band6.dyn4.staticGainDb'?: number;
    'band6.dyn4.thresholdDb'?: number;
    'band6.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn5.attackMs'?: number;
    'band6.dyn5.detectorDelayMs'?: number;
    'band6.dyn5.enabled'?: boolean;
    'band6.dyn5.frequencyHz'?: number;
    'band6.dyn5.lookaheadMs'?: number;
    'band6.dyn5.q'?: number;
    'band6.dyn5.rangeDb'?: number;
    'band6.dyn5.ratio'?: number;
    'band6.dyn5.releaseMs'?: number;
    'band6.dyn5.sidechainFreqHz'?: number;
    'band6.dyn5.sidechainQ'?: number;
    'band6.dyn5.staticGainDb'?: number;
    'band6.dyn5.thresholdDb'?: number;
    'band6.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn6.attackMs'?: number;
    'band6.dyn6.detectorDelayMs'?: number;
    'band6.dyn6.enabled'?: boolean;
    'band6.dyn6.frequencyHz'?: number;
    'band6.dyn6.lookaheadMs'?: number;
    'band6.dyn6.q'?: number;
    'band6.dyn6.rangeDb'?: number;
    'band6.dyn6.ratio'?: number;
    'band6.dyn6.releaseMs'?: number;
    'band6.dyn6.sidechainFreqHz'?: number;
    'band6.dyn6.sidechainQ'?: number;
    'band6.dyn6.staticGainDb'?: number;
    'band6.dyn6.thresholdDb'?: number;
    'band6.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band6.dyn7.attackMs'?: number;
    'band6.dyn7.detectorDelayMs'?: number;
    'band6.dyn7.enabled'?: boolean;
    'band6.dyn7.frequencyHz'?: number;
    'band6.dyn7.lookaheadMs'?: number;
    'band6.dyn7.q'?: number;
    'band6.dyn7.rangeDb'?: number;
    'band6.dyn7.ratio'?: number;
    'band6.dyn7.releaseMs'?: number;
    'band6.dyn7.sidechainFreqHz'?: number;
    'band6.dyn7.sidechainQ'?: number;
    'band6.dyn7.staticGainDb'?: number;
    'band6.dyn7.thresholdDb'?: number;
    'band6.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn0.attackMs'?: number;
    'band7.dyn0.detectorDelayMs'?: number;
    'band7.dyn0.enabled'?: boolean;
    'band7.dyn0.frequencyHz'?: number;
    'band7.dyn0.lookaheadMs'?: number;
    'band7.dyn0.q'?: number;
    'band7.dyn0.rangeDb'?: number;
    'band7.dyn0.ratio'?: number;
    'band7.dyn0.releaseMs'?: number;
    'band7.dyn0.sidechainFreqHz'?: number;
    'band7.dyn0.sidechainQ'?: number;
    'band7.dyn0.staticGainDb'?: number;
    'band7.dyn0.thresholdDb'?: number;
    'band7.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn1.attackMs'?: number;
    'band7.dyn1.detectorDelayMs'?: number;
    'band7.dyn1.enabled'?: boolean;
    'band7.dyn1.frequencyHz'?: number;
    'band7.dyn1.lookaheadMs'?: number;
    'band7.dyn1.q'?: number;
    'band7.dyn1.rangeDb'?: number;
    'band7.dyn1.ratio'?: number;
    'band7.dyn1.releaseMs'?: number;
    'band7.dyn1.sidechainFreqHz'?: number;
    'band7.dyn1.sidechainQ'?: number;
    'band7.dyn1.staticGainDb'?: number;
    'band7.dyn1.thresholdDb'?: number;
    'band7.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn2.attackMs'?: number;
    'band7.dyn2.detectorDelayMs'?: number;
    'band7.dyn2.enabled'?: boolean;
    'band7.dyn2.frequencyHz'?: number;
    'band7.dyn2.lookaheadMs'?: number;
    'band7.dyn2.q'?: number;
    'band7.dyn2.rangeDb'?: number;
    'band7.dyn2.ratio'?: number;
    'band7.dyn2.releaseMs'?: number;
    'band7.dyn2.sidechainFreqHz'?: number;
    'band7.dyn2.sidechainQ'?: number;
    'band7.dyn2.staticGainDb'?: number;
    'band7.dyn2.thresholdDb'?: number;
    'band7.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn3.attackMs'?: number;
    'band7.dyn3.detectorDelayMs'?: number;
    'band7.dyn3.enabled'?: boolean;
    'band7.dyn3.frequencyHz'?: number;
    'band7.dyn3.lookaheadMs'?: number;
    'band7.dyn3.q'?: number;
    'band7.dyn3.rangeDb'?: number;
    'band7.dyn3.ratio'?: number;
    'band7.dyn3.releaseMs'?: number;
    'band7.dyn3.sidechainFreqHz'?: number;
    'band7.dyn3.sidechainQ'?: number;
    'band7.dyn3.staticGainDb'?: number;
    'band7.dyn3.thresholdDb'?: number;
    'band7.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn4.attackMs'?: number;
    'band7.dyn4.detectorDelayMs'?: number;
    'band7.dyn4.enabled'?: boolean;
    'band7.dyn4.frequencyHz'?: number;
    'band7.dyn4.lookaheadMs'?: number;
    'band7.dyn4.q'?: number;
    'band7.dyn4.rangeDb'?: number;
    'band7.dyn4.ratio'?: number;
    'band7.dyn4.releaseMs'?: number;
    'band7.dyn4.sidechainFreqHz'?: number;
    'band7.dyn4.sidechainQ'?: number;
    'band7.dyn4.staticGainDb'?: number;
    'band7.dyn4.thresholdDb'?: number;
    'band7.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn5.attackMs'?: number;
    'band7.dyn5.detectorDelayMs'?: number;
    'band7.dyn5.enabled'?: boolean;
    'band7.dyn5.frequencyHz'?: number;
    'band7.dyn5.lookaheadMs'?: number;
    'band7.dyn5.q'?: number;
    'band7.dyn5.rangeDb'?: number;
    'band7.dyn5.ratio'?: number;
    'band7.dyn5.releaseMs'?: number;
    'band7.dyn5.sidechainFreqHz'?: number;
    'band7.dyn5.sidechainQ'?: number;
    'band7.dyn5.staticGainDb'?: number;
    'band7.dyn5.thresholdDb'?: number;
    'band7.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn6.attackMs'?: number;
    'band7.dyn6.detectorDelayMs'?: number;
    'band7.dyn6.enabled'?: boolean;
    'band7.dyn6.frequencyHz'?: number;
    'band7.dyn6.lookaheadMs'?: number;
    'band7.dyn6.q'?: number;
    'band7.dyn6.rangeDb'?: number;
    'band7.dyn6.ratio'?: number;
    'band7.dyn6.releaseMs'?: number;
    'band7.dyn6.sidechainFreqHz'?: number;
    'band7.dyn6.sidechainQ'?: number;
    'band7.dyn6.staticGainDb'?: number;
    'band7.dyn6.thresholdDb'?: number;
    'band7.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band7.dyn7.attackMs'?: number;
    'band7.dyn7.detectorDelayMs'?: number;
    'band7.dyn7.enabled'?: boolean;
    'band7.dyn7.frequencyHz'?: number;
    'band7.dyn7.lookaheadMs'?: number;
    'band7.dyn7.q'?: number;
    'band7.dyn7.rangeDb'?: number;
    'band7.dyn7.ratio'?: number;
    'band7.dyn7.releaseMs'?: number;
    'band7.dyn7.sidechainFreqHz'?: number;
    'band7.dyn7.sidechainQ'?: number;
    'band7.dyn7.staticGainDb'?: number;
    'band7.dyn7.thresholdDb'?: number;
    'band7.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn0.attackMs'?: number;
    'band8.dyn0.detectorDelayMs'?: number;
    'band8.dyn0.enabled'?: boolean;
    'band8.dyn0.frequencyHz'?: number;
    'band8.dyn0.lookaheadMs'?: number;
    'band8.dyn0.q'?: number;
    'band8.dyn0.rangeDb'?: number;
    'band8.dyn0.ratio'?: number;
    'band8.dyn0.releaseMs'?: number;
    'band8.dyn0.sidechainFreqHz'?: number;
    'band8.dyn0.sidechainQ'?: number;
    'band8.dyn0.staticGainDb'?: number;
    'band8.dyn0.thresholdDb'?: number;
    'band8.dyn0.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn1.attackMs'?: number;
    'band8.dyn1.detectorDelayMs'?: number;
    'band8.dyn1.enabled'?: boolean;
    'band8.dyn1.frequencyHz'?: number;
    'band8.dyn1.lookaheadMs'?: number;
    'band8.dyn1.q'?: number;
    'band8.dyn1.rangeDb'?: number;
    'band8.dyn1.ratio'?: number;
    'band8.dyn1.releaseMs'?: number;
    'band8.dyn1.sidechainFreqHz'?: number;
    'band8.dyn1.sidechainQ'?: number;
    'band8.dyn1.staticGainDb'?: number;
    'band8.dyn1.thresholdDb'?: number;
    'band8.dyn1.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn2.attackMs'?: number;
    'band8.dyn2.detectorDelayMs'?: number;
    'band8.dyn2.enabled'?: boolean;
    'band8.dyn2.frequencyHz'?: number;
    'band8.dyn2.lookaheadMs'?: number;
    'band8.dyn2.q'?: number;
    'band8.dyn2.rangeDb'?: number;
    'band8.dyn2.ratio'?: number;
    'band8.dyn2.releaseMs'?: number;
    'band8.dyn2.sidechainFreqHz'?: number;
    'band8.dyn2.sidechainQ'?: number;
    'band8.dyn2.staticGainDb'?: number;
    'band8.dyn2.thresholdDb'?: number;
    'band8.dyn2.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn3.attackMs'?: number;
    'band8.dyn3.detectorDelayMs'?: number;
    'band8.dyn3.enabled'?: boolean;
    'band8.dyn3.frequencyHz'?: number;
    'band8.dyn3.lookaheadMs'?: number;
    'band8.dyn3.q'?: number;
    'band8.dyn3.rangeDb'?: number;
    'band8.dyn3.ratio'?: number;
    'band8.dyn3.releaseMs'?: number;
    'band8.dyn3.sidechainFreqHz'?: number;
    'band8.dyn3.sidechainQ'?: number;
    'band8.dyn3.staticGainDb'?: number;
    'band8.dyn3.thresholdDb'?: number;
    'band8.dyn3.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn4.attackMs'?: number;
    'band8.dyn4.detectorDelayMs'?: number;
    'band8.dyn4.enabled'?: boolean;
    'band8.dyn4.frequencyHz'?: number;
    'band8.dyn4.lookaheadMs'?: number;
    'band8.dyn4.q'?: number;
    'band8.dyn4.rangeDb'?: number;
    'band8.dyn4.ratio'?: number;
    'band8.dyn4.releaseMs'?: number;
    'band8.dyn4.sidechainFreqHz'?: number;
    'band8.dyn4.sidechainQ'?: number;
    'band8.dyn4.staticGainDb'?: number;
    'band8.dyn4.thresholdDb'?: number;
    'band8.dyn4.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn5.attackMs'?: number;
    'band8.dyn5.detectorDelayMs'?: number;
    'band8.dyn5.enabled'?: boolean;
    'band8.dyn5.frequencyHz'?: number;
    'band8.dyn5.lookaheadMs'?: number;
    'band8.dyn5.q'?: number;
    'band8.dyn5.rangeDb'?: number;
    'band8.dyn5.ratio'?: number;
    'band8.dyn5.releaseMs'?: number;
    'band8.dyn5.sidechainFreqHz'?: number;
    'band8.dyn5.sidechainQ'?: number;
    'band8.dyn5.staticGainDb'?: number;
    'band8.dyn5.thresholdDb'?: number;
    'band8.dyn5.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn6.attackMs'?: number;
    'band8.dyn6.detectorDelayMs'?: number;
    'band8.dyn6.enabled'?: boolean;
    'band8.dyn6.frequencyHz'?: number;
    'band8.dyn6.lookaheadMs'?: number;
    'band8.dyn6.q'?: number;
    'band8.dyn6.rangeDb'?: number;
    'band8.dyn6.ratio'?: number;
    'band8.dyn6.releaseMs'?: number;
    'band8.dyn6.sidechainFreqHz'?: number;
    'band8.dyn6.sidechainQ'?: number;
    'band8.dyn6.staticGainDb'?: number;
    'band8.dyn6.thresholdDb'?: number;
    'band8.dyn6.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    'band8.dyn7.attackMs'?: number;
    'band8.dyn7.detectorDelayMs'?: number;
    'band8.dyn7.enabled'?: boolean;
    'band8.dyn7.frequencyHz'?: number;
    'band8.dyn7.lookaheadMs'?: number;
    'band8.dyn7.q'?: number;
    'band8.dyn7.rangeDb'?: number;
    'band8.dyn7.ratio'?: number;
    'band8.dyn7.releaseMs'?: number;
    'band8.dyn7.sidechainFreqHz'?: number;
    'band8.dyn7.sidechainQ'?: number;
    'band8.dyn7.staticGainDb'?: number;
    'band8.dyn7.thresholdDb'?: number;
    'band8.dyn7.type'?:
      | 'peak'
      | 'lowShelf'
      | 'highShelf'
      | 'lowPass'
      | 'highPass'
      | 'bandPass'
      | 'notch'
      | 'allPass';
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'multiband.expander': {
    'band0.thresholdDb'?: number;
    'band0.ratio'?: number;
    'band0.attackMs'?: number;
    'band0.releaseMs'?: number;
    'band0.rangeDb'?: number;
    'band1.thresholdDb'?: number;
    'band1.ratio'?: number;
    'band1.attackMs'?: number;
    'band1.releaseMs'?: number;
    'band1.rangeDb'?: number;
    'band2.thresholdDb'?: number;
    'band2.ratio'?: number;
    'band2.attackMs'?: number;
    'band2.releaseMs'?: number;
    'band2.rangeDb'?: number;
    'band3.attackMs'?: number;
    'band3.rangeDb'?: number;
    'band3.ratio'?: number;
    'band3.releaseMs'?: number;
    'band3.thresholdDb'?: number;
    'band4.attackMs'?: number;
    'band4.rangeDb'?: number;
    'band4.ratio'?: number;
    'band4.releaseMs'?: number;
    'band4.thresholdDb'?: number;
    'band5.attackMs'?: number;
    'band5.rangeDb'?: number;
    'band5.ratio'?: number;
    'band5.releaseMs'?: number;
    'band5.thresholdDb'?: number;
    'band6.attackMs'?: number;
    'band6.rangeDb'?: number;
    'band6.ratio'?: number;
    'band6.releaseMs'?: number;
    'band6.thresholdDb'?: number;
    'band7.attackMs'?: number;
    'band7.rangeDb'?: number;
    'band7.ratio'?: number;
    'band7.releaseMs'?: number;
    'band7.thresholdDb'?: number;
    'band8.attackMs'?: number;
    'band8.rangeDb'?: number;
    'band8.ratio'?: number;
    'band8.releaseMs'?: number;
    'band8.thresholdDb'?: number;
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'multiband.imager': {
    'band0.width'?: number;
    'band0.decorrelationAmount'?: number;
    'band1.width'?: number;
    'band1.decorrelationAmount'?: number;
    'band2.width'?: number;
    'band2.decorrelationAmount'?: number;
    'band0.enabled'?: boolean;
    'band0.preserveEnergy'?: boolean;
    'band1.enabled'?: boolean;
    'band1.preserveEnergy'?: boolean;
    'band2.enabled'?: boolean;
    'band2.preserveEnergy'?: boolean;
    'band3.decorrelationAmount'?: number;
    'band3.enabled'?: boolean;
    'band3.preserveEnergy'?: boolean;
    'band3.width'?: number;
    'band4.decorrelationAmount'?: number;
    'band4.enabled'?: boolean;
    'band4.preserveEnergy'?: boolean;
    'band4.width'?: number;
    'band5.decorrelationAmount'?: number;
    'band5.enabled'?: boolean;
    'band5.preserveEnergy'?: boolean;
    'band5.width'?: number;
    'band6.decorrelationAmount'?: number;
    'band6.enabled'?: boolean;
    'band6.preserveEnergy'?: boolean;
    'band6.width'?: number;
    'band7.decorrelationAmount'?: number;
    'band7.enabled'?: boolean;
    'band7.preserveEnergy'?: boolean;
    'band7.width'?: number;
    'band8.decorrelationAmount'?: number;
    'band8.enabled'?: boolean;
    'band8.preserveEnergy'?: boolean;
    'band8.width'?: number;
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'multiband.limiter': {
    'band0.thresholdDb'?: number;
    'band0.releaseMs'?: number;
    'band1.thresholdDb'?: number;
    'band1.releaseMs'?: number;
    'band2.thresholdDb'?: number;
    'band2.releaseMs'?: number;
    'band0.lookaheadMs'?: number;
    'band1.lookaheadMs'?: number;
    'band2.lookaheadMs'?: number;
    'band3.lookaheadMs'?: number;
    'band3.releaseMs'?: number;
    'band3.thresholdDb'?: number;
    'band4.lookaheadMs'?: number;
    'band4.releaseMs'?: number;
    'band4.thresholdDb'?: number;
    'band5.lookaheadMs'?: number;
    'band5.releaseMs'?: number;
    'band5.thresholdDb'?: number;
    'band6.lookaheadMs'?: number;
    'band6.releaseMs'?: number;
    'band6.thresholdDb'?: number;
    'band7.lookaheadMs'?: number;
    'band7.releaseMs'?: number;
    'band7.thresholdDb'?: number;
    'band8.lookaheadMs'?: number;
    'band8.releaseMs'?: number;
    'band8.thresholdDb'?: number;
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'multiband.saturation': {
    'band0.driveDb'?: number;
    'band0.mix'?: number;
    'band0.outputGainDb'?: number;
    'band1.driveDb'?: number;
    'band1.mix'?: number;
    'band1.outputGainDb'?: number;
    'band2.driveDb'?: number;
    'band2.mix'?: number;
    'band2.outputGainDb'?: number;
    'band0.enabled'?: boolean;
    'band0.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band1.enabled'?: boolean;
    'band1.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band2.enabled'?: boolean;
    'band2.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band3.driveDb'?: number;
    'band3.enabled'?: boolean;
    'band3.mix'?: number;
    'band3.outputGainDb'?: number;
    'band3.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band4.driveDb'?: number;
    'band4.enabled'?: boolean;
    'band4.mix'?: number;
    'band4.outputGainDb'?: number;
    'band4.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band5.driveDb'?: number;
    'band5.enabled'?: boolean;
    'band5.mix'?: number;
    'band5.outputGainDb'?: number;
    'band5.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band6.driveDb'?: number;
    'band6.enabled'?: boolean;
    'band6.mix'?: number;
    'band6.outputGainDb'?: number;
    'band6.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band7.driveDb'?: number;
    'band7.enabled'?: boolean;
    'band7.mix'?: number;
    'band7.outputGainDb'?: number;
    'band7.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    'band8.driveDb'?: number;
    'band8.enabled'?: boolean;
    'band8.mix'?: number;
    'band8.outputGainDb'?: number;
    'band8.type'?: 'softClip' | 'tape' | 'tube' | 'exciter';
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'repair.declick': {
    lpcOrder?: number;
    maxClickSamples?: number;
    neighborRatio?: number;
    residualRatio?: number;
    threshold?: number;
  };
  'repair.declip': {
    clipThreshold?: number;
    iterations?: number;
    lpcBlend?: number;
    lpcOrder?: number;
  };
  'repair.decrackle': {
    levels?: number;
    mode?: 'median';
    threshold?: number;
  };
  'repair.dehum': {
    adaptation?: number;
    adaptive?: boolean;
    frameSize?: number;
    fundamentalHz?: number;
    harmonics?: number;
    mode?: 'subtract' | 'notch';
    pllBandwidth?: number;
    q?: number;
    searchRangeHz?: number;
  };
  'repair.denoiseClassical': {
    ddAlpha?: number;
    gainSmoothing?: boolean;
    hopLength?: number;
    mode?: 'logMmse' | 'mmseStsa' | 'spectralSubtraction';
    nFft?: number;
    noiseEstimationQuantile?: number;
    noiseEstimator?: 'mcra' | 'imcra' | 'spp';
    overSubtraction?: number;
    reductionDb?: number;
    spectralFloor?: number;
    speechPresenceGain?: boolean;
  };
  'repair.dereverbClassical': {
    attenuation?: number;
    hopLength?: number;
    lateDelayMs?: number;
    nFft?: number;
    overSubtraction?: number;
    spectralFloor?: number;
    t60Sec?: number;
    threshold?: number;
    wpeEnabled?: boolean;
    wpeIterations?: number;
    wpeStrength?: number;
    wpeTaps?: number;
  };
  'repair.trimSilence': {
    gateLufs?: number;
    mode?: 'peak' | 'lufsGated';
    paddingSamples?: number;
    threshold?: number;
    windowMs?: number;
  };
  'saturation.ampSim': {
    drive?: number;
    bassDb?: number;
    midDb?: number;
    trebleDb?: number;
    presenceDb?: number;
    levelDb?: number;
    power?: number;
    sag?: number;
    transformer?: number;
    nfb?: number;
    micAxis?: number;
    micBAxis?: number;
    micBlend?: number;
    cone?: number;
    crossover?: number;
    biasShift?: number;
    inputDb?: number;
    cab?: boolean;
    cabModel?: 'guitar4x12' | 'bass8x10' | 'guitar1x12Combo' | 'guitar2x12Open';
    ampModel?:
      | 'classicCrunch'
      | 'cleanCombo'
      | 'modernHiGain'
      | 'vintageCombo'
      | 'classAChime'
      | 'heavyHiGain';
    cabIrDrivers?: boolean;
    cabIrF32Base64?: string;
    cabIrGenerate?: boolean;
    cabIrSampleRate?: number;
    doppler?: number;
    micBDistanceCm?: number;
    micBInvert?: boolean;
    micBModel?: 'none' | 'dynamic' | 'ribbon' | 'condenser';
    micDistanceCm?: number;
    micModel?: 'none' | 'dynamic' | 'ribbon' | 'condenser';
    powerTube?: '6l6' | 'el34' | 'el84' | '6v6';
    preampStages?: number;
    preset?: string;
    presetIndex?: number;
    topology?: 'voiced' | 'circuit';
  };
  'saturation.bitcrusher': {
    bitDepth?: number;
    mix?: number;
    holdHz?: number;
    radioNoiseLevel?: number;
    wpNoiseLevel?: number;
    discNoiseLevel?: number;
    humLevel?: number;
    noiseDetune?: number;
    noiseLpfHz?: number;
    wpNoiseLpfHz?: number;
    discNoiseLpfHz?: number;
    humLpfHz?: number;
    preFilterHz?: number;
    postFilterHz?: number;
    humHz?: number;
    wpNoisePink?: boolean;
    discType?: 'lp' | 'ep' | 'sp' | 'rnd';
    filterType?: 'off' | 'lowpass' | 'highpass';
    mono?: boolean;
    typeLadder?: number;
    mixLaw?: 'crossfade' | 'twoRamps';
    ditherSeed?: number;
    ditherType?: 'none' | 'rpdf' | 'tpdf' | 'noiseShaped';
    downsampleFactor?: number;
    quantizerMode?: 'fixedDepth' | 'off';
  };
  'saturation.distortion': {
    gainDb?: number;
    toneHz?: number;
    levelDb?: number;
  };
  'saturation.exciter': {
    frequencyHz?: number;
    driveDb?: number;
    amount?: number;
    q?: number;
    evenOddMix?: number;
    aliasing?: 'none' | 'oversample4x';
  };
  'saturation.hardClipper': {
    ceiling?: number;
    aliasing?: 'none' | 'adaa1' | 'adaa2' | 'oversample4x';
  };
  'saturation.multibandExciter': {
    'band0.frequencyHz'?: number;
    'band0.driveDb'?: number;
    'band0.amount'?: number;
    'band0.q'?: number;
    'band0.evenOddMix'?: number;
    'band1.frequencyHz'?: number;
    'band1.driveDb'?: number;
    'band1.amount'?: number;
    'band1.q'?: number;
    'band1.evenOddMix'?: number;
    'band2.frequencyHz'?: number;
    'band2.driveDb'?: number;
    'band2.amount'?: number;
    'band2.q'?: number;
    'band2.evenOddMix'?: number;
    'band3.amount'?: number;
    'band3.driveDb'?: number;
    'band3.evenOddMix'?: number;
    'band3.frequencyHz'?: number;
    'band3.q'?: number;
    'band4.amount'?: number;
    'band4.driveDb'?: number;
    'band4.evenOddMix'?: number;
    'band4.frequencyHz'?: number;
    'band4.q'?: number;
    'band5.amount'?: number;
    'band5.driveDb'?: number;
    'band5.evenOddMix'?: number;
    'band5.frequencyHz'?: number;
    'band5.q'?: number;
    'band6.amount'?: number;
    'band6.driveDb'?: number;
    'band6.evenOddMix'?: number;
    'band6.frequencyHz'?: number;
    'band6.q'?: number;
    'band7.amount'?: number;
    'band7.driveDb'?: number;
    'band7.evenOddMix'?: number;
    'band7.frequencyHz'?: number;
    'band7.q'?: number;
    'band8.amount'?: number;
    'band8.driveDb'?: number;
    'band8.evenOddMix'?: number;
    'band8.frequencyHz'?: number;
    'band8.q'?: number;
    cutoff0Hz?: number;
    cutoff1Hz?: number;
    cutoff2Hz?: number;
    cutoff3Hz?: number;
    cutoff4Hz?: number;
    cutoff5Hz?: number;
    cutoff6Hz?: number;
    cutoff7Hz?: number;
    firKernelSize?: number;
    mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
    slope?: 'lr2' | 'lr4' | 'lr8';
  };
  'saturation.overdrive': {
    gainDb?: number;
    toneHz?: number;
    levelDb?: number;
  };
  'saturation.softClipper': {
    driveDb?: number;
    mix?: number;
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
    ceiling?: number;
  };
  'saturation.tape': {
    driveDb?: number;
    saturation?: number;
    hysteresis?: number;
    outputGainDb?: number;
    speedIps?: number;
    headBumpDb?: number;
    bias?: number;
    gapLoss?: number;
    oversampleFactor?: number;
  };
  'saturation.transformer': {
    driveDb?: number;
    asymmetry?: number;
    mix?: number;
  };
  'saturation.tube': {
    driveDb?: number;
    bias?: number;
    mix?: number;
    biasV?: number;
    harmonicDrive?: number;
    oversampleFactor?: number;
  };
  'saturation.waveshaper': {
    driveDb?: number;
    mix?: number;
    outputGainDb?: number;
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
    bias?: number;
    curve?: 'tanh' | 'arctan' | 'asymmetric';
  };
  'spectral.airBand': {
    amount?: number;
    shelfFrequencyHz?: number;
    dynamicThresholdDb?: number;
    dynamicRangeDb?: number;
  };
  'spectral.lowEndFocus': {
    cutoffHz?: number;
    width?: number;
    subharmonicAmount?: number;
    transientTightness?: number;
  };
  'spectral.presenceEnhancer': {
    amount?: number;
    drive?: number;
    centerFrequencyHz?: number;
    q?: number;
    aliasing?: 'none' | 'adaa1' | 'oversample4x';
  };
  'spectral.spectralShaper': {
    threshold?: number;
    amount?: number;
    frequencyHz?: number;
    highFrequencyHz?: number;
    attackMs?: number;
    releaseMs?: number;
    rangeDb?: number;
  };
  'stereo.autoPan': {
    rateHz?: number;
    depth?: number;
    phase?: number;
    shape?: 'sine' | 'triangle' | 'square' | 'sawUp' | 'sawDown';
  };
  'stereo.binaural': {
    azimuthDeg?: number;
    autoTurn?: boolean;
    turnRateHz?: number;
    clockwise?: boolean;
    output?: 'speakers' | 'phones';
    dryWet?: number;
  };
  'stereo.haasEnhancer': {
    delayMs?: number;
    mix?: number;
    delayRight?: boolean;
  };
  'stereo.imager': {
    width?: number;
    outputGainDb?: number;
    decorrelationAmount?: number;
    preserveEnergy?: boolean;
  };
  'stereo.monoMaker': {
    amount?: number;
    frequencyHz?: number;
  };
  'stereo.phaseAlign': {
    fractionalDelaySamples?: number;
    delayRight?: boolean;
    delaySamples?: number;
  };
  'stereo.stereoBalance': {
    balance?: number;
    law?: 'normalized' | 'rawConstantPower';
    gain?: number;
    constantPower?: boolean;
  };
  'utility.gain': {
    levelDb?: number;
  };
  'voice.changer': {
    inputGainDb?: number;
    outputGainDb?: number;
    wetMix?: number;
    retuneSemitones?: number;
    retuneMix?: number;
    formantFactor?: number;
    formantAmount?: number;
    formantBody?: number;
    formantBrightness?: number;
    formantNasal?: number;
    eqHighpassHz?: number;
    eqBodyDb?: number;
    eqPresenceDb?: number;
    eqAirDb?: number;
    gateThresholdDb?: number;
    gateAttackMs?: number;
    gateReleaseMs?: number;
    gateRangeDb?: number;
    compressorThresholdDb?: number;
    compressorRatio?: number;
    compressorAttackMs?: number;
    compressorReleaseMs?: number;
    compressorMakeupGainDb?: number;
    deesserFrequencyHz?: number;
    deesserThresholdDb?: number;
    deesserRatio?: number;
    deesserRangeDb?: number;
    reverbMix?: number;
    reverbTimeMs?: number;
    reverbDamping?: number;
    limiterCeilingDb?: number;
    limiterReleaseMs?: number;
    limiterIspCeilingDbtp?: number;
    pitchCorrectionMode?: 'off' | 'auto' | 'robot';
    pitchCorrectionAmount?: number;
    pitchCorrectionSpeedMs?: number;
    pitchCorrectionKeyRoot?: number;
    pitchCorrectionScale?: 'major' | 'naturalMinor' | 'chromatic';
    pitchCorrectionRobotOctave?: number;
    harmonyVoices?: number;
    harmonyStep1?: number;
    harmonyStep2?: number;
    harmonyStep3?: number;
    harmonyLevel?: number;
    harmonyLeadLevel?: number;
    harmonyDetuneCents?: number;
    harmonyDelayMs?: number;
    vocoderEnabled?: boolean;
    vocoderAttackMs?: number;
    vocoderReleaseMs?: number;
    vocoderSibilance?: number;
    vocoderCarrierWave?: 'saw' | 'pulse' | 'ensemble';
    vocoderEnsembleDetuneCents?: number;
    vocoderLevel?: number;
    characterType?: 'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo';
    characterAmount?: number;
    characterTone?: number;
    reverbVariationVariation?: 'reverb' | 'echo' | 'tempoDelay' | 'dubEcho';
    reverbVariationDelayMs?: number;
    reverbVariationFeedback?: number;
    reverbVariationTempoBpm?: number;
    reverbVariationDivision?: 'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth';
    midiRelease?: 'hold' | 'input';
    formantMode?: 'relative' | 'absolute';
    limiterEnableIspLimiter?: boolean;
    retuneGrainSize?: number;
    reverbSeed?: number;
    vocoderBands?: number;
    vocoderCarrierSource?: 'internal';
    voiceEngineEngine?: 'grain' | 'psola';
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
  /** Integrated loudness in LUFS; absent for a silent or below-gate take. */
  integratedLufs?: number;
  /** EBU Tech 3342 loudness range, in LU. */
  loudnessRangeLu: number;
  /** True peak, in dBTP. */
  truePeakDbtp: number;
  /** Crest factor (peak over RMS level) in dB. */
  crestFactorDb: number;
}

/** Spectral group of {@link MasteringAudioProfile}; band levels (`*RmsDb`) are dBFS, mean square: a full-scale sine reads -3.01 dBFS in its band. */
export interface MasteringAudioProfileSpectral {
  /** Sub band level in dBFS. */
  subRmsDb: number;
  /** Low band level in dBFS. */
  lowRmsDb: number;
  /** Low-mid band level in dBFS. */
  lowMidRmsDb: number;
  /** Mid band level in dBFS. */
  midRmsDb: number;
  /** High-mid band level in dBFS. */
  highMidRmsDb: number;
  /** High band level in dBFS. */
  highRmsDb: number;
  /** Air band level in dBFS. */
  airRmsDb: number;
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
  /** Reverb reading. */
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
 * `MasteringAudioProfile`'s nested groups. A non-finite loudness is absent.
 */
export interface MasteringAssistantProfile {
  /** Duration of the input in seconds. */
  durationSec: number;
  /** Estimated tempo in beats per minute. */
  bpm: number;
  /** Confidence of the tempo estimate. */
  bpmConfidence: number;
  /** Integrated loudness in LUFS; absent for a silent or below-gate take. */
  integratedLufs?: number;
  /** EBU Tech 3342 loudness range, in LU. */
  loudnessRangeLu: number;
  /** True peak, in dBTP. */
  truePeakDbtp: number;
  /** Crest factor (peak over RMS level) in dB. */
  crestFactorDb: number;
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
  /** Integrated loudness in LUFS; absent for a silent or below-gate take. */
  integratedLufs?: number;
  /** True peak of the take, in dBTP. */
  truePeakDbtp: number;
  /** Gain that lands the take on the platform's target; 0 when the loudness is absent. */
  normalizationGainDb: number;
  /** True when that gain would push the true peak above the platform's ceiling. */
  ceilingRisk: boolean;
}

/** What {@link masteringStreamingPreview} and its stereo form return. */
export interface MasteringStreamingPreviewResult {
  /** One entry per delivery target. */
  platforms: MasteringStreamingPreviewPlatform[];
}

/** Result of the `match.referenceLoudness` pair analysis. A loudness a silent take cannot give, and the gain derived from it, is absent. */
export interface MatchReferenceLoudnessResult {
  /** Integrated loudness of the source in LUFS. */
  sourceLufs?: number;
  /** Integrated loudness of the reference in LUFS. */
  referenceLufs?: number;
  /** Gain in dB that brings the source to the reference loudness. */
  gainToMatchDb?: number;
}

/** One band of the `match.tonalBalance` and `match.tonalBalanceLogBands` analyses. */
export interface MatchTonalBalanceBand {
  /** Lower band edge in Hz. */
  lowHz: number;
  /** Upper band edge in Hz. */
  highHz: number;
  /** Source level in the band, in dB. */
  sourceDb: number;
  /** Reference level in the band, in dB. */
  referenceDb: number;
  /** Source level minus reference level in dB. */
  deviationDb: number;
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
  /** Inter-channel correlation (-1 to 1). */
  correlation: number;
  /** Stereo width; absent for a fully out-of-phase pair. */
  width?: number;
  /** Peak of the mono fold-down. */
  monoPeak: number;
  /** RMS level of the side signal. */
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
  /** Inter-channel correlation (-1 to 1). */
  correlation: number;
  /** RMS level of the side signal. */
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

/** Options of {@link masteringNormalizeLoudness}. All fields are optional. */
export interface MasteringNormalizeLoudnessOptions extends ValidateOptions {
  /** Target integrated LUFS. Default -14. */
  targetLufs?: number;
  /**
   * True/sample peak ceiling in dBFS. Default -1. Held on the true peak read at
   * {@link truePeakOversample}; a finer meter can read up to ~0.15 dB higher.
   */
  ceilingDb?: number;
  /** Oversampling factor used for peak estimation. Default 4; an explicit 0 is a `RangeError`. */
  truePeakOversample?: number;
  /**
   * Post true-peak limiter release in ms: one time constant, about 63% of the way at the set
   * time. Default 50; an explicit 0 is a `RangeError`.
   */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

/**
 * The loudness numbers a {@link StreamingMasteringChain} is constructed with,
 * measured at the loudness stage's input (after every earlier enabled stage),
 * which is where the offline chain measures.
 *
 * `loudnessStaticGainDb` equals the gain {@link masterAudio} applies, ceiling
 * clamp included, and 0 for a silent or below-gate stage input. Pass it and
 * `truePeakDbtp` as the chain config's `loudnessStaticGainDb` and
 * `loudnessStaticGainPeakDb`.
 */
export interface StreamingLoudnessGainResult {
  /** Static gain in dB the offline loudness stage applies. */
  loudnessStaticGainDb: number;
  /** True peak in dBTP of the loudness stage's input; -120 for digital silence. */
  truePeakDbtp: number;
  /** Integrated loudness in LUFS of the loudness stage's input; `-Infinity` below the absolute gate. */
  integratedLufs: number;
}

/** What gain-matching one take to another's loudness took, and produced. */
export interface LoudnessMatchResult {
  /** The source take at the reference's loudness. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
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
  /** Sample rate of the returned audio in Hz. */
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
  /** Processed mono samples. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /** Integrated loudness of the input in LUFS. */
  inputLufs: number;
  /** Integrated loudness of the output in LUFS. */
  outputLufs: number;
  /** Gain applied, in dB. */
  appliedGainDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited?: boolean;
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it. 0 when the processor has none.
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
   * with, not because nothing needed replacing. The counter saturates rather
   * than wrapping, because a wrapped total could read as the one value zero
   * is reserved for.
   *
   * @example
   * ```ts
   * const result = masteringApplyProcessor({
   *   processor: 'dynamics.limiter',
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

/**
 * One band of the multiband compressor, addressed by index in
 * `dynamics.multibandComp.bands`. Every field is optional: an omitted field
 * keeps the base (preset) value, or the compressor default for a band the base
 * does not have.
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
  /** Repair modules (denoise, declip, decrackle, dehum, declick, dereverb). */
  repair?: {
    denoise?: {
      enabled?: boolean;
      mode?: 'logMmse' | 'mmseStsa' | 'spectralSubtraction';
      /** `'spp'` is the speech-presence-probability estimator. */
      noiseEstimator?: 'quantile' | 'mcra' | 'imcra' | 'spp';
      nFft?: number;
      hopLength?: number;
      ddAlpha?: number;
      reductionDb?: number;
      overSubtraction?: number;
      spectralFloor?: number;
      noiseEstimationQuantile?: number;
      speechPresenceGain?: boolean;
      gainSmoothing?: boolean;
    };
    declip?: {
      enabled?: boolean;
      /** Clip level in dBFS, at or below 0. */
      clipThresholdDb?: number;
      lpcOrder?: number;
      iterations?: number;
      lpcBlend?: number;
    };
    decrackle?: {
      enabled?: boolean;
      threshold?: number;
      mode?: 'median' | 'waveletShrinkage';
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
      /** `'subtract'` the tracked harmonics, or `'notch'` (cascaded notches). */
      mode?: 'subtract' | 'notch';
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
  /** EQ modules. */
  eq?: {
    tilt?: {
      enabled?: boolean;
      tiltDb?: number;
      pivotHz?: number;
    };
  };
  /** Dynamics modules (compressor, deesser, transientShaper, multibandComp). */
  dynamics?: {
    compressor?: {
      enabled?: boolean;
      thresholdDb?: number;
      ratio?: number;
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      attackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
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
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      attackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
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
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      lowAttackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      lowReleaseMs?: number;
      midThresholdDb?: number;
      midRatio?: number;
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      midAttackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      midReleaseMs?: number;
      highThresholdDb?: number;
      highRatio?: number;
      /** Attack in ms: one time constant, about 63% of the way at the set time. */
      highAttackMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      highReleaseMs?: number;
      /**
       * Crossover structure. `cutoffsHz` replaces the cutoff list and sets the
       * band count to `length + 1`; do not combine it with `lowCutoffHz` /
       * `highCutoffHz` in one call. An empty list is refused.
       */
      crossover?: {
        /** Ascending split frequencies in Hz; N values give N + 1 bands. */
        cutoffsHz?: number[];
        slope?: 'lr2' | 'lr4' | 'lr8';
        mode?: 'linkwitzRiley' | 'butterworth' | 'bessel' | 'firLinearPhase';
        firKernelSize?: number;
      };
      /**
       * Per-band compressor settings by band index. An omitted band or field
       * keeps the base (preset) value, or the compressor default for a band the
       * base does not have. Do not combine with the `low*` / `mid*` / `high*`
       * shorthand for the same field in one call.
       */
      bands?: MultibandCompressorBand[];
    };
  };
  /** Saturation modules (tape, exciter). */
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
  /** Spectral modules (airBand). */
  spectral?: {
    airBand?: {
      enabled?: boolean;
      amount?: number;
      shelfFrequencyHz?: number;
      dynamicThresholdDb?: number;
      dynamicRangeDb?: number;
    };
  };
  /** Stereo modules (imager, monoMaker); applied on the stereo path only. */
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
  /** Maximizer modules (true-peak limiter). */
  maximizer?: {
    truePeakLimiter?: {
      enabled?: boolean;
      ceilingDb?: number;
      lookaheadMs?: number;
      /** Release in ms: one time constant, about 63% of the way at the set time. */
      releaseMs?: number;
      oversampleFactor?: number;
      applyGainAtInputRate?: boolean;
    };
  };
  /** Loudness stage: integrated-loudness target and true-peak ceiling. */
  loudness?: {
    enabled?: boolean;
    targetLufs?: number;
    /** Ceiling of the loudness stage; `maximizer.truePeakLimiter.ceilingDb` bounds the limiter, each applies to its own stage. */
    ceilingDb?: number;
    truePeakOversample?: number;
    /** Release in ms: one time constant, about 63% of the way at the set time. */
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
}

/**
 * Constructor input of the block-by-block {@link StreamingMasteringChain}: a
 * chain config plus the stream layout and optional precomputed loudness gain.
 *
 * The streaming chain cannot measure whole-signal integrated LUFS, so an enabled
 * `loudness` stage normally throws at construction. Precompute the gain offline
 * with {@link streamingLoudnessGain} and pass it as `loudnessStaticGainDb`.
 */
export interface StreamingMasteringChainConfig extends MasteringChainConfig {
  /** Stream sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Largest block `process` is given, in samples per channel. Omit it for the library default; an explicit 0 is refused. */
  maxBlockSize?: number;
  /** Channel count of the stream: 1 or 2. Required. */
  numChannels: 1 | 2;
  /**
   * Precomputed static loudness gain in dB. When omitted, an enabled `loudness`
   * stage still throws. When provided and `loudness.enabled` is set, the chain
   * applies this fixed gain per block before the loudness stage's true-peak
   * limiter instead of throwing.
   */
  loudnessStaticGainDb?: number;
  /**
   * True peak (dBTP) of the loudness stage's input the static gain was computed
   * for (`truePeakDbtp` of {@link streamingLoudnessGain}). When provided, the
   * static gain is clamped to `(loudness.ceilingDb - loudnessStaticGainPeakDb) +
   * max(loudness.maxLimiterGainReductionDb, 0)` so the streaming preview does
   * not drive the loudness limiter harder than the offline chain. When omitted
   * the static gain is applied verbatim.
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
  /** Integrated loudness in LUFS. */
  integratedLufs: number;
  /** Maximum momentary loudness in LUFS. */
  maxMomentaryLufs: number;
  /** Maximum short-term loudness in LUFS. */
  maxShortTermLufs: number;
  /** True peak in dBTP. */
  truePeakDbtp: number;
  /** EBU Tech 3342 loudness range, in LU. */
  loudnessRangeLu: number;
}

/** Compact explanation of how an offline mastering chain changed a program. */
export interface MasteringReport {
  /** Measurements of the input. */
  before: MasteringLoudnessSummary;
  /** Measurements of the output. */
  after: MasteringLoudnessSummary;
  /** Gain applied by the chain, in dB. */
  appliedGainDb: number;
  /** Most-negative final dynamics/limiter gain reduction in dB, or zero when none ran. */
  maxGainReductionDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /** 32 logarithmically-spaced after-minus-before spectral energy deltas (dB). */
  bandEnergyDeltaDb: Float32Array;
}

/** Result of the offline mastering chain on a mono signal. */
export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /** Integrated loudness of the input in LUFS. */
  inputLufs: number;
  /** Integrated loudness of the output in LUFS. */
  outputLufs: number;
  /** Gain applied by the loudness stage, in dB. */
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
   */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 Loudness Range of the output (LU). */
  outputLoudnessRangeLu: number;
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
   * individually through {@link masteringApplyProcessor} to attribute a non-zero count.
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
  /** Integrated loudness of the input in LUFS. */
  inputLufs: number;
  /** Integrated loudness of the output in LUFS. */
  outputLufs: number;
  /** Gain applied by the loudness stage, in dB. */
  appliedGainDb: number;
  /** Names of the stages that ran, in order. */
  stages: string[];
  /** See {@link MasteringChainResult} for field semantics. */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 loudness range of the output, in LU. */
  outputLoudnessRangeLu: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * See {@link MasteringChainResult.nonFiniteSubstitutionCount}. Aggregated
   * over both channels, so it does not identify which channel substituted.
   */
  nonFiniteSubstitutionCount: number;
  /** Per-stage gain reductions for the dynamics/maximizer stages (a subset of `stages`). */
  stageGainReductions: StageGainReduction[];
  /** Before/after measurements of the run. */
  report: MasteringReport;
}

/** Result of a single stereo mastering processor. */
export interface MasteringStereoResult {
  /** Left channel of the processed audio. */
  left: Float32Array;
  /** Right channel of the processed audio. */
  right: Float32Array;
  /** Sample rate of the returned audio in Hz. */
  sampleRate: number;
  /** Integrated loudness of the input in LUFS. */
  inputLufs: number;
  /** Integrated loudness of the output in LUFS. */
  outputLufs: number;
  /** Gain applied, in dB. */
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
