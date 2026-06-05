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
  /** Number of mel bands; flat `mel` is `[nFrames * nMels]` row-major. */
  nMels: number;
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

/**
 * Quantization ranges for the uint8/int16 bandwidth-reduction read paths
 * (`StreamAnalyzer.readFramesU8` / `readFramesI16`). Omitted fields fall back to
 * the library defaults shown below; widen any range whose source values exceed
 * the defaults, otherwise a louder/quieter stream saturates to the endpoints.
 */
export interface StreamQuantizeConfig {
  /** dB floor for mel quantization (default -80). */
  melDbMin?: number;
  /** dB ceiling for mel quantization (default 0). */
  melDbMax?: number;
  /** Max expected onset strength (default 50). */
  onsetMax?: number;
  /** Max expected RMS energy (default 1). */
  rmsMax?: number;
  /** Max expected spectral centroid in Hz (default 11025). */
  centroidMax?: number;
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
 *
 * Omitted values are read from the native StreamConfig defaults via
 * streamAnalyzerConfigDefault(), keeping the WASM wrapper in sync with core.
 */
export interface StreamConfig {
  /** Sample rate in Hz. Optional for parity with the Node/Python bindings. */
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  fmin?: number;
  fmax?: number;
  tuningRefHz?: number;
  /** Unsupported: no read path surfaces per-frame magnitude spectra. */
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

export type StreamConfigDefaults = Required<StreamConfig>;
