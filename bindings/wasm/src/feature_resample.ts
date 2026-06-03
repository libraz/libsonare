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
export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  return requireModule().resample(samples, srcSr, targetSr);
}
