/**
 * Region-based spectral editing: time x frequency rectangles applied over an
 * STFT and resynthesized.
 */

import { refuse, refuseType, withEntry } from './internal/entry.js';
import { refuseZeroSentinel } from './internal/metering_native.js';
import { windowOrdinal } from './internal/window_type.js';
import { getSonareModule } from './module_state.js';
import type { SpectralEditOptions, SpectralRegionOp } from './public_types.js';
import { assertAudioInput, resolveSampleBound, sampleOneShot } from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Spectral edit request: the audio and rate (Hz, required) plus {@link SpectralEditOptions}. */
export interface SpectralEditRequest extends SpectralEditOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required: region frequency boundaries are mapped to STFT bins with this rate. */
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
): SpectralRegionOp {
  const at = `ops[${index}]`;
  assertRegionHighHz(op.highHz, at);
  return {
    ...op,
    startSample: resolveSampleBound(
      op.startSample,
      startSec,
      sampleRate,
      `${at}.startSample`,
      `${at}.startSec`,
    ),
    endSample: resolveSampleBound(
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
 * a negative bound is refused. The output has the same length and sample rate as
 * the input; an empty `ops` list returns a copy of the input.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - `ops` (required, {@link SpectralRegionOp}) plus the STFT and heal configuration
 * @returns Edited audio
 * @throws TypeError for a missing `sampleRate` or `ops`
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
    assertAudioInput(request.samples, request.sampleRate, request);
    const requestOps = request.ops;
    if (requestOps === null || typeof requestOps !== 'object') {
      throw refuseType('ops must be an array');
    }
    if (request.window !== undefined) {
      windowOrdinal(request.window);
    }
    // Each of the three is its own "0 => the documented default" on the C side.
    refuseZeroSentinel(request, ['nFft', 'hopLength', 'healRadiusFrames']);
    // A non-Array array-like goes to the native reader, which bounds its length.
    const nativeOps = Array.isArray(requestOps)
      ? requestOps.map((op, index) => {
          if (op === null || typeof op !== 'object') {
            throw refuseType(`ops[${index}] must be an object`);
          }
          return resolveRegionTimes(op, index, request.sampleRate);
        })
      : (requestOps as SpectralRegionOp[]);
    return requireModule().spectralEdit(
      request.samples,
      request.sampleRate,
      nativeOps,
      request as unknown as Record<string, unknown>,
    );
  });
}
