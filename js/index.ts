/**
 * sonare - Audio Analysis Library
 *
 * @example
 * ```typescript
 * import { init, detectBpm, detectKey, analyze } from '@libraz/sonare';
 *
 * await init();
 *
 * // Detect BPM from audio samples
 * const bpm = detectBpm(samples, sampleRate);
 *
 * // Detect musical key
 * const key = detectKey(samples, sampleRate);
 *
 * // Full analysis
 * const result = analyze(samples, sampleRate);
 * ```
 */

/**
 * Progress callback for analysis progress reporting.
 * @param progress - Progress value (0.0 to 1.0)
 * @param stage - Current analysis stage name
 */
export type ProgressCallback = (progress: number, stage: string) => void;

// ============================================================================
// Internal WASM types
// ============================================================================

interface WasmKeyResult {
  root: number;
  mode: number;
  confidence: number;
  name: string;
  shortName: string;
}

interface WasmBeatResult {
  time: number;
  strength: number;
}

interface WasmChordResult {
  root: number;
  quality: number;
  start: number;
  end: number;
  confidence: number;
  name: string;
}

interface WasmSectionResult {
  type: number;
  start: number;
  end: number;
  energyLevel: number;
  confidence: number;
  name: string;
}

interface WasmTimbreResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

interface WasmDynamicsResult {
  dynamicRangeDb: number;
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

interface WasmRhythmResult {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
}

interface WasmTimeSignatureResult {
  numerator: number;
  denominator: number;
  confidence: number;
}

interface WasmAnalysisResult {
  bpm: number;
  bpmConfidence: number;
  key: WasmKeyResult;
  timeSignature: WasmTimeSignatureResult;
  beats: WasmBeatResult[];
  chords: WasmChordResult[];
  sections: WasmSectionResult[];
  timbre: WasmTimbreResult;
  dynamics: WasmDynamicsResult;
  rhythm: WasmRhythmResult;
  form: string;
}

interface WasmHpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}

interface WasmStftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

interface WasmStftDbResult {
  nBins: number;
  nFrames: number;
  db: Float32Array;
}

interface WasmMelResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

interface WasmMfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

interface WasmChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

interface WasmPitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

// Streaming types
interface WasmChordChange {
  root: number;
  quality: number;
  startTime: number;
  confidence: number;
}

interface WasmBarChord {
  barIndex: number;
  root: number;
  quality: number;
  startTime: number;
  confidence: number;
}

interface WasmPatternScore {
  name: string;
  score: number;
}

interface WasmProgressiveEstimate {
  bpm: number;
  bpmConfidence: number;
  bpmCandidateCount: number;
  key: number;
  keyMinor: boolean;
  keyConfidence: number;
  chordRoot: number;
  chordQuality: number;
  chordConfidence: number;
  chordProgression: WasmChordChange[];
  barChordProgression: WasmBarChord[];
  currentBar: number;
  barDuration: number;
  votedPattern: WasmBarChord[];
  patternLength: number;
  detectedPatternName: string;
  detectedPatternScore: number;
  allPatternScores: WasmPatternScore[];
  accumulatedSeconds: number;
  usedFrames: number;
  updated: boolean;
}

interface WasmAnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  estimate: WasmProgressiveEstimate;
}

interface WasmFrameBuffer {
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

interface WasmStreamAnalyzer {
  process: (samples: Float32Array) => void;
  processWithOffset: (samples: Float32Array, sampleOffset: number) => void;
  availableFrames: () => number;
  readFramesSoa: (maxFrames: number) => WasmFrameBuffer;
  readFramesU8: (maxFrames: number) => unknown;
  readFramesI16: (maxFrames: number) => unknown;
  reset: (baseSampleOffset?: number) => void;
  stats: () => WasmAnalyzerStats;
  frameCount: () => number;
  currentTime: () => number;
  sampleRate: () => number;
  setExpectedDuration: (durationSeconds: number) => void;
  setNormalizationGain: (gain: number) => void;
  setTuningRefHz: (refHz: number) => void;
  delete: () => void;
}

// Types for Emscripten module with embind exports
interface SonareModule {
  // Quick API
  detectBpm: (samples: Float32Array, sampleRate: number) => number;
  detectKey: (samples: Float32Array, sampleRate: number) => WasmKeyResult;
  detectOnsets: (samples: Float32Array, sampleRate: number) => Float32Array;
  detectBeats: (samples: Float32Array, sampleRate: number) => Float32Array;
  analyze: (samples: Float32Array, sampleRate: number) => WasmAnalysisResult;
  analyzeWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    progressCallback: ProgressCallback | null,
  ) => WasmAnalysisResult;
  version: () => string;

