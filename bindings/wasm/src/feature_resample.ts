import { getSonareModule } from './module_state.js';
import { assertAudioSamples, assertInt32, requestOnly } from './validation.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Core - Resample
// ============================================================================

/** Inputs for {@link resample}. */
export interface ResampleRequest {
  /** Mono audio to resample. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Sample rate to resample to, in Hz. */
  targetSampleRate: number;
}

/**
 * Resample audio to a different sample rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate of `samples` in Hz
 * @param targetSampleRate - Sample rate to resample to, in Hz
 * @returns Resampled audio
 * @throws TypeError for a missing or non-number rate, RangeError for a rate that is not an integer or lies outside the supported range.
 */
export function resample(request: ResampleRequest): Float32Array;
export function resample(
  samples: Float32Array,
  sampleRate: number,
  targetSampleRate: number,
): Float32Array;
export function resample(...args: unknown[]): Float32Array {
  const first = args[0];
  let request: ResampleRequest;
  if (first instanceof Float32Array) {
    if (args.length > 3) {
      throw new TypeError('resample: expected (samples, sampleRate, targetSampleRate)');
    }
    request = {
      samples: first,
      sampleRate: args[1] as number,
      targetSampleRate: args[2] as number,
    };
  } else {
    request = requestOnly<ResampleRequest>('resample', args);
  }
  // Type and emptiness only: rate range and non-finite samples are the core's to refuse.
  assertAudioSamples('resample', request.samples, { validate: false });
  assertInt32('resample', request.sampleRate, 'sampleRate');
  assertInt32('resample', request.targetSampleRate, 'targetSampleRate');
  return requireModule().resample(request.samples, request.sampleRate, request.targetSampleRate);
}
