export interface Key {
  root: string;
  mode: string;
  confidence: number;
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
}

export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
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
