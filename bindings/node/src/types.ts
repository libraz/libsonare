export interface Key {
  root: string;
  mode: string;
  confidence: number;
  name: string;
  shortName: string;
}

export interface TimeSignature {
  numerator: number;
  denominator: number;
  confidence: number;
}

export interface AnalysisResult {
  bpm: number;
  bpmConfidence: number;
  key: Key;
  timeSignature: TimeSignature;
  beatTimes: Float32Array;
  beats: Array<{ time: number; strength: undefined }>;
}

export interface BpmCandidate {
  bpm: number;
  confidence: number;
}

export interface BpmAnalysisResult {
  bpm: number;
  confidence: number;
  candidates: BpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

export interface RhythmResult {
  bpm: number;
  timeSignature: TimeSignature;
  grooveType: 'straight' | 'shuffle' | 'swing';
  syncopation: number;
  patternRegularity: number;
  tempoStability: number;
  beatIntervals: Float32Array;
}

export interface DynamicsResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
  loudnessTimes: Float32Array;
  loudnessRmsDb: Float32Array;
}

export interface TimbreResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
}

export interface Chord {
  root: string;
  quality:
    | 'major'
    | 'minor'
    | 'diminished'
    | 'augmented'
    | 'dominant7'
    | 'major7'
    | 'minor7'
    | 'sus2'
    | 'sus4'
    | 'unknown';
  start: number;
  end: number;
  duration: number;
  confidence: number;
}

export interface ChordAnalysisResult {
  chords: Chord[];
}

export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}

export interface MasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
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

export interface MasteringChainResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export interface MasteringChainStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export interface StftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

export interface StftDbResult {
  nBins: number;
  nFrames: number;
  db: Float32Array;
}

export interface MelSpectrogramResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

export interface MfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

export interface ChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

export interface PitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}
