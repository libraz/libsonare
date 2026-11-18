/**
 * Region-based spectral editing: time x frequency rectangles applied over an
 * STFT and resynthesized.
 */

import { WINDOW_TYPES } from './internal/window_type.js';
import { addon } from './native.js';
import type { SpectralEditOptions, SpectralRegionOp } from './types.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertInt32,
  resolveSampleBound,
  sampleOneShot,
} from './validation.js';

/** Spectral edit request: the audio and rate plus {@link SpectralEditOptions}. */
export interface SpectralEditRequest extends SpectralEditOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /**
   * Sample rate in Hz. Required: region frequency boundaries are mapped to STFT
   * bins using this rate, so a wrong/omitted value silently corrupts the edit.
   */
  sampleRate: number;
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
 * Region-based spectral editing: STFT -> per-op time x frequency bin/frame
 * masking -> iSTFT. A stateless mono transform whose output has the same length
 * and sample rate as the input.
 *
 * Each {@link SpectralRegionOp} in `ops` is a time x frequency rectangle applied
 * in array order (gain / attenuate / mute / heal). Its time bounds are samples
 * (`startSample` / `endSample`) or seconds (`startSec` / `endSec`), one spelling
 * per bound; an omitted end is the end of the signal and a negative bound is
 * refused. An empty `ops` array returns a copy of the input. Wraps the C
 * `sonare_spectral_edit`.
 *
 * @param samples - Mono input audio.
 * @param sampleRate - Sample rate in Hz (required; region frequency boundaries
 *   are mapped to STFT bins using this rate).
 * @param options - `ops` (required, applied in order) plus the optional STFT and heal configuration.
 * @returns The edited audio (same length as `samples`).
 * @throws TypeError for a missing `sampleRate` or `ops`.
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  options: SpectralEditOptions,
): Float32Array;
export function spectralEdit(...args: unknown[]): Float32Array {
  const request = sampleOneShot<SpectralEditRequest>('spectralEdit', args);
  const { samples: input, sampleRate, ops, ...requestOptions } = request;
  assertAudioInput('spectralEdit', input, sampleRate);
  if (ops === undefined || ops === null) {
    throw new TypeError('spectralEdit: ops must be an array');
  }
  if (requestOptions.window !== undefined) {
    assertClosedToken('spectralEdit', requestOptions.window, WINDOW_TYPES, 'window');
  }
  // Each of the three is its own "0 => the documented default" on the C side,
  // and the addon's narrowing truncates onto that 0.
  for (const field of ['nFft', 'hopLength', 'healRadiusFrames'] as const) {
    const value = requestOptions[field];
    if (value !== undefined) {
      assertInt32('spectralEdit', value, field);
    }
  }
  const nativeOps = Array.isArray(ops)
    ? ops.map((op, index) =>
        op === null || typeof op !== 'object' ? op : resolveRegionTimes(op, index, sampleRate),
      )
    : ops;
  return addon.spectralEdit(input, sampleRate, nativeOps, requestOptions);
}
