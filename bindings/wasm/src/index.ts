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

import type {
  AnalysisResult,
  ChordQuality,
  ChromaResult,
  HpssResult,
  Key,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoChainResult,
  MasteringStereoResult,
  MelSpectrogramResult,
  MfccResult,
  Mode,
  PitchClass,
  PitchResult,
  SectionType,
  StftResult,
} from './public_types';
import type { AnalyzerStats, FrameBuffer, StreamConfig } from './stream_types';
import type {
  ProgressCallback,
  SonareModule,
  WasmAnalysisResult,
  WasmFrameResult,
  WasmStreamAnalyzer,
  WasmTempogramResult,
  WasmTrimResult,
} from './wasm_types';

export type {
  AnalysisResult,
  Beat,
  Chord,
  ChromaResult,
  Dynamics,
  HpssResult,
  Key,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoChainResult,
  MasteringStereoResult,
  MelSpectrogramResult,
  MfccResult,
  PitchResult,
  RhythmFeatures,
  Section,
  StftResult,
  Timbre,
  TimeSignature,
} from './public_types';
export {
  ChordQuality,
  Mode,
  PitchClass,
  SectionType,
} from './public_types';
export type {
  AnalyzerStats,
  BarChord,
  ChordChange,
  FrameBuffer,
  PatternScore,
  ProgressiveEstimate,
  StreamConfig,
} from './stream_types';
export type { ProgressCallback } from './wasm_types';

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
    try {
      const createModule = (await import('./sonare.js')).default;
      module = await createModule(options);
    } catch (error) {
      initPromise = null;
      throw error;
    }
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
  const beatTimes = new Float32Array(wasm.beats.length);
  for (let i = 0; i < wasm.beats.length; i++) {
    beatTimes[i] = wasm.beats[i].time;
  }
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
    beatTimes,
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
 * Apply mastering loudness normalization with a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetLufs - Target integrated LUFS (default: -14)
 * @param ceilingDb - True/sample peak ceiling in dBFS (default: -1)
 * @param truePeakOversample - Oversampling factor used for peak estimation
 * @returns Processed audio and loudness metadata
 */
export function mastering(
  samples: Float32Array,
  sampleRate: number,
  targetLufs = -14.0,
  ceilingDb = -1.0,
  truePeakOversample = 4,
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mastering(samples, sampleRate, targetLufs, ceilingDb, truePeakOversample);
}

export function masteringProcessorNames(): string[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringProcessorNames();
}

export function masteringPairProcessorNames(): string[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairProcessorNames();
}

export function masteringPairAnalysisNames(): string[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairAnalysisNames();
}

export function masteringStereoAnalysisNames(): string[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringStereoAnalysisNames();
}

export function masteringProcess(
  processorName: string,
  samples: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: string,
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringStereoResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masteringProcessStereo(processorName, left, right, sampleRate, params);
}

export function masteringPairProcess(
  processorName: string,
  source: Float32Array,
  reference: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairProcess(processorName, source, reference, sampleRate, params);
}