  // Effects
  hpss: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
  ) => WasmHpssResult;
  harmonic: (samples: Float32Array, sampleRate: number) => Float32Array;
  percussive: (samples: Float32Array, sampleRate: number) => Float32Array;
  timeStretch: (samples: Float32Array, sampleRate: number, rate: number) => Float32Array;
  pitchShift: (samples: Float32Array, sampleRate: number, semitones: number) => Float32Array;
  normalize: (samples: Float32Array, sampleRate: number, targetDb: number) => Float32Array;
  trim: (samples: Float32Array, sampleRate: number, thresholdDb: number) => Float32Array;

  // Features - Spectrogram
  stft: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmStftResult;
  stftDb: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmStftDbResult;

  // Features - Mel Spectrogram
  melSpectrogram: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
  ) => WasmMelResult;
  mfcc: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nMfcc: number,
  ) => WasmMfccResult;

  // Features - Chroma
  chroma: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmChromaResult;

  // Features - Spectral
  spectralCentroid: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  spectralBandwidth: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  spectralRolloff: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    rollPercent: number,
  ) => Float32Array;
  spectralFlatness: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  zeroCrossingRate: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
  ) => Float32Array;
  rmsEnergy: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
  ) => Float32Array;

  // Features - Pitch
  pitchYin: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    threshold: number,
  ) => WasmPitchResult;
  pitchPyin: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    threshold: number,
  ) => WasmPitchResult;

  // Core - Conversion
  hzToMel: (hz: number) => number;
  melToHz: (mel: number) => number;
  hzToMidi: (hz: number) => number;
  midiToHz: (midi: number) => number;
  hzToNote: (hz: number) => string;
  noteToHz: (note: string) => number;
  framesToTime: (frames: number, sr: number, hopLength: number) => number;
  timeToFrames: (time: number, sr: number, hopLength: number) => number;

  // Core - Resample
  resample: (samples: Float32Array, srcSr: number, targetSr: number) => Float32Array;

  // Streaming - StreamAnalyzer class
  StreamAnalyzer: new (
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    computeMel: boolean,
    computeChroma: boolean,
    computeOnset: boolean,
    emitEveryNFrames: number,
  ) => WasmStreamAnalyzer;
}

// ============================================================================
// Public Types
// ============================================================================

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

// ============================================================================
// Module State
// ============================================================================

let module: SonareModule | null = null;
let initPromise: Promise<void> | null = null;

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the WASM module.
 * Must be called before using any analysis functions.
 *
 * @param options - Optional module configuration
 * @returns Promise that resolves when initialization is complete
 */
export async function init(options?: {
  locateFile?: (path: string, prefix: string) => string;
}): Promise<void> {
  if (module) {
    return;
  }

  if (initPromise) {
    return initPromise;
  }

  initPromise = (async () => {
    const createModule = (await import('./sonare.js')).default;
    module = await createModule(options);
  })();

  return initPromise;
}

/**
 * Check if the module is initialized.
 */
export function isInitialized(): boolean {
  return module !== null;
}

/**
 * Get the library version.
 */
export function version(): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.version();
}

// ============================================================================
// Quick API (High-level Analysis)
// ============================================================================

/**
 * Detect BPM from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Detected BPM
 */
export function detectBpm(samples: Float32Array, sampleRate: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectBpm(samples, sampleRate);
}

/**
 * Detect musical key from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Detected key
 */
export function detectKey(samples: Float32Array, sampleRate: number): Key {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.detectKey(samples, sampleRate);
  return {
    root: result.root as PitchClass,
    mode: result.mode as Mode,
    confidence: result.confidence,
    name: result.name,
    shortName: result.shortName,
  };
}

/**
 * Detect onset times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Array of onset times in seconds
 */
export function detectOnsets(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectOnsets(samples, sampleRate);
}

/**
 * Detect beat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Array of beat times in seconds
 */
export function detectBeats(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectBeats(samples, sampleRate);
}

