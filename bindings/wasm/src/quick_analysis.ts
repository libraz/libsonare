import {
  chordChromaMethodValue,
  convertAnalysisResult,
  convertChordAnalysisResult,
  convertKeyCandidate,
  keyModeValues,
  keyProfileValue,
} from './analysis_helpers';
import { getSonareModule } from './module_state';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeTimbreOptions,
  ChordAnalysisResult,
  ChordDetectionOptions,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomMorphOptions,
} from './public_types';
import { Mode, PitchClass } from './public_types';
import type { ProgressCallback, WasmAcousticResult } from './sonare.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Quick API (High-level Analysis)
// ============================================================================

/**
 * Detect BPM from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Detected BPM
 */
export function detectBpm(samples: Float32Array, sampleRate = 22050): number {
  return requireModule().detectBpm(samples, sampleRate);
}

/**
 * Detect musical key from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Detected key
 */
export function detectKey(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): Key {
  const result = requireModule()._detectKeyWithOptions(
    samples,
    sampleRate,
    options.nFft ?? 4096,
    options.hopLength ?? 512,
    options.useHpss ?? false,
    options.loudnessWeighted ?? false,
    options.highPassHz ?? 0,
    keyModeValues(options.modes),
    keyProfileValue(options.profile),
    options.genreHint ?? '',
  );
  return {
    root: result.root as PitchClass,
    mode: result.mode as Mode,
    confidence: result.confidence,
    name: result.name,
    shortName: result.shortName,
  };
}

export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): KeyCandidate[] {
  return requireModule()
    ._detectKeyCandidates(
      samples,
      sampleRate,
      options.nFft ?? 4096,
      options.hopLength ?? 512,
      options.useHpss ?? false,
      options.loudnessWeighted ?? false,
      options.highPassHz ?? 0,
      keyModeValues(options.modes),
      keyProfileValue(options.profile),
      options.genreHint ?? '',
    )
    .map(convertKeyCandidate);
}

/**
 * Detect onset times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of onset times in seconds
 */
export function detectOnsets(samples: Float32Array, sampleRate = 22050): Float32Array {
  return requireModule().detectOnsets(samples, sampleRate);
}

/**
 * Detect beat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of beat times in seconds
 */
export function detectBeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return requireModule().detectBeats(samples, sampleRate);
}

/**
 * Detect downbeat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of downbeat times in seconds
 */
export function detectDownbeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return requireModule().detectDownbeats(samples, sampleRate);
}

/**
 * Detect chords from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param options - Optional chord detection settings
 * @returns Detected chord segments
 */
export function detectChords(
  samples: Float32Array,
  sampleRate = 22050,
  options: ChordDetectionOptions = {},
): ChordAnalysisResult {
  const result = requireModule().detectChords(
    samples,
    sampleRate,
    options.minDuration ?? 0.3,
    options.smoothingWindow ?? 2.0,
    options.threshold ?? 0.5,
    options.useTriadsOnly ?? false,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.useBeatSync ?? true,
    options.useHmm ?? false,
    options.hmmBeamWidth ?? 24,
    options.useKeyContext ?? false,
    options.keyRoot ?? PitchClass.C,
    options.keyMode ?? Mode.Major,
    options.detectInversions ?? false,
    chordChromaMethodValue(options.chromaMethod ?? 'stft'),
  );
  return convertChordAnalysisResult(result);
}

/**
 * Functional (Roman-numeral) harmonic analysis of the detected chord
 * progression, relative to the given key. Mirrors the C-ABI
 * `sonare_chord_functional_analysis` and the Node/Python `chordFunctionalAnalysis`.
 *
 * @returns One Roman-numeral label (e.g. "I", "IV", "V", "vi") per detected chord
 */
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: PitchClass,
  keyMode: Mode,
  sampleRate = 22050,
  options: ChordDetectionOptions = {},
): string[] {
  return requireModule().chordFunctionalAnalysis(
    samples,
    keyRoot,
    keyMode,
    sampleRate,
    options.minDuration ?? 0.3,
    options.smoothingWindow ?? 2.0,
    options.threshold ?? 0.5,
    options.useTriadsOnly ?? false,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.useBeatSync ?? true,
    options.useHmm ?? false,
    options.hmmBeamWidth ?? 24,
    options.useKeyContext ?? false,
    options.detectInversions ?? false,
    chordChromaMethodValue(options.chromaMethod ?? 'stft'),
  );
}

/**
 * Perform complete music analysis.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Complete analysis result
 *
 * @remarks
 * This call is synchronous and blocks until analysis completes. Unlike the
 * Node binding (which offers `analyzeAsync` on a libuv worker thread), the
 * WASM build runs on a single thread, so there is no non-blocking variant —
 * the DSP pipeline always runs to completion on the calling thread. To keep
 * the UI responsive for long inputs, drive this from a Web Worker and use
 * {@link analyzeWithProgress} to report progress.
 */
export function analyze(samples: Float32Array, sampleRate = 22050): AnalysisResult {
  const result = requireModule().analyze(samples, sampleRate);
  return convertAnalysisResult(result);
}

