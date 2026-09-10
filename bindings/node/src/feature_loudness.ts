import type { FeatureSamplesRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { LufsResult } from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

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
  const request = samples instanceof Float32Array ? { samples, channels, sampleRate } : samples;
  return addon.lufsInterleaved(request.samples, request.channels, request.sampleRate ?? 22050);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.ebur128LoudnessRange(request.samples, request.sampleRate ?? 22050);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('lufs', request.samples, request.validate !== false);
  return addon.lufs(request.samples, request.sampleRate ?? 22050);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('momentaryLufs', request.samples, request.validate !== false);
  return addon.momentaryLufs(request.samples, request.sampleRate ?? 22050);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('shortTermLufs', request.samples, request.validate !== false);
  return addon.shortTermLufs(request.samples, request.sampleRate ?? 22050);
}
