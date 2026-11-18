import { addon } from './native.js';
import { assertAudioSamples, assertInt32, requestObject } from './validation.js';

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
 * Resample mono audio to another sample rate.
 *
 * @throws TypeError for a wrong-typed argument, an omitted rate, or an argument
 *   after a request object; RangeError for a rate that is not an integer or lies outside the
 *   supported range.
 */
export function resample(request: ResampleRequest): Float32Array;
export function resample(
  samples: Float32Array,
  sampleRate: number,
  targetSampleRate: number,
): Float32Array;
export function resample(...args: unknown[]): Float32Array {
  const [first, sampleRate, targetSampleRate] = args;
  let request: ResampleRequest;
  if (first instanceof Float32Array) {
    if (args.length > 3) {
      throw new TypeError('resample: expected (samples, sampleRate, targetSampleRate)');
    }
    request = { samples: first, sampleRate, targetSampleRate } as ResampleRequest;
  } else {
    if (args.length > 1) {
      throw new TypeError('resample: unexpected argument after the request object');
    }
    request = requestObject('resample', first as ResampleRequest);
  }
  // Type and emptiness only: rate range and non-finite samples are the core's to refuse.
  assertAudioSamples('resample', request.samples, { scanFinite: false });
  assertInt32('resample', request.sampleRate, 'sampleRate');
  assertInt32('resample', request.targetSampleRate, 'targetSampleRate');
  return addon.resample(request.samples, request.sampleRate, request.targetSampleRate);
}