// Helper to convert WASM result to typed result
function convertAnalysisResult(wasm: WasmAnalysisResult): AnalysisResult {
  return {
    bpm: wasm.bpm,
    bpmConfidence: wasm.bpmConfidence,
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    timeSignature: wasm.timeSignature,
    beats: wasm.beats,
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
      confidence: c.confidence,
      name: c.name,
    })),
    sections: wasm.sections.map((s) => ({
      type: s.type as SectionType,
      start: s.start,
      end: s.end,
      energyLevel: s.energyLevel,
      confidence: s.confidence,
      name: s.name,
    })),
    timbre: wasm.timbre,
    dynamics: wasm.dynamics,
    rhythm: wasm.rhythm,
    form: wasm.form,
  };
}

/**
 * Perform complete music analysis.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Complete analysis result
 */
export function analyze(samples: Float32Array, sampleRate: number): AnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.analyze(samples, sampleRate);
  return convertAnalysisResult(result);
}

/**
 * Perform complete music analysis with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Complete analysis result
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number,
  onProgress: ProgressCallback,
): AnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.analyzeWithProgress(samples, sampleRate, onProgress);
  return convertAnalysisResult(result);
}

// ============================================================================
// Effects
// ============================================================================

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param kernelHarmonic - Horizontal median filter size for harmonic (default: 31)
 * @param kernelPercussive - Vertical median filter size for percussive (default: 31)
 * @returns Separated harmonic and percussive components
 */
export function hpss(
  samples: Float32Array,
  sampleRate: number,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.harmonic(samples, sampleRate);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.percussive(samples, sampleRate);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @returns Time-stretched audio
 */
export function timeStretch(samples: Float32Array, sampleRate: number, rate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.timeStretch(samples, sampleRate, rate);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @returns Pitch-shifted audio
 */
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchShift(samples, sampleRate, semitones);
}

/**
 * Normalize audio to target peak level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetDb - Target peak level in dB (default: 0 dB = full scale)
 * @returns Normalized audio
 */
export function normalize(samples: Float32Array, sampleRate: number, targetDb = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.normalize(samples, sampleRate, targetDb);
}

/**
 * Trim silence from beginning and end of audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param thresholdDb - Silence threshold in dB (default: -60 dB)
 * @returns Trimmed audio
 */
export function trim(samples: Float32Array, sampleRate: number, thresholdDb = -60.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.trim(samples, sampleRate, thresholdDb);
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Compute Short-Time Fourier Transform (STFT).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with magnitude and power spectrograms
 */
export function stft(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): StftResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.stft(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute STFT and return magnitude in decibels.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with dB values
 */
export function stftDb(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): { nBins: number; nFrames: number; db: Float32Array } {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.stftDb(samples, sampleRate, nFft, hopLength);
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

/**
 * Compute Mel spectrogram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @returns Mel spectrogram result
 */
export function melSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): MelSpectrogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melSpectrogram(samples, sampleRate, nFft, hopLength, nMels);
}

/**
 * Compute MFCC (Mel-Frequency Cepstral Coefficients).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param nMfcc - Number of MFCC coefficients (default: 13)
 * @returns MFCC result
 */
export function mfcc(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 13,
): MfccResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mfcc(samples, sampleRate, nFft, hopLength, nMels, nMfcc);
}

// ============================================================================
// Features - Chroma
// ============================================================================

/**
 * Compute chromagram (pitch class distribution).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Chroma features result
 */
export function chroma(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): ChromaResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.chroma(samples, sampleRate, nFft, hopLength);
}

// ============================================================================
// Features - Spectral
// ============================================================================

/**
 * Compute spectral centroid (center of mass of spectrum).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral centroid in Hz for each frame
 */
