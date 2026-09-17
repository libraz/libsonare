/**
 * Harmonic/percussive separation: the masked split and the two shortcuts
 * that return a single component.
 */

import { resolveFftOptions } from './_fft_options';
import { getSonareModule } from './module_state';
import type { HpssResult } from './public_types';
import type { ValidateOptions } from './validation';
import { assertHpssKernels, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

function resolveHardMask(value: unknown, fnName: string): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

/** Canonical request form for HPSS. */
export interface HpssRequest {
  samples: Float32Array;
  sampleRate?: number;
  /**
   * Horizontal median filter size, in STFT frames: a positive odd integer at
   * most 524287. Default 31. The ceiling is 524288 and an even kernel is
   * refused, so 524287 is the largest legal value.
   */
  kernelHarmonic?: number;
  /** Vertical median filter size, in STFT bins, under the same rule. Default 31. */
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
}

export interface HarmonicRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface PercussiveRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param kernelHarmonic - Horizontal median filter size in STFT frames; a
 *   positive odd integer at most 524287 (default: 31)
 * @param kernelPercussive - Vertical median filter size in STFT bins, under the
 *   same rule (default: 31)
 * @returns Separated harmonic and percussive components
 * @throws SonareError (`InvalidParameter`) on a kernel that is not an integer
 *   within the signed 32-bit range, or one the core rejects as even,
 *   non-positive or above its ceiling
 */
export function hpss(request: HpssRequest): HpssResult;
export function hpss(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult;
export function hpss(
  samples: Float32Array | HpssRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive, nFft, hopLength, hardMask }
      : samples;
  const fftOptions = resolveFftOptions('hpss', request.nFft, request.hopLength);
  const resolvedHardMask = resolveHardMask(request.hardMask, 'hpss');
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpss', resolvedKernelHarmonic, resolvedKernelPercussive);
  return requireModule().hpssEx(
    request.samples,
    request.sampleRate ?? 22050,
    resolvedKernelHarmonic,
    resolvedKernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(request: HarmonicRequest): Float32Array;
export function harmonic(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function harmonic(
  samples: Float32Array | HarmonicRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('harmonic', request.samples, request.validate !== false);
  return requireModule().harmonic(request.samples, request.sampleRate ?? 22050);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(request: PercussiveRequest): Float32Array;
export function percussive(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function percussive(
  samples: Float32Array | PercussiveRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('percussive', request.samples, request.validate !== false);
  return requireModule().percussive(request.samples, request.sampleRate ?? 22050);
}