export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate = 48000,
  nOctaveBands = 6,
): AcousticResult {
  const result: WasmAcousticResult = requireModule().analyzeImpulseResponse(
    samples,
    sampleRate,
    nOctaveBands,
  );
  return result;
}

export function detectAcoustic(
  samples: Float32Array,
  sampleRate = 48000,
  options: AcousticOptions = {},
): AcousticResult {
  const result: WasmAcousticResult = requireModule().detectAcoustic(
    samples,
    sampleRate,
    options.nOctaveBands ?? 6,
    options.nThirdOctaveSubbands ?? 24,
    options.minDecayDb ?? 30.0,
    options.noiseFloorMarginDb ?? 10.0,
  );
  return result;
}

/**
 * Synthesize a room impulse response from shoebox geometry. `hasError` is true
 * when the source/listener falls outside the room (the RIR is then empty).
 */
export function synthesizeRir(options: RirSynthOptions = {}): RirResult {
  const module = requireModule();
  if (typeof module.synthesizeRir !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  return module.synthesizeRir(options);
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response.
 */
export function estimateRoom(
  samples: Float32Array,
  sampleRate = 48000,
  options: RoomEstimateOptions = {},
): RoomEstimateResult {
  const module = requireModule();
  if (typeof module.estimateRoom !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  return module.estimateRoom(samples, sampleRate, options);
}

/**
 * Morph a recording's reverberation toward a target room (creative FX, not
 * dereverberation). Returns the morphed samples (input length plus the target
 * room's reverb tail).
 */
export function roomMorph(
  samples: Float32Array,
  sampleRate: number,
  options: RoomMorphOptions = {},
): Float32Array {
  const module = requireModule();
  if (typeof module.roomMorph !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  return module.roomMorph(samples, sampleRate, options);
}

/**
 * Perform complete music analysis with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Complete analysis result
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  onProgress: ProgressCallback,
): AnalysisResult {
  const result = requireModule().analyzeWithProgress(samples, sampleRate, onProgress);
  return convertAnalysisResult(result);
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

export interface RhythmAnalysisResult {
  timeSignature: { numerator: number; denominator: number; confidence: number };
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  bpm: number;
  beatIntervals: Float32Array;
}

export interface DynamicsAnalysisResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
  /** Loudness curve timestamps (seconds), parallel to {@link loudnessRmsDb}. */
  loudnessTimes: Float32Array;
  /** Loudness curve RMS values (dB), parallel to {@link loudnessTimes}. */
  loudnessRmsDb: Float32Array;
}

/** Timbre metrics for one analysis window. Entries are ordered by time in `timbreOverTime`. */
export interface TimbreFrame {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

export interface TimbreAnalysisResult extends TimbreFrame {
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
  /** Time-varying timbre metrics, one entry per analysis window. */
  timbreOverTime: TimbreFrame[];
}

/**
 * Detailed BPM analysis (BPM, confidence, alternate candidates, autocorrelation,
 * tempogram). Matches the Node `analyzeBpm` / Python `analyze_bpm` surface.
 */
export function analyzeBpm(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeBpmOptions = {},
): BpmAnalysisResult {
  return requireModule().analyzeBpm(
    samples,
    sampleRate,
    options.bpmMin ?? 30.0,
    options.bpmMax ?? 300.0,
    options.startBpm ?? 120.0,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.maxCandidates ?? 5,
  );
}

/**
 * Detailed rhythm analysis (time signature, groove, syncopation, beat intervals).
 */
export function analyzeRhythm(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeRhythmOptions = {},
): RhythmAnalysisResult {
  return requireModule().analyzeRhythm(
    samples,
    sampleRate,
    options.bpmMin ?? 60.0,
    options.bpmMax ?? 200.0,
    options.startBpm ?? 120.0,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
  );
}

/**
 * Dynamics analysis (RMS, peak, crest factor, LRA, loudness curve).
 */
export function analyzeDynamics(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeDynamicsOptions = {},
): DynamicsAnalysisResult {
  return requireModule().analyzeDynamics(
    samples,
    sampleRate,
    options.windowSec ?? 0.4,
    options.hopLength ?? 512,
    options.compressionThreshold ?? 6.0,
  );
}

/**
 * Timbre analysis (brightness/warmth/density/roughness/complexity plus spectral
 * features and per-window timbre frames).
 */
export function analyzeTimbre(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeTimbreOptions = {},
): TimbreAnalysisResult {
  return requireModule().analyzeTimbre(
    samples,
    sampleRate,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.nMels ?? 128,
    options.nMfcc ?? 13,
    options.windowSec ?? 0.5,
  );
}

/**
 * Whether this WASM build was compiled with FFmpeg support. Mirrors Node /
 * Python `hasFfmpegSupport`. In the published WASM binding this currently
 * always returns `false` (FFmpeg is not bundled into the .wasm), but the API
 * exists so caller code can branch on capabilities portably.
 */
export function hasFfmpegSupport(): boolean {
  return requireModule().hasFfmpegSupport();
}