export function spectralCentroid(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralCentroid(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral bandwidth.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral bandwidth in Hz for each frame
 */
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralBandwidth(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral rolloff frequency.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param rollPercent - Percentage threshold (default: 0.85)
 * @returns Rolloff frequency in Hz for each frame
 */
export function spectralRolloff(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralRolloff(samples, sampleRate, nFft, hopLength, rollPercent);
}

/**
 * Compute spectral flatness.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral flatness for each frame (0 = tonal, 1 = noise-like)
 */
export function spectralFlatness(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralFlatness(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute zero crossing rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Zero crossing rate for each frame
 */
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.zeroCrossingRate(samples, sampleRate, frameLength, hopLength);
}

/**
 * Compute RMS energy.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns RMS energy for each frame
 */
export function rmsEnergy(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.rmsEnergy(samples, sampleRate, frameLength, hopLength);
}

// ============================================================================
// Features - Pitch
// ============================================================================

/**
 * Detect pitch using YIN algorithm.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.3)
 * @returns Pitch detection result
 */
export function pitchYin(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
): PitchResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchYin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
}

/**
 * Detect pitch using pYIN algorithm (probabilistic YIN with HMM smoothing).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.3)
 * @returns Pitch detection result
 */
export function pitchPyin(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
): PitchResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchPyin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
}

// ============================================================================
// Core - Unit Conversion
// ============================================================================

/**
 * Convert frequency in Hz to Mel scale.
 *
 * @param hz - Frequency in Hz
 * @returns Mel frequency
 */
export function hzToMel(hz: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToMel(hz);
}

/**
 * Convert Mel scale to frequency in Hz.
 *
 * @param mel - Mel frequency
 * @returns Frequency in Hz
 */
export function melToHz(mel: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melToHz(mel);
}

/**
 * Convert frequency in Hz to MIDI note number.
 *
 * @param hz - Frequency in Hz
 * @returns MIDI note number (A4 = 440 Hz = 69)
 */
export function hzToMidi(hz: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToMidi(hz);
}

/**
 * Convert MIDI note number to frequency in Hz.
 *
 * @param midi - MIDI note number
 * @returns Frequency in Hz
 */
export function midiToHz(midi: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.midiToHz(midi);
}

/**
 * Convert frequency in Hz to note name.
 *
 * @param hz - Frequency in Hz
 * @returns Note name (e.g., "A4", "C#5")
 */
export function hzToNote(hz: number): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToNote(hz);
}

/**
 * Convert note name to frequency in Hz.
 *
 * @param note - Note name (e.g., "A4", "C#5")
 * @returns Frequency in Hz
 */
export function noteToHz(note: string): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.noteToHz(note);
}

/**
 * Convert frame index to time in seconds.
 *
 * @param frames - Frame index
 * @param sr - Sample rate in Hz
 * @param hopLength - Hop length in samples
 * @returns Time in seconds
 */
export function framesToTime(frames: number, sr: number, hopLength: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.framesToTime(frames, sr, hopLength);
}

/**
 * Convert time in seconds to frame index.
 *
 * @param time - Time in seconds
 * @param sr - Sample rate in Hz
 * @param hopLength - Hop length in samples
 * @returns Frame index
 */
export function timeToFrames(time: number, sr: number, hopLength: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.timeToFrames(time, sr, hopLength);
}

// ============================================================================
// Core - Resample
// ============================================================================

/**
 * Resample audio to a different sample rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param srcSr - Source sample rate in Hz
 * @param targetSr - Target sample rate in Hz
 * @returns Resampled audio
 */
export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.resample(samples, srcSr, targetSr);
}

// ============================================================================
// Streaming Types
// ============================================================================

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

/**
 * Configuration for StreamAnalyzer
 */
export interface StreamConfig {
  sampleRate: number;
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  computeMel?: boolean;
  computeChroma?: boolean;
  computeOnset?: boolean;
  emitEveryNFrames?: number;
}

// ============================================================================
// StreamAnalyzer Class
// ============================================================================

/**
 * Real-time streaming audio analyzer.
 *
 * @example
 * ```typescript
 * import { init, StreamAnalyzer } from '@libraz/sonare';
 *
 * await init();
 *
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 *
 * // In audio processing callback
 * analyzer.process(samples);
 *
 * // Get current analysis state
 * const stats = analyzer.stats();
 * console.log('BPM:', stats.estimate.bpm);
 * console.log('Key:', stats.estimate.key);
 * console.log('Chord progression:', stats.estimate.chordProgression);
 * ```
 */
export class StreamAnalyzer {
  private analyzer: WasmStreamAnalyzer;

  /**
   * Create a new StreamAnalyzer.
   *
   * @param config - Configuration options
   */
  constructor(config: StreamConfig) {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    this.analyzer = new module.StreamAnalyzer(
      config.sampleRate,
      config.nFft ?? 2048,
      config.hopLength ?? 512,
      config.nMels ?? 128,
      config.computeMel ?? true,
      config.computeChroma ?? true,
      config.computeOnset ?? true,
      config.emitEveryNFrames ?? 1,
    );
  }