export function masteringPairAnalyze(
  analysisName: string,
  source: Float32Array,
  reference: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: string,
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

/**
 * Apply a configurable mastering chain in WASM.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChain(
  samples: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
): MasteringChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringChain(samples, sampleRate, config as Record<string, unknown>);
}

/**
 * Apply a configurable stereo mastering chain in WASM.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
): MasteringStereoChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masteringChainStereo(left, right, sampleRate, config as Record<string, unknown>);
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

export function framesToSamples(frames: number, hopLength = 512, nFft = 0): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.framesToSamples(frames, hopLength, nFft);
}

export function samplesToFrames(samples: number, hopLength = 512, nFft = 0): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.samplesToFrames(samples, hopLength, nFft);
}

export function powerToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.powerToDb(values, ref, amin, topDb);
}

export function amplitudeToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.amplitudeToDb(values, ref, amin, topDb);
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.dbToAmplitude(values, ref);
}

export function preemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.preemphasis(samples, coef, zi ?? null);
}

export function deemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.deemphasis(samples, coef, zi ?? null);
}

export function trimSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): WasmTrimResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.trimSilence(samples, topDb, frameLength, hopLength);
}

export function splitSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.splitSilence(samples, topDb, frameLength, hopLength);
}

export function frameSignal(
  samples: Float32Array,
  frameLength: number,
  hopLength: number,
): WasmFrameResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.frameSignal(samples, frameLength, hopLength);
}

export function padCenter(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.padCenter(values, size, padValue);
}

export function fixLength(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.fixLength(values, size, padValue);
}

export function fixFrames(frames: Int32Array, xMin = 0, xMax = -1, pad = true): Int32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.fixFrames(frames, xMin, xMax, pad);
}

export function peakPick(
  values: Float32Array,
  preMax: number,
  postMax: number,
  preAvg: number,
  postAvg: number,
  delta: number,
  wait: number,
): Int32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.peakPick(values, preMax, postMax, preAvg, postAvg, delta, wait);
}

export function vectorNormalize(
  values: Float32Array,
  normType = 0,
  threshold = 1e-12,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.vectorNormalize(values, normType, threshold);
}

export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options: Record<string, number> = {},
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pcen(values, nBins, nFrames, options);
}

export function tonnetz(chromagram: Float32Array, nChroma: number, nFrames: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.tonnetz(chromagram, nChroma, nFrames);
}

export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate: number,
  hopLength = 512,
  winLength = 384,
): WasmTempogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.tempogram(onsetEnvelope, sampleRate, hopLength, winLength);
}

export function plp(
  onsetEnvelope: Float32Array,
  sampleRate: number,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.plp(onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength);
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
// Audio Class
// ============================================================================

/**
 * Wrapper around audio data that exposes all analysis and feature functions as instance methods.
 *
 * @example
 * ```typescript
 * import { init, Audio } from '@libraz/sonare';
 *
 * await init();
 *
 * const audio = Audio.fromBuffer(samples, 44100);
 * console.log('BPM:', audio.detectBpm());
 * console.log('Key:', audio.detectKey().name);
 *
 * const mel = audio.melSpectrogram();
 * ```
 */
export class Audio {
  private _samples: Float32Array;
  private _sampleRate: number;

  private constructor(samples: Float32Array, sampleRate: number) {
    this._samples = samples;
    this._sampleRate = sampleRate;
  }

  /** Create an Audio instance from raw sample data. */
  static fromBuffer(samples: Float32Array, sampleRate: number): Audio {
    return new Audio(samples, sampleRate);
  }

  /** The raw audio samples. */
  get data(): Float32Array {
    return this._samples;
  }

  /** Number of samples. */
  get length(): number {
    return this._samples.length;
  }

  /** Sample rate in Hz. */
  get sampleRate(): number {
    return this._sampleRate;
  }

  /** Duration in seconds. */
  get duration(): number {
    return this._samples.length / this._sampleRate;
  }

  // -- Analysis --

  detectBpm(): number {
    return detectBpm(this._samples, this._sampleRate);
  }

  detectKey(): Key {
    return detectKey(this._samples, this._sampleRate);
  }

  detectOnsets(): Float32Array {
    return detectOnsets(this._samples, this._sampleRate);
  }

  detectBeats(): Float32Array {
    return detectBeats(this._samples, this._sampleRate);
  }

  analyze(): AnalysisResult {
    return analyze(this._samples, this._sampleRate);
  }

  analyzeWithProgress(onProgress: ProgressCallback): AnalysisResult {
    return analyzeWithProgress(this._samples, this._sampleRate, onProgress);
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return hpss(this._samples, this._sampleRate, kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonic(this._samples, this._sampleRate);
  }

  percussive(): Float32Array {
    return percussive(this._samples, this._sampleRate);
  }

  timeStretch(rate: number): Float32Array {
    return timeStretch(this._samples, this._sampleRate, rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShift(this._samples, this._sampleRate, semitones);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalize(this._samples, this._sampleRate, targetDb);
  }

  mastering(targetLufs = -14.0, ceilingDb = -1.0, truePeakOversample = 4): MasteringResult {
    return mastering(this._samples, this._sampleRate, targetLufs, ceilingDb, truePeakOversample);
  }

  masteringChain(config: MasteringChainConfig): MasteringChainResult {
    return masteringChain(this._samples, this._sampleRate, config);
  }

  masteringProcess(processorName: string, params: MasteringProcessorParams = {}): MasteringResult {
    return masteringProcess(processorName, this._samples, this._sampleRate, params);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return trim(this._samples, this._sampleRate, thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return stft(this._samples, this._sampleRate, nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): { nBins: number; nFrames: number; db: Float32Array } {
    return stftDb(this._samples, this._sampleRate, nFft, hopLength);
  }

  melSpectrogram(nFft = 2048, hopLength = 512, nMels = 128): MelSpectrogramResult {
    return melSpectrogram(this._samples, this._sampleRate, nFft, hopLength, nMels);
  }

  mfcc(nFft = 2048, hopLength = 512, nMels = 128, nMfcc = 13): MfccResult {
    return mfcc(this._samples, this._sampleRate, nFft, hopLength, nMels, nMfcc);
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return chroma(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return spectralCentroid(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return spectralBandwidth(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return spectralRolloff(this._samples, this._sampleRate, nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return spectralFlatness(this._samples, this._sampleRate, nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return zeroCrossingRate(this._samples, this._sampleRate, frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return rmsEnergy(this._samples, this._sampleRate, frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
  ): PitchResult {
    return pitchYin(this._samples, this._sampleRate, frameLength, hopLength, fmin, fmax, threshold);
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
  ): PitchResult {
    return pitchPyin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
    );
  }

  resample(targetSr: number): Float32Array {
    return resample(this._samples, this._sampleRate, targetSr);
  }
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

export { PitchClass as Pitch } from './public_types';
