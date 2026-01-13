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

// Types for Emscripten module
interface EmscriptenModule {
  detectBpm: (samples: Float32Array, sampleRate: number) => number;
  detectKey: (samples: Float32Array, sampleRate: number) => KeyResult;
  detectOnsets: (samples: Float32Array, sampleRate: number) => Float32Array;
  detectBeats: (samples: Float32Array, sampleRate: number) => Float32Array;
  analyze: (samples: Float32Array, sampleRate: number) => AnalysisResult;
  analyzeWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    progressCallback: ProgressCallback | null,
  ) => AnalysisResult;
  version: () => string;
  PitchClass: typeof PitchClass;
  Mode: typeof Mode;
  ChordQuality: typeof ChordQuality;
  SectionType: typeof SectionType;
}

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

// Internal result type from WASM
interface KeyResult {
  root: number;
  mode: number;
  confidence: number;
  name: string;
  shortName: string;
}

// Module state
let module: EmscriptenModule | null = null;
let initPromise: Promise<void> | null = null;

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
  return module.analyze(samples, sampleRate);
}

/**
 * Perform complete music analysis with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Complete analysis result
 *
 * @example
 * ```typescript
 * const result = analyzeWithProgress(samples, sampleRate, (progress, stage) => {
 *   console.log(`${stage}: ${Math.round(progress * 100)}%`);
 * });
 * ```
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number,
  onProgress: ProgressCallback,
): AnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.analyzeWithProgress(samples, sampleRate, onProgress);
}

// Re-export enums
export { PitchClass as Pitch };
