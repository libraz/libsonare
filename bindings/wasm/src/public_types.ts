/**
 * Pitch class enum (C=0, C#=1, ..., B=11)
 */
export const PitchClass = {
  C: 0,
  Cs: 1,
  D: 2,
  Ds: 3,
  E: 4,
  F: 5,
  Fs: 6,
  G: 7,
  Gs: 8,
  A: 9,
  As: 10,
  B: 11,
} as const;

export type PitchClass = (typeof PitchClass)[keyof typeof PitchClass];

/**
 * Musical mode
 */
export const Mode = {
  Major: 0,
  Minor: 1,
  Dorian: 2,
  Phrygian: 3,
  Lydian: 4,
  Mixolydian: 5,
  Locrian: 6,
} as const;

export type Mode = (typeof Mode)[keyof typeof Mode];

export type TempogramMode = 'autocorrelation' | 'auto' | 'ac' | 'cosine' | 0 | 1;

export const KeyProfile = {
  KrumhanslSchmuckler: 0,
  Temperley: 1,
  Shaath: 2,
  FaraldoEDMT: 3,
  FaraldoEDMA: 4,
  FaraldoEDMM: 5,
  BellmanBudge: 6,
} as const;

export type KeyProfile = (typeof KeyProfile)[keyof typeof KeyProfile];

export type KeyProfileName =
  | 'ks'
  | 'krumhansl'
  | 'temperley'
  | 'shaath'
  | 'keyfinder'
  | 'faraldo-edmt'
  | 'edmt'
  | 'faraldo-edma'
  | 'edma'
  | 'faraldo-edmm'
  | 'edmm'
  | 'bellman-budge'
  | 'bellman';

/**
 * Chord quality
 */
export const ChordQuality = {
  Major: 0,
  Minor: 1,
  Diminished: 2,
  Augmented: 3,
  Dominant7: 4,
  Major7: 5,
  Minor7: 6,
  Sus2: 7,
  Sus4: 8,
  Unknown: 9,
  Add9: 10,
  MinorAdd9: 11,
  Dim7: 12,
  HalfDim7: 13,
  Major9: 14,
  Dominant9: 15,
  Sus2Add4: 16,
} as const;

export type ChordQuality = (typeof ChordQuality)[keyof typeof ChordQuality];

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

/**
 * Section type
 */
export const SectionType = {
  Intro: 0,
  Verse: 1,
  PreChorus: 2,
  Chorus: 3,
  Bridge: 4,
  Instrumental: 5,
  Outro: 6,
  Unknown: 7,
} as const;

export type SectionType = (typeof SectionType)[keyof typeof SectionType];

/**
 * Detected musical key
 */
export interface Key {
  root: PitchClass;
  mode: Mode;
  confidence: number;
  name: string;
  shortName: string;
}

export interface KeyDetectionOptions {
  nFft?: number;
  hopLength?: number;
  useHpss?: boolean;
  loudnessWeighted?: boolean;
  highPassHz?: number;
  modes?:
    | Mode[]
    | ('major' | 'minor' | 'dorian' | 'phrygian' | 'lydian' | 'mixolydian' | 'locrian')[]
    | 'major-minor'
    | 'all'
    | 'modal';
  profile?: KeyProfile | KeyProfileName;
  genreHint?: 'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz' | string;
}

export interface KeyCandidate {
  key: Key;
  correlation: number;
}

export interface ChordDetectionOptions {
  minDuration?: number;
  smoothingWindow?: number;
  threshold?: number;
  useTriadsOnly?: boolean;
  nFft?: number;
  hopLength?: number;
  useBeatSync?: boolean;
  useHmm?: boolean;
  hmmBeamWidth?: number;
  useKeyContext?: boolean;
  keyRoot?: PitchClass;
  keyMode?: Mode;
  detectInversions?: boolean;
  chromaMethod?: 'stft' | 'nnls';
}

/**
 * Detected beat
 */
export interface Beat {
  time: number;
  strength: number;
}

/**
 * Detected chord
 */
export interface Chord {
  root: PitchClass;
  bass: PitchClass;
  quality: ChordQuality;
  start: number;
  end: number;
  confidence: number;
  name: string;
}

export interface ChordAnalysisResult {
  chords: Chord[];
}

/**
 * Detected section
 */
export interface Section {
  type: SectionType;
  start: number;
  end: number;
  energyLevel: number;
  confidence: number;
  name: string;
}

