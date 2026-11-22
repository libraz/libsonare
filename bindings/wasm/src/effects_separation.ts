/**
 * Harmonic/percussive separation: the masked split and the two shortcuts
 * that return a single component.
 */

import { withEntry } from './internal/entry.js';
import { resolveEffectFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { HpssResult } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertHpssKernels,
  optionalBoolean,
  sampleOneShot,
} from './validation.js';

/** Canonical request form for HPSS. */
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
   * Also return `residual`, what neither component claimed. Default `false`.
   * Silent under the soft mask, so combine with `hardMask: true` for a residual
   * that holds signal.
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
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * With `withResidual: true` the result also carries `residual`, the part of the
 * input neither component claimed (input - harmonic - percussive), so the three
 * signals add back up to the input. Under the default soft mask the two masks sum
 * to one and the residual is silent; pass `hardMask: true` for a residual that
 * holds signal, since the thresholded masks leave out the band where neither
 * component dominates (measured at 3 % of the input energy on a voice-plus-kick
 * signal).
 *
 * @example
 * ```ts
 * const { harmonic, percussive } = hpss({ samples, sampleRate });
 * const split = hpss({ samples, sampleRate, hardMask: true, withResidual: true });
 * // split.residual carries what neither component claimed
 * ```
 *
 * @returns Separated harmonic and percussive components, plus `residual` when requested
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
    const hardMask = optionalBoolean(r.hardMask, 'hardMask', false);
    const withResidual = optionalBoolean(r.withResidual, 'withResidual', false);
    const kernelHarmonic = r.kernelHarmonic ?? 31;
    const kernelPercussive = r.kernelPercussive ?? 31;
    assertHpssKernels(kernelHarmonic, kernelPercussive);
    return getSonareModule().hpss(
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
 * Extract the harmonic component from audio.
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
    return getSonareModule().harmonic(r.samples, r.sampleRate);
  });
}

/**
 * Extract the percussive component from audio.
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
    return getSonareModule().percussive(r.samples, r.sampleRate);
  });
}
