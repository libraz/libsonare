import { addon } from './native.js';
import {
  assertAudioInput,
  assertSampleRate,
  requestObject,
  type ValidateOptions,
} from './validation.js';

/** Inputs for {@link resample}. */
export interface ResampleRequest extends ValidateOptions {
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
 *   after a request object; RangeError for a rate outside the supported range.
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
  assertAudioInput('resample', request.samples, request.sampleRate, request);
  assertSampleRate('resample', request.targetSampleRate, 'targetSampleRate');
  return addon.resample(request.samples, request.sampleRate, request.targetSampleRate);
}
