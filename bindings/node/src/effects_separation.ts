/**
 * Harmonic/percussive separation: the masked split and the two shortcuts
 * that return a single component.
 */

import type { EffectSamplesRequest } from './_effects_common.js';
import { resolveFftOptions } from './_fft_options.js';
import { addon } from './native.js';
import type { HpssResult } from './types.js';
import { assertHpssKernels } from './validation.js';

function resolveHardMask(fnName: string, hardMask: unknown): boolean {
  if (hardMask === undefined) {
    return false;
  }
  if (typeof hardMask !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return hardMask;
}

export interface HpssRequest extends EffectSamplesRequest {
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
  const resolvedHardMask = resolveHardMask('hpss', request.hardMask);
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpss', resolvedKernelHarmonic, resolvedKernelPercussive);
  return addon.hpss(
    request.samples,
    request.sampleRate ?? 22050,
    resolvedKernelHarmonic,
    resolvedKernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}

export function harmonic(request: EffectSamplesRequest): Float32Array;
export function harmonic(samples: Float32Array, sampleRate?: number): Float32Array;
export function harmonic(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.harmonic(request.samples, request.sampleRate ?? 22050);
}

export function percussive(request: EffectSamplesRequest): Float32Array;
export function percussive(samples: Float32Array, sampleRate?: number): Float32Array;
export function percussive(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.percussive(request.samples, request.sampleRate ?? 22050);
}
