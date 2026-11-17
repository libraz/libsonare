import { getSonareModule } from './module_state.js';
import { assertAudioSamples, assertSampleRate, requestOnly } from './validation.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Core - Resample
// ============================================================================

/** Resample request: two rates, so it is the one sample-taking request without `sampleRate`. */
export interface ResampleRequest {
  samples: Float32Array;
  /** Rate of `samples`, in Hz. */
  sourceSampleRate: number;
  /** Rate to resample to, in Hz. */
  targetSampleRate: number;
}

/**
 * Resample audio to a different sample rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sourceSampleRate - Source sample rate in Hz
 * @param targetSampleRate - Target sample rate in Hz
 * @returns Resampled audio
 * @throws TypeError for a missing or non-number rate, RangeError for a rate that is not an integer in the supported range.
 */
export function resample(request: ResampleRequest): Float32Array;
export function resample(
  samples: Float32Array,
  sourceSampleRate: number,
  targetSampleRate: number,
): Float32Array;
export function resample(...args: unknown[]): Float32Array {
  const first = args[0];
  let request: ResampleRequest;
  if (first instanceof Float32Array) {
    if (args.length > 3) {
      throw new TypeError('resample: expected (samples, sourceSampleRate, targetSampleRate)');
    }
    request = {
      samples: first,
      sourceSampleRate: args[1] as number,
      targetSampleRate: args[2] as number,
    };
  } else {
    request = requestOnly<ResampleRequest>('resample', args);
  }
  assertAudioSamples('resample', request.samples, request, 'samples');
  assertSampleRate('resample', request.sourceSampleRate, 'sourceSampleRate');
  assertSampleRate('resample', request.targetSampleRate, 'targetSampleRate');
  return requireModule().resample(
    request.samples,
    request.sourceSampleRate,
    request.targetSampleRate,
  );
}
