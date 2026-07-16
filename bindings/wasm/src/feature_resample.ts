import { getSonareModule } from './module_state';

function requireModule() {
  return getSonareModule();
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
export interface ResampleRequest {
  samples: Float32Array;
  srcSr: number;
  targetSr: number;
}

export function resample(request: ResampleRequest): Float32Array;
export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array;
export function resample(
  samples: Float32Array | ResampleRequest,
  srcSr?: number,
  targetSr?: number,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return resample(samples.samples, samples.srcSr, samples.targetSr);
  }
  return requireModule().resample(samples, srcSr as number, targetSr as number);
}
