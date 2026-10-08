/**
 * Region-based spectral editing: time x frequency rectangles applied over an
 * STFT and resynthesized.
 */

import { getSonareModule } from './module_state.js';
import type { SpectralEditOptions, SpectralRegionOp } from './public_types.js';
import type { ValidateOptions } from './validation.js';
import { assertSampleRate, assertSamples, resolveSampleBound } from './validation.js';

function requireModule() {
  return getSonareModule();
}

export interface SpectralEditRequest extends SpectralEditOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate: number;
  ops?: SpectralRegionOp[];
}

function resolveRegionTimes(
  { startSec, endSec, ...op }: SpectralRegionOp,
  index: number,
  sampleRate: number,
): SpectralRegionOp {
  const at = `ops[${index}]`;
  return {
    ...op,
    startSample: resolveSampleBound(
      'spectralEdit',
      op.startSample,
      startSec,
      sampleRate,
      `${at}.startSample`,
      `${at}.startSec`,
    ),
    endSample: resolveSampleBound(
      'spectralEdit',
      op.endSample,
      endSec,
      sampleRate,
      `${at}.endSample`,
      `${at}.endSec`,
    ),
  };
}

/**
 * Apply region-based spectral edits (gain/attenuate/mute/heal) to mono audio.
 *
 * Each op is a time x frequency rectangle applied in array order over a single
 * STFT buffer, so a later op observes the result of earlier ops. An op's time
 * bounds are samples (`startSample` / `endSample`) or seconds (`startSec` /
 * `endSec`), one spelling per bound; an omitted end is the end of the signal and
 * a negative bound is refused. The output has
 * the same length and sample rate as the input; an empty `ops` list is an
 * identity transform (within the iSTFT's own tolerance).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param ops - Region edit ops applied in order ({@link SpectralRegionOp})
 * @param options - STFT + heal configuration ({@link SpectralEditOptions})
 * @returns Edited audio
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions & ValidateOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions & ValidateOptions = {},
): Float32Array {
  const request: SpectralEditRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ops, ...options }
      : samples;
  assertSamples('spectralEdit', request.samples, request.validate !== false);
  assertSampleRate('spectralEdit', request.sampleRate);
  const requestOps = request.ops ?? [];
  const nativeOps = Array.isArray(requestOps)
    ? requestOps.map((op, index) =>
        op === null || typeof op !== 'object'
          ? op
          : resolveRegionTimes(op, index, request.sampleRate),
      )
    : requestOps;
  return requireModule().spectralEdit(
    request.samples,
    request.sampleRate,
    nativeOps,
    request as unknown as Record<string, unknown>,
  );
}
