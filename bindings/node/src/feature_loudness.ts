import type { FeatureSamplesRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { LufsResult, LufsSeriesResult } from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertAudioInput, requestObject } from './validation.js';

/** Input for LUFS feature functions, including optional input validation control. */
export interface LufsRequest extends FeatureSamplesRequest, ValidateOptions {}

/**
 * Channel-weighted multichannel loudness + LRA (BS.1770 / EBU R128) from an
 * interleaved buffer of `frames * channels` samples. The per-channel frame
 * count is derived from the buffer length and `channels`.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 */
export function lufsInterleaved(request: FeatureSamplesRequest & { channels: number }): LufsResult;
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
): LufsResult;
export function lufsInterleaved(
  samples: Float32Array | (FeatureSamplesRequest & { channels: number }),
  channels = 0,
  sampleRate = 22050,
): LufsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, channels, sampleRate }
      : requestObject('lufsInterleaved', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('lufsInterleaved', request.samples, resolvedSampleRate, request);
  return addon.lufsInterleaved(request.samples, request.channels, resolvedSampleRate);
}

/**
 * Per-block momentary (400 ms) and short-term (3 s) LUFS series for an
 * interleaved buffer of `frames * channels` samples, measured with ITU-R
 * BS.1770-4 channel summing. The per-channel frame count is derived from the
 * buffer length and `channels`.
 *
 * This is not recoverable from {@link momentaryLufs} / {@link shortTermLufs}:
 * those measure one channel each, and the standard sums the K-weighted
 * per-channel block energies rather than mixing per-channel loudness in dB. Both
 * series come out of one K-weighting pass. For `channels === 1` they match the
 * mono meters element for element.
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
export function lufsSeriesInterleaved(
  request: FeatureSamplesRequest & { channels: number },
): LufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
): LufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array | (FeatureSamplesRequest & { channels: number }),
  channels = 0,
  sampleRate = 22050,
): LufsSeriesResult {
  const request =
    samples instanceof Float32Array
      ? { samples, channels, sampleRate }
      : requestObject('lufsSeriesInterleaved', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('lufsSeriesInterleaved', request.samples, resolvedSampleRate, request);
  return addon.lufsSeriesInterleaved(request.samples, request.channels, resolvedSampleRate);
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard and K-weighting is
 * sample-rate dependent.
 */
export function ebur128LoudnessRange(request: FeatureSamplesRequest): number;
export function ebur128LoudnessRange(samples: Float32Array, sampleRate?: number): number;
export function ebur128LoudnessRange(
  samples: Float32Array | FeatureSamplesRequest,
  sampleRate = 22050,
): number {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate }
      : requestObject('ebur128LoudnessRange', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('ebur128LoudnessRange', request.samples, resolvedSampleRate, request);
  return addon.ebur128LoudnessRange(request.samples, resolvedSampleRate);
}

/**
 * Integrated/momentary/short-term LUFS + loudness range. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard for audio, and
 * K-weighting is sample-rate dependent, so a wrong rate yields wrong loudness.
 */
export function lufs(request: LufsRequest): LufsResult;
export function lufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): LufsResult;
export function lufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): LufsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('lufs', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('lufs', request.samples, resolvedSampleRate, request);
  return addon.lufs(request.samples, resolvedSampleRate);
}

/**
 * Per-block momentary LUFS series. Pass the buffer's actual `sampleRate`: the
 * default (22050) is non-standard and K-weighting is sample-rate dependent.
 */
export function momentaryLufs(request: LufsRequest): Float32Array;
export function momentaryLufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function momentaryLufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('momentaryLufs', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('momentaryLufs', request.samples, resolvedSampleRate, request);
  return addon.momentaryLufs(request.samples, resolvedSampleRate);
}

/**
 * Per-block short-term LUFS series. Pass the buffer's actual `sampleRate`: the
 * default (22050) is non-standard and K-weighting is sample-rate dependent.
 */
export function shortTermLufs(request: LufsRequest): Float32Array;
export function shortTermLufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function shortTermLufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('shortTermLufs', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('shortTermLufs', request.samples, resolvedSampleRate, request);
  return addon.shortTermLufs(request.samples, resolvedSampleRate);
}
