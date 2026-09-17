/**
 * Region-based spectral editing: time x frequency rectangles applied over an
 * STFT and resynthesized.
 */

import type { EffectSamplesRequest } from './_effects_common.js';
import { addon } from './native.js';
import type { SpectralEditOptions, SpectralRegionOp } from './types.js';
import { assertInt32, assertSampleRate } from './validation.js';

export interface SpectralEditRequest extends EffectSamplesRequest, SpectralEditOptions {
  /**
   * Sample rate in Hz. Required: region frequency boundaries are mapped to STFT
   * bins using this rate, so a wrong/omitted value silently corrupts the edit
   * (unlike the other effects, this has no safe default).
   */
  sampleRate: number;
  ops?: SpectralRegionOp[];
}

/**
 * Region-based spectral editing: STFT -> per-op time x frequency bin/frame
 * masking -> iSTFT. A stateless mono transform whose output has the same length
 * and sample rate as the input.
 *
 * Each {@link SpectralRegionOp} in `ops` is a time x frequency rectangle applied
 * in array order (gain / attenuate / mute / heal). Passing an empty `ops` array
 * is the identity transform (the input is returned). Wraps the C
 * `sonare_spectral_edit`.
 *
 * @param samples - Mono input audio.
 * @param sampleRate - Sample rate in Hz (required; region frequency boundaries
 *   are mapped to STFT bins using this rate, so a wrong/omitted value silently
 *   corrupts the edit).
 * @param ops - Region ops applied in order.
 * @param options - Optional STFT + heal configuration.
 * @returns The edited audio (same length as `samples`).
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, ops, ...options } : samples;
  const {
    samples: input,
    sampleRate: requestSampleRate,
    ops: requestOps = [],
    ...requestOptions
  } = request;
  assertSampleRate('spectralEdit', requestSampleRate as number);
  // Each of the three is its own "0 => the documented default" on the C side,
  // and the addon's narrowing truncates onto that 0.
  for (const field of ['nFft', 'hopLength', 'healRadiusFrames'] as const) {
    const value = requestOptions[field];
    if (value !== undefined) {
      assertInt32('spectralEdit', value, field);
    }
  }
  return addon.spectralEdit(input, requestSampleRate as number, requestOps, requestOptions);
}
