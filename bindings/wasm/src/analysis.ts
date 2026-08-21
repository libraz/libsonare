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
export * from './feature_music';
export * from './feature_pitch';
export * from './feature_resample';
export * from './feature_spectral';
export * from './feature_spectrogram';
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
