/**
 * Harmonic/percussive separation: the masked split and the two shortcuts
 * that return a single component.
 */

import { refuseType, withEntry } from './internal/entry.js';
import { resolveEffectFftOptions } from './internal/fft_options.js';
import { addon } from './native.js';
import type { HpssResult } from './types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, assertHpssKernels, sampleOneShot } from './validation.js';

function resolveBoolean(value: unknown, name: string): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw refuseType(`${name} must be a boolean`);
  }
  return value;
}

/** Request for {@link hpss}. */
export interface HpssRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Horizontal median filter size, in STFT frames: a positive odd integer at
   * most 524287. Default 31. The ceiling is 524288 and an even kernel is
   * refused, so 524287 is the largest legal value.
   */
  kernelHarmonic?: number;
  /** Vertical median filter size, in STFT bins, under the same rule. Default 31. */
  kernelPercussive?: number;
  /** FFT size in samples. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Threshold the masks into binary ones instead of the soft split. Default `false`. */
  hardMask?: boolean;
  /**
   * Also return `residual`, the band where neither component dominates. Default
   * `false`, which leaves `residual` absent and the other two outputs unchanged.
   */
  withResidual?: boolean;
}

/** Request for {@link harmonic}. */
export interface HarmonicRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request for {@link percussive}. */
export interface PercussiveRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/**
 * Harmonic-percussive source separation.
 *
 * With `withResidual: true` the result also carries `residual`, and the three
 * outputs add back up to the input. `residual` is silent under the default soft
 * mask, whose two masks sum to one; pass `hardMask: true` for a residual that
 * holds signal — its thresholded masks leave the band where neither component
 * dominates, measured at 3 % of the input energy on a voice-plus-kick signal.
 *
 * @example
 * ```ts
 * const { harmonic, percussive } = hpss({ samples, sampleRate });
 * const hard = hpss({ samples, sampleRate, hardMask: true, withResidual: true });
 * // hard.residual carries what neither component claimed
 * ```
 *
 * @throws {TypeError} when `sampleRate` is missing or `hardMask` / `withResidual` is not a boolean
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, or a
 *   kernel that is not an integer within the signed 32-bit range or one the core
 *   rejects as even, non-positive or above its ceiling
 */
export function hpss(request: HpssRequest): HpssResult;
export function hpss(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HpssRequest>,
): HpssResult;
export function hpss(...args: unknown[]): HpssResult {
  return withEntry('hpss', () => {
    const r = sampleOneShot<HpssRequest>('hpss', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fftOptions = resolveEffectFftOptions(r.nFft, r.hopLength);
    const hardMask = resolveBoolean(r.hardMask, 'hardMask');
    const withResidual = resolveBoolean(r.withResidual, 'withResidual');
    const kernelHarmonic = r.kernelHarmonic ?? 31;
    const kernelPercussive = r.kernelPercussive ?? 31;
    assertHpssKernels(kernelHarmonic, kernelPercussive);
    return addon.hpss(
      r.samples,
      r.sampleRate,
      kernelHarmonic,
      kernelPercussive,
      fftOptions.nFft,
      fftOptions.hopLength,
      hardMask,
      withResidual,
    );
  });
}

/**
 * Extract the harmonic component of `samples`.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 */
export function harmonic(request: HarmonicRequest): Float32Array;
export function harmonic(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HarmonicRequest>,
): Float32Array;
export function harmonic(...args: unknown[]): Float32Array {
  return withEntry('harmonic', () => {
    const r = sampleOneShot<HarmonicRequest>('harmonic', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.harmonic(r.samples, r.sampleRate);
  });
}

/**
 * Extract the percussive component of `samples`.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 */
export function percussive(request: PercussiveRequest): Float32Array;
export function percussive(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PercussiveRequest>,
): Float32Array;
export function percussive(...args: unknown[]): Float32Array {
  return withEntry('percussive', () => {
    const r = sampleOneShot<PercussiveRequest>('percussive', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.percussive(r.samples, r.sampleRate);
  });
}