/**
 * A single melody contour point (mirrors the C `SonareMelodyPoint`).
 */
export interface MelodyPoint {
  /** Frame time in seconds. */
  time: number;
  /** Estimated fundamental frequency in Hz (0 when unvoiced). */
  frequency: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

/**
 * Melody analysis result (mirrors the C `SonareMelodyResult`).
 */
export interface MelodyResult {
  points: MelodyPoint[];
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
}

/**
 * Constant-Q / Variable-Q transform magnitude result (mirrors the C
 * `SonareCqtResult`).
 */
export interface CqtResult {
  /** Number of frequency bins. */
  nBins: number;
  /** Number of time frames. */
  nFrames: number;
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Row-major `[nBins x nFrames]` magnitude matrix. */
  magnitude: Float32Array;
  /** Center frequency (Hz) of each of the `nBins` bins. */
  frequencies: Float32Array;
}

/**
 * Timbre characteristics
 */
export interface Timbre {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

/**
 * Dynamics characteristics
 */
export interface Dynamics {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

/**
 * Time signature
 */
export interface TimeSignature {
  numerator: number;
  denominator: number;
  confidence: number;
}

/**
 * Rhythm features
 */
export interface RhythmFeatures {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  timeSignature: TimeSignature;
}

/**
 * Melody contour from the unified analysis (pitch trajectory + summary stats).
 */
export interface MelodyContour {
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
  pitches: MelodyPoint[];
}

/**
 * Complete analysis result
 */
export interface AnalysisResult {
  bpm: number;
  bpmConfidence: number;
  key: Key;
  timeSignature: TimeSignature;
  beatTimes: Float32Array;
  beats: Beat[];
  chords: Chord[];
  sections: Section[];
  timbre: Timbre;
  dynamics: Dynamics;
  rhythm: RhythmFeatures;
  melody: MelodyContour;
  form: string;
}

/**
 * Room acoustic parameters from an impulse response
 */
export interface AcousticResult {
  rt60: number;
  edt: number;
  c50: number;
  c80: number;
  d50: number;
  rt60Bands: Float32Array;
  edtBands: Float32Array;
  c50Bands: Float32Array;
  c80Bands: Float32Array;
  confidence: number;
  isBlind: boolean;
}

/** Shoebox geometry + placement shared by RIR synthesis and the room morph. */
export interface RoomGeometryOptions {
  lengthM?: number;
  widthM?: number;
  heightM?: number;
  /** Uniform wall absorption, clamped to [0, 0.999] (the back-compat scalar). */
  absorption?: number;
  /**
   * Optional per-octave-band wall absorption (125/250/500/1k/2k/4k.. Hz). When
   * provided it overrides `absorption` unless `materialPreset` is set.
   */
  bandAbsorption?: Float32Array | number[];
  /**
   * Named wall-material preset (0 none; 1 concrete, 2 wood, 3 curtain,
   * 4 carpet, 5 glass). A non-zero preset wins over `bandAbsorption`/`absorption`.
   */
  materialPreset?: number;
  sourceX?: number;
  sourceY?: number;
  sourceZ?: number;
  listenerX?: number;
  listenerY?: number;
  listenerZ?: number;
  ismOrder?: number;
  seed?: number;
  maxSeconds?: number;
}

export interface RirSynthOptions extends RoomGeometryOptions {
  sampleRate?: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Early/late crossover in ms (0 = auto, ~sqrt(V) ms). */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms (0 = default). */
  crossfadeMs?: number;
}

export interface RirResult {
  rir: Float32Array;
  sampleRate: number;
  hasError: boolean;
}

export interface RoomEstimateOptions {
  aspectHintLw?: number;
  aspectHintLh?: number;
  referenceAbsorption?: number;
  preferEyring?: boolean;
  nOctaveBands?: number;
  /** Analyzer routing: 0 = auto, 1 = blind, 2 = impulse-response. */
  mode?: number;
  /** Analyzer decay-fit span in dB (0 = library default). */
  minDecayDb?: number;
  /** Analyzer noise-floor margin in dB (0 = library default). */
  noiseFloorMarginDb?: number;
}

export interface RoomEstimateResult {
  volume: number;
  length: number;
  width: number;
  height: number;
  drrDb: number;
  confidence: number;
  absorptionBands: Float32Array;
  rt60Bands: Float32Array;
}

export interface RoomMorphOptions extends RoomGeometryOptions {
  wet?: number;
  sourceTailSuppression?: number;
  /**
   * Use the Eyring statistical late-tail model for the target room (default
   * true); false = Sabine. Matches {@link RirSynthOptions.preferEyring}.
   */
  preferEyring?: boolean;
  /** Early/late crossover in ms (0 = auto, ~sqrt(V) ms). */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms (0 = default). */
  crossfadeMs?: number;
}

/**
 * HPSS (Harmonic-Percussive Source Separation) result
 */
export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
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
  latencySamples?: number;
}

export type MasteringProcessorParams = Record<string, number | boolean>;

export type PanMode = 'balance' | 'stereoPan' | 'stereo-pan' | 'dualPan' | 'dual-pan' | number;

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
 * Pan law applied when computing left/right gains from a pan position
 * (see {@link Mixer.setPanLaw}). Maps to the underlying integer code.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/**
 * Meter tap point for reading a strip's meter snapshot
 * (see {@link Mixer.meterTap} and {@link Mixer.stripMeter}).
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/** A single goniometer (left/right) sample returned by {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}

export interface MasteringChainConfig {
  repair?: {
    denoise?: boolean;
    nFft?: number;
    hopLength?: number;
    ddAlpha?: number;
    gainFloor?: number;
    declick?: {
      threshold?: number;
      neighborRatio?: number;
      maxClickSamples?: number;
      lpcOrder?: number;
      residualRatio?: number;
    };
    dereverb?: {
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
    tiltDb?: number;
    pivotHz?: number;
  };
  dynamics?: {
    compressor?: {
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      kneeDb?: number;
      makeupGainDb?: number;
      autoMakeup?: boolean;
    };
    deesser?: {
      frequencyHz?: number;
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      rangeDb?: number;
      bandpassQ?: number;
    };
    transientShaper?: {
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
      frequencyHz?: number;
      driveDb?: number;
      amount?: number;
      q?: number;
      evenOddMix?: number;
    };
  };
  spectral?: {
    airBand?: {
      amount?: number;
      shelfFrequencyHz?: number;
      dynamicThresholdDb?: number;
      dynamicRangeDb?: number;
    };
  };
  stereo?: {
    imager?: {
      width?: number;
      outputGainDb?: number;
      decorrelationAmount?: number;
      preserveEnergy?: boolean;
    };
    monoMaker?: {
      amount?: number;
    };
  };
  maximizer?: {
    truePeakLimiter?: {
      ceilingDb?: number;
      lookaheadMs?: number;
      releaseMs?: number;
      oversampleFactor?: number;
      applyGainAtInputRate?: boolean;
    };
  };
  loudness?: {
    targetLufs?: number;
    ceilingDb?: number;
    truePeakOversample?: number;
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

export interface MasteringChainResult extends MasteringResult {
  stages: string[];
}

export interface MasteringStereoChainResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
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

/**
 * STFT (Short-Time Fourier Transform) result
 */
export interface StftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

/**
 * Mel spectrogram result
 */
export interface MelSpectrogramResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

/**
 * MFCC result
 */
export interface MfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

/**
 * STFT power spectrogram result (from inverse Mel reconstruction)
 */
export interface StftPowerResult {
  nBins: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Mel power spectrogram result (from inverse MFCC reconstruction)
 */
export interface MelPowerResult {
  nMels: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Chroma features result
 */
export interface ChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

/**
 * Pitch detection result
 */
export interface PitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

/**
 * Loudness measurement result (EBU R128 / ITU-R BS.1770)
 */
export interface LufsResult {
  integratedLufs: number;
  momentaryLufs: number;
  shortTermLufs: number;
  loudnessRange: number;
}

/**
 * Realtime equalizer spectrum snapshot.
 *
 * Mirrors the C++ `EqualizerSpectrumSnapshot`: `preLeft`/`preRight` and
 * `postLeft`/`postRight` are the pre- and post-EQ spectrum streams (trimmed to
 * their valid count). `bandGainDb` holds per-band applied gain (24 entries),
 * `profileDb` the smoothed magnitude profile (16 entries), `lastAutoGainDb`
 * the latest auto-gain compensation, and `seq` increments each time a new
 * snapshot is published.
 */
export interface EqSpectrumSnapshot {
  preLeft: Float32Array;
  preRight: Float32Array;
  postLeft: Float32Array;
  postRight: Float32Array;
  bandGainDb: Float32Array;
  profileDb: Float32Array;
  lastAutoGainDb: number;
  seq: number;
}

/**
 * Equalizer band type (string union mirroring `sonare::mastering::eq::EqBandType`).
 */
export type EqBandType =
  | 'Peak'
  | 'LowShelf'
  | 'HighShelf'
  | 'LowPass'
  | 'HighPass'
  | 'BandPass'
  | 'Notch'
  | 'TiltShelf'
  | 'FlatTilt';

/** Biquad coefficient design mode. */
export type EqCoeffMode = 'Rbj' | 'Vicanek';

/** Stereo placement for an EQ band. */
export type EqStereoPlacement = 'Stereo' | 'Left' | 'Right' | 'Mid' | 'Side';

/** Per-band phase behaviour. */
export type EqBandPhase = 'Inherit' | 'ZeroLatency' | 'NaturalPhase' | 'LinearPhase';

/**
 * Equalizer band configuration accepted by {@link StreamingEqualizer.setBand}.
 *
 * All fields are optional; omitted values fall back to the C++ band defaults
 * (Peak, 1000 Hz, 0 dB gain, Butterworth Q, disabled).
 */
export interface EqBand {
  type?: EqBandType;
  frequencyHz?: number;
  gainDb?: number;
  q?: number;
  enabled?: boolean;
  coeffMode?: EqCoeffMode;
  slopeDbOct?: number;
  placement?: EqStereoPlacement;
  phase?: EqBandPhase;
  soloed?: boolean;
  bypassed?: boolean;
  proportionalQ?: boolean;
  proportionalQStrength?: number;
  dynamic?: boolean;
  thresholdDb?: number;
  autoThreshold?: boolean;
  ratio?: number;
  rangeDb?: number;
  attackMs?: number;
  releaseMs?: number;
  lookaheadMs?: number;
  externalSidechain?: boolean;
  sidechainFreqHz?: number;
  sidechainQ?: number;
}

/** Construction options for {@link StreamingEqualizer}. */
export interface StreamingEqualizerConfig {
  sampleRate?: number;
  maxBlockSize?: number;
}

/** Configuration for {@link StreamingRetune}. */
export interface StreamingRetuneConfig {
  /** Pitch shift in semitones, clamped by the native processor to +/-24. */
  semitones?: number;
  /** Wet/dry mix, clamped by the native processor to 0..1. */
  mix?: number;
  /** Grain size in samples. Use 0/omit to derive it from the sample rate. */
  grainSize?: number;
}

export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export interface RealtimeVoiceChangerPreset {
  schemaVersion: 1;
  id?: string;
  name?: string;
  description?: string;
  macros?: Record<string, number>;
  dsp?: Record<string, unknown>;
}

export type RealtimeVoiceChangerConfigInput = VoicePresetId | RealtimeVoiceChangerPreset;

/**
 * Flat (POD) realtime voice-changer configuration. Field names mirror the
 * C ABI `SonareRealtimeVoiceChangerConfig` / Python POD exactly (snake_case),
 * so a config can be round-tripped across bindings without renaming.
 */
export interface RealtimeVoiceChangerPodConfig {
  input_gain_db: number;
  output_gain_db: number;
  wet_mix: number;
  retune_semitones: number;
  retune_mix: number;
  retune_grain_size: number;
  formant_factor: number;
  formant_amount: number;
  formant_body: number;
  formant_brightness: number;
  formant_nasal: number;
  eq_highpass_hz: number;
  eq_body_db: number;
  eq_presence_db: number;
  eq_air_db: number;
  gate_threshold_db: number;
  gate_attack_ms: number;
  gate_release_ms: number;
  gate_range_db: number;
  compressor_threshold_db: number;
  compressor_ratio: number;
  compressor_attack_ms: number;
  compressor_release_ms: number;
  compressor_makeup_gain_db: number;
  deesser_frequency_hz: number;
  deesser_threshold_db: number;
  deesser_ratio: number;
  deesser_range_db: number;
  reverb_mix: number;
  reverb_time_ms: number;
  reverb_damping: number;
  reverb_seed: number;
  limiter_ceiling_db: number;
  limiter_release_ms: number;
  /** Non-zero enables the 4x-oversampled inter-sample-peak limiter (default enabled). */
  limiter_enable_isp_limiter: boolean;
  /** True-peak ceiling in dBTP applied by the ISP limiter (default -1.0). */
  limiter_isp_ceiling_dbtp: number;
}

/** Options for {@link StreamingEqualizer.match}. */
export interface EqMatchOptions {
  sampleRate?: number;
  maxBands?: number;
}
