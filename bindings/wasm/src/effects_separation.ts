/**
 * Harmonic/percussive separation: the masked split and the two shortcuts
 * that return a single component.
 */

import { resolveEffectFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { HpssResult } from './public_types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertHpssKernels,
  optionalBoolean,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Canonical request form for HPSS. */
export interface HpssRequest extends ValidateOptions {
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
}

/** Request for {@link harmonic}. */
export interface HarmonicRequest extends ValidateOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request for {@link percussive}. */
export interface PercussiveRequest extends ValidateOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @returns Separated harmonic and percussive components
 * @throws {TypeError} when `sampleRate` is missing or `hardMask` is not a boolean
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
  const r = sampleOneShot<HpssRequest>('hpss', args);
  assertAudioInput('hpss', r.samples, r.sampleRate, r);
  const fftOptions = resolveEffectFftOptions('hpss', r.nFft, r.hopLength);
  const hardMask = optionalBoolean('hpss', r.hardMask, 'hardMask', false);
  const kernelHarmonic = r.kernelHarmonic ?? 31;
  const kernelPercussive = r.kernelPercussive ?? 31;
  assertHpssKernels('hpss', kernelHarmonic, kernelPercussive);
  return requireModule().hpssEx(
    r.samples,
    r.sampleRate,
    kernelHarmonic,
    kernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    hardMask,
  );
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
  const r = sampleOneShot<HarmonicRequest>('harmonic', args);
  assertAudioInput('harmonic', r.samples, r.sampleRate, r);
  return requireModule().harmonic(r.samples, r.sampleRate);
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
  const r = sampleOneShot<PercussiveRequest>('percussive', args);
  assertAudioInput('percussive', r.samples, r.sampleRate, r);
  return requireModule().percussive(r.samples, r.sampleRate);
}