  /**
   * Process audio samples.
   *
   * @param samples - Audio samples (mono, float32)
   */
  process(samples: Float32Array): void {
    this.analyzer.process(samples);
  }

  /**
   * Process audio samples with explicit sample offset.
   *
   * @param samples - Audio samples (mono, float32)
   * @param sampleOffset - Cumulative sample count at start of this chunk
   */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.analyzer.processWithOffset(samples, sampleOffset);
  }

  /**
   * Get the number of frames available to read.
   */
  availableFrames(): number {
    return this.analyzer.availableFrames();
  }

  /**
   * Read processed frames as Structure of Arrays.
   *
   * @param maxFrames - Maximum number of frames to read
   * @returns Frame buffer with analysis results
   */
  readFrames(maxFrames: number): FrameBuffer {
    return this.analyzer.readFramesSoa(maxFrames);
  }

  /**
   * Reset the analyzer state.
   *
   * @param baseSampleOffset - Starting sample offset (default 0)
   */
  reset(baseSampleOffset = 0): void {
    this.analyzer.reset(baseSampleOffset);
  }

  /**
   * Get current statistics and progressive estimates.
   *
   * @returns Analyzer statistics including BPM, key, and chord progression
   */
  stats(): AnalyzerStats {
    const s = this.analyzer.stats();
    return {
      totalFrames: s.totalFrames,
      totalSamples: s.totalSamples,
      durationSeconds: s.durationSeconds,
      estimate: {
        bpm: s.estimate.bpm,
        bpmConfidence: s.estimate.bpmConfidence,
        bpmCandidateCount: s.estimate.bpmCandidateCount,
        key: s.estimate.key as PitchClass,
        keyMinor: s.estimate.keyMinor,
        keyConfidence: s.estimate.keyConfidence,
        chordRoot: s.estimate.chordRoot as PitchClass,
        chordQuality: s.estimate.chordQuality as ChordQuality,
        chordConfidence: s.estimate.chordConfidence,
        chordProgression: s.estimate.chordProgression.map((c) => ({
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        barChordProgression: s.estimate.barChordProgression.map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        currentBar: s.estimate.currentBar,
        barDuration: s.estimate.barDuration,
        votedPattern: (s.estimate.votedPattern || []).map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        patternLength: s.estimate.patternLength,
        detectedPatternName: s.estimate.detectedPatternName || '',
        detectedPatternScore: s.estimate.detectedPatternScore || 0,
        allPatternScores: (s.estimate.allPatternScores || []).map((p) => ({
          name: p.name,
          score: p.score,
        })),
        accumulatedSeconds: s.estimate.accumulatedSeconds,
        usedFrames: s.estimate.usedFrames,
        updated: s.estimate.updated,
      },
    };
  }

  /**
   * Get total frames processed.
   */
  frameCount(): number {
    return this.analyzer.frameCount();
  }

  /**
   * Get current time position in seconds.
   */
  currentTime(): number {
    return this.analyzer.currentTime();
  }

  /**
   * Get the sample rate.
   */
  sampleRate(): number {
    return this.analyzer.sampleRate();
  }

  /**
   * Set the expected total duration for pattern lock timing.
   *
   * @param durationSeconds - Total duration in seconds
   */
  setExpectedDuration(durationSeconds: number): void {
    this.analyzer.setExpectedDuration(durationSeconds);
  }

  /**
   * Set normalization gain for loud/compressed audio.
   *
   * @param gain - Gain factor to apply (e.g., 0.5 for -6dB reduction)
   */
  setNormalizationGain(gain: number): void {
    this.analyzer.setNormalizationGain(gain);
  }

  /**
   * Set tuning reference frequency for non-standard tuning.
   *
   * @param refHz - Reference frequency for A4 (default 440 Hz)
   * @example
   * // If audio is 1 semitone sharp (A4 = 466.16 Hz)
   * analyzer.setTuningRefHz(466.16);
   * // If audio is 1 semitone flat (A4 = 415.30 Hz)
   * analyzer.setTuningRefHz(415.30);
   */
  setTuningRefHz(refHz: number): void {
    this.analyzer.setTuningRefHz(refHz);
  }

  /**
   * Release resources. Call when done using the analyzer.
   */
  dispose(): void {
    this.analyzer.delete();
  }
}

// ============================================================================
// Re-exports
// ============================================================================

export { PitchClass as Pitch };
