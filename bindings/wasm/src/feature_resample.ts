import { refuseType, withEntry } from './internal/entry.js';
import { withHeapNeedWhen } from './internal/memory_budget.js';
import { getSonareModule } from './module_state.js';
import {
  assertAudioSamples,
  assertInt32,
  MAX_AUDIO_SAMPLE_RATE,
  MIN_AUDIO_SAMPLE_RATE,
  requestOnly,
} from './validation.js';

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
    // Type only: an empty buffer converts to an empty one, and rate range and non-finite
    // samples are the core's to refuse.
    assertAudioSamples(request.samples, { scanFinite: false }, 'samples', true);
    assertInt32(request.sampleRate, 'sampleRate');
    assertInt32(request.targetSampleRate, 'targetSampleRate');
    const { samples, sampleRate, targetSampleRate } = request;
    const inRange = (rate: number) =>
      rate >= MIN_AUDIO_SAMPLE_RATE && rate <= MAX_AUDIO_SAMPLE_RATE;
    // An out-of-range rate is the native reader's refusal, not a memory one.
    return withHeapNeedWhen(
      inRange(sampleRate) && inRange(targetSampleRate),
      {
        subject: 'targetSampleRate',
        size: targetSampleRate,
        // The input copy and the converted output, whose length rounds down.
        bytes: (n) => 4 * samples.length + 4 * Math.floor((samples.length * n) / sampleRate),
        monotone: true,
      },
      () => getSonareModule().resample(samples, sampleRate, targetSampleRate),
    );
  });
}
