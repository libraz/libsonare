/**
 * Region-based spectral editing: time x frequency rectangles applied over an
 * STFT and resynthesized.
 */

import { mergeRespelled, type Respelled, refuse, refuseType, withEntry } from './internal/entry.js';
import { refuseZeroSentinel } from './internal/metering_native.js';
import { withRespelled } from './internal/respell.js';
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

/** The C field reads `high_hz <= 0` as Nyquist, so a given edge must be a real frequency. */
function assertRegionHighHz(highHz: unknown, at: string): void {
  if (typeof highHz === 'number' && !(Number.isFinite(highHz) && highHz > 0)) {
    throw refuse(`${at}.highHz must be finite and greater than 0; omit it for Nyquist`);
  }
}

function resolveRegionTimes(
  { startSec, endSec, ...op }: SpectralRegionOp,
  index: number,
  sampleRate: number,
): { op: SpectralRegionOp; respelled: Respelled } {
  const at = `ops[${index}]`;
  assertRegionHighHz(op.highHz, at);
  const start = resolveSampleBound(
    op.startSample,
    startSec,
    sampleRate,
    `${at}.startSample`,
    `${at}.startSec`,
  );
  const end = resolveSampleBound(
    op.endSample,
    endSec,
    sampleRate,
    `${at}.endSample`,
    `${at}.endSec`,
  );
  return {
    op: { ...op, startSample: start.value, endSample: end.value },
    respelled: mergeRespelled(start.respelled, end.respelled),
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
  return withEntry('spectralEdit', () => {
    const request = sampleOneShot<SpectralEditRequest>('spectralEdit', args);
    const { samples: input, sampleRate, ops, ...requestOptions } = request;
    assertAudioInput(input, sampleRate);
    if (ops === undefined || ops === null) {
      throw refuseType('ops must be an array');
    }
    if (requestOptions.window !== undefined) {
      assertClosedToken(requestOptions.window, WINDOW_TYPES, 'window');
    }
    // Each of the three is its own "0 => the documented default" on the C side, so a given 0
    // is refused, and the addon's narrowing would truncate a fraction onto that 0.
    refuseZeroSentinel(requestOptions, ['nFft', 'hopLength', 'healRadiusFrames']);
    for (const field of ['nFft', 'hopLength', 'healRadiusFrames'] as const) {
      const value = requestOptions[field];
      if (value !== undefined) {
        assertInt32(value, field);
      }
    }
    const respellings: Respelled[] = [];
    const nativeOps = Array.isArray(ops)
      ? ops.map((op, index) => {
          if (op === null || typeof op !== 'object') {
            return op;
          }
          const resolved = resolveRegionTimes(op, index, sampleRate);
          respellings.push(resolved.respelled);
          return resolved.op;
        })
      : ops;
    return withRespelled(mergeRespelled(...respellings), () =>
      addon.spectralEdit(input, sampleRate, nativeOps, requestOptions),
    );
  });
}
