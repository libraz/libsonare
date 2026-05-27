import type { ChordQuality, PitchClass } from './public_types';

/**
 * A detected chord change in the progression
 */
export interface ChordChange {
  root: PitchClass;
  quality: ChordQuality;
  startTime: number;
  confidence: number;
}

/**
 * A chord detected at bar boundary (beat-synchronized)
 */
export interface BarChord {
  barIndex: number;
  root: PitchClass;
  quality: ChordQuality;
  startTime: number;
  confidence: number;
}

/**
 * Pattern score for known chord progressions
 */
export interface PatternScore {
  name: string;
  score: number;
}

/**
 * Progressive estimation results for BPM, Key, and Chord
 */
export interface ProgressiveEstimate {
  bpm: number;
  bpmConfidence: number;
  bpmCandidateCount: number;
  key: PitchClass;
  keyMinor: boolean;
  keyConfidence: number;
  chordRoot: PitchClass;
  chordQuality: ChordQuality;
  chordConfidence: number;
  chordStartTime: number;
  chordProgression: ChordChange[];
  barChordProgression: BarChord[];
  currentBar: number;
  barDuration: number;
  votedPattern: BarChord[];
  patternLength: number;
  detectedPatternName: string;
  detectedPatternScore: number;
  allPatternScores: PatternScore[];
  accumulatedSeconds: number;
  usedFrames: number;
  updated: boolean;
}

/**
 * Statistics and current state of the analyzer
 */
export interface AnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  estimate: ProgressiveEstimate;
}

/**
 * Frame buffer with analysis results
 */
export interface FrameBuffer {
  nFrames: number;
  timestamps: Float32Array;
  mel: Float32Array;
  chroma: Float32Array;
  onsetStrength: Float32Array;
  rmsEnergy: Float32Array;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  chordRoot: Int32Array;
  chordQuality: Int32Array;
  chordConfidence: Float32Array;
}

export interface StreamFramesU8 {
  nFrames: number;
  nMels: number;
  timestamps: Float32Array;
  mel: Uint8Array;
  chroma: Uint8Array;
  onsetStrength: Uint8Array;
  rmsEnergy: Uint8Array;
  spectralCentroid: Uint8Array;
  spectralFlatness: Uint8Array;
}

export interface StreamFramesI16 {
  nFrames: number;
  nMels: number;
  timestamps: Float32Array;
  mel: Int16Array;
  chroma: Int16Array;
  onsetStrength: Int16Array;
  rmsEnergy: Int16Array;
  spectralCentroid: Int16Array;
  spectralFlatness: Int16Array;
}

/**
 * Configuration for StreamAnalyzer
 */
export interface StreamConfig {
  sampleRate: number;
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  fmin?: number;
  fmax?: number;
  tuningRefHz?: number;
  computeMagnitude?: boolean;
  computeMel?: boolean;
  computeChroma?: boolean;
  computeOnset?: boolean;
  computeSpectral?: boolean;
  emitEveryNFrames?: number;
  magnitudeDownsample?: number;
  keyUpdateIntervalSec?: number;
  bpmUpdateIntervalSec?: number;
  window?: number;
  outputFormat?: number;
}
