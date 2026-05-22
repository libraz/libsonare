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
} as const;

export type Mode = (typeof Mode)[keyof typeof Mode];

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
} as const;

export type ChordQuality = (typeof ChordQuality)[keyof typeof ChordQuality];

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
  quality: ChordQuality;
  start: number;
  end: number;
  confidence: number;
  name: string;
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
  form: string;
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
