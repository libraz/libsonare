/**
 * Programme loudness over interleaved audio, as the broadcast standards define
 * it.
 */

import { getSonareModule } from './module_state';
import type { WasmLufsResult, WasmLufsSeriesResult } from './sonare.js';
import type { ValidateOptions } from './validation';
import { assertInterleavedSamples, assertSampleRate } from './validation';

function requireModule() {
  return getSonareModule();
}

export interface LufsInterleavedRequest extends ValidateOptions {
  samples: Float32Array;
  channels: number;
  sampleRate?: number;
}

export interface LufsSeriesInterleavedRequest extends ValidateOptions {
  samples: Float32Array;
  channels: number;
  sampleRate?: number;
}

export interface Ebur128LoudnessRangeRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/**
 * Channel-weighted multichannel integrated loudness + LRA (ITU-R BS.1770 /
 * EBU R128) from an interleaved buffer of `frames * channels` samples. The
 * per-channel frame count is derived from the buffer length and `channels`.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 */
export function lufsInterleaved(request: LufsInterleavedRequest): WasmLufsResult;
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
  options?: ValidateOptions,
): WasmLufsResult;
export function lufsInterleaved(
  samples: Float32Array | LufsInterleavedRequest,
  channels = 0,
  sampleRate = 22050,
  options: ValidateOptions = {},
): WasmLufsResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return lufsInterleaved(r.samples, r.channels, r.sampleRate, r);
  }
  assertSampleRate('lufsInterleaved', sampleRate);
  assertInterleavedSamples('lufsInterleaved', samples, channels, options.validate !== false);
  return requireModule().lufsInterleaved(samples, channels, sampleRate);
}

/**
 * Per-block momentary (400 ms) and short-term (3 s) LUFS series for an
 * interleaved buffer of `frames * channels` samples, measured with ITU-R
 * BS.1770-4 channel summing. The per-channel frame count is derived from the
 * buffer length and `channels`.
 *
 * This is not recoverable from `momentaryLufs` / `shortTermLufs`: those measure
 * one channel each, and the standard sums the K-weighted per-channel block
 * energies rather than mixing per-channel loudness in dB. Both series come out
 * of one K-weighting pass. For `channels === 1` they match the mono meters
 * element for element.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 *
 * @example
 * ```ts
 * const { momentary, shortTerm } = lufsSeriesInterleaved({
 *   samples: interleavedStereo,
 *   channels: 2,
 *   sampleRate: 48000,
 * });
 * ```
 */
export function lufsSeriesInterleaved(request: LufsSeriesInterleavedRequest): WasmLufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
  options?: ValidateOptions,
): WasmLufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array | LufsSeriesInterleavedRequest,
  channels = 0,
  sampleRate = 22050,
  options: ValidateOptions = {},
): WasmLufsSeriesResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return lufsSeriesInterleaved(r.samples, r.channels, r.sampleRate, r);
  }
  assertSampleRate('lufsSeriesInterleaved', sampleRate);
  assertInterleavedSamples('lufsSeriesInterleaved', samples, channels, options.validate !== false);
  return requireModule().lufsSeriesInterleaved(samples, channels, sampleRate);
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard and K-weighting is
 * sample-rate dependent.
 */
export function ebur128LoudnessRange(request: Ebur128LoudnessRangeRequest): number;
export function ebur128LoudnessRange(samples: Float32Array, sampleRate?: number): number;
export function ebur128LoudnessRange(
  samples: Float32Array | Ebur128LoudnessRangeRequest,
  sampleRate = 22050,
): number {
  if (!(samples instanceof Float32Array)) {
    return ebur128LoudnessRange(samples.samples, samples.sampleRate);
  }
  return requireModule().ebur128LoudnessRange(samples, sampleRate);
}
