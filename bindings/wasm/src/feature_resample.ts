import { refuseType, withEntry } from './internal/entry.js';
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
 * @param request - Mono `samples`, `sampleRate` of `samples` in Hz and `targetSampleRate` to
 *   resample to, in Hz; or pass `(samples, sampleRate, targetSampleRate)`.
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
  return withEntry('resample', () => {
    const first = args[0];
    let request: ResampleRequest;
    if (first instanceof Float32Array) {
      if (args.length > 3) {
        throw refuseType('expected (samples, sampleRate, targetSampleRate)');
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
    assertAudioSamples(request.samples, { scanFinite: false });
    assertInt32(request.sampleRate, 'sampleRate');
    assertInt32(request.targetSampleRate, 'targetSampleRate');
    return requireModule().resample(request.samples, request.sampleRate, request.targetSampleRate);
  });
}
