/**
 * Analysis and feature-extraction entry point for the smaller WASM binary.
 *
 * This entry intentionally omits mastering, mixing, realtime engines, Project,
 * and other native-handle APIs. Import the package root when those surfaces are
 * required.
 */

import { setSonareModule } from './module_state';
import type { SonareCapabilities } from './public_types';
import type { SonareModule } from './sonare.js';

export { ErrorCode, isSonareError, SonareError } from './errors';
export * from './feature_core';
// Several modules here also hold functions the analysis-only embind source set
// never registers, so those are re-exported by name rather than wholesale:
// `export *` published symbols that imported fine and then threw
// "is not a function" at call time, on a binary that never had them. Types stay
// wholesale — a type cannot fail to resolve. The lists are not maintained by
// hand: `analysis-entry.test.ts` compares every function this entry exports
// against the registrations of the module it loads and fails on a mismatch in
// either direction, so adding a name here that the analysis binary lacks, or
// omitting one it gained, is a test failure rather than a runtime surprise.
export type * from './feature_inverse';
export { griffinLim, melToAudio, melToStft, mfccToAudio, mfccToMel } from './feature_inverse';
export * from './feature_music';
export * from './feature_pitch';
export type * from './feature_resample';
export type * from './feature_spectral';
export {
  ebur128LoudnessRange,
  estimateTuning,
  lufsInterleaved,
  lufsSeriesInterleaved,
  pitchTuning,
  polyFeatures,
  rmsEnergy,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  spectralBandwidth,
  spectralCentroid,
  spectralContrast,
  spectralFlatness,
  spectralFlux,
  spectralRolloff,
  zeroCrossingRate,
  zeroCrossings,
} from './feature_spectral';
export type * from './feature_spectrogram';
export {
  bassChroma,
  chroma,
  chromaCens,
  chromaCqt,
  melDelta,
  melSpectrogram,
  mfcc,
  reassignedSpectrogram,
  stft,
  stftDb,
} from './feature_spectrogram';
export * from './metering';
export * from './public_types';
export type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  AnalyzeWithProgressRequest,
  BpmAnalysisResult,
  BpmCandidate,
  ChordFunctionalAnalysisRequest,
  DetectAcousticRequest,
  DetectChordsRequest,
  DetectKeyRequest,
  DynamicsAnalysisResult,
  DynamicsResult,
  EstimateMeterRequest,
  MusicAnalyzeOptions,
  MusicAnalyzeRequest,
  RhythmAnalysisResult,
  SamplesRequest,
  TimbreAnalysisResult,
  TimbreFrame,
} from './quick_analysis';
export {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectAcoustic,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  estimateMeter,
  hasFfmpegSupport,
} from './quick_analysis';

let module: SonareModule | null = null;
let initPromise: Promise<void> | null = null;

/** Initialize the analysis-only WASM module. */
export async function init(options?: {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
  moduleFactory?: (options?: {
    locateFile?: (path: string, prefix: string) => string;
    wasmBinary?: ArrayBuffer | Uint8Array;
  }) => Promise<SonareModule>;
}): Promise<void> {
  if (module) {
    return;
  }
  if (initPromise) {
    return initPromise;
  }
  initPromise = (async () => {
    try {
      const createModule = options?.moduleFactory ?? (await import('./sonare-analysis.js')).default;
      module = await createModule(options);
      setSonareModule(module);
    } catch (error) {
      initPromise = null;
      throw error;
    }
  })();
  return initPromise;
}

/** Whether this analysis entry has loaded its WASM module. */
export function isInitialized(): boolean {
  return module !== null;
}

/** Version reported by the loaded analysis WASM module. */
export function version(): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.version();
}

/** Build capabilities for the loaded analysis-only module. */
export function capabilities(): SonareCapabilities {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.capabilities();
}

/** Packed C-ABI version for compatibility checks. */
export function abiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.abiVersion();
}

/** Realtime command-queue ABI version shared with the full entry. */
export function engineAbiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.engineAbiVersion();
}

/** Voice-changer ABI version retained for cross-entry compatibility checks. */
export function voiceChangerAbiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.voiceChangerAbiVersion();
}
