import { addon } from './native.js';
import type { LufsResult, LufsSeriesResult } from './types.js';
import { assertAudioInput, requestObject, sampleOneShot } from './validation.js';

// Native names the loudness range `loudnessRange`; the public field carries its unit.
function toLufsResult(
  native: Omit<LufsResult, 'loudnessRangeLu'> & { loudnessRange: number },
): LufsResult {
  const { loudnessRange, ...rest } = native;
  return { ...rest, loudnessRangeLu: loudnessRange };
}

/** Request for {@link lufs}. `validate: false` skips the NaN/Inf pre-scan. */
export interface LufsRequest {
  samples: Float32Array;
  sampleRate: number;
  validate?: boolean;
}

/** Request for {@link momentaryLufs}. */
export type MomentaryLufsRequest = LufsRequest;

/** Request for {@link shortTermLufs}. */
export type ShortTermLufsRequest = LufsRequest;

/** Request for {@link lufsInterleaved} and {@link lufsSeriesInterleaved}. */
export interface LufsInterleavedRequest {
  /** Interleaved `frames * channels` samples. */
  samples: Float32Array;
  channels: number;
  /** Default 22050; pass the buffer's actual rate. */
  sampleRate?: number;
}

/** Request for {@link ebur128LoudnessRange}. */
export interface Ebur128LoudnessRangeRequest {
  samples: Float32Array;
  /** Default 22050; pass the buffer's actual rate. */
  sampleRate?: number;
}

/**
 * Channel-weighted multichannel loudness + LRA (BS.1770 / EBU R128) from an
 * interleaved buffer of `frames * channels` samples. The per-channel frame
 * count is derived from the buffer length and `channels`.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 */
export function lufsInterleaved(request: LufsInterleavedRequest): LufsResult;
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
): LufsResult;
export function lufsInterleaved(
  samples: Float32Array | LufsInterleavedRequest,
  channels = 0,
  sampleRate = 22050,
): LufsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, channels, sampleRate }
      : requestObject('lufsInterleaved', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('lufsInterleaved', request.samples, resolvedSampleRate, request);
  return toLufsResult(addon.lufsInterleaved(request.samples, request.channels, resolvedSampleRate));
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
export function lufsSeriesInterleaved(request: LufsInterleavedRequest): LufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
): LufsSeriesResult;
export function lufsSeriesInterleaved(
  samples: Float32Array | LufsInterleavedRequest,
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
export function ebur128LoudnessRange(request: Ebur128LoudnessRangeRequest): number;
export function ebur128LoudnessRange(samples: Float32Array, sampleRate?: number): number;
export function ebur128LoudnessRange(
  samples: Float32Array | Ebur128LoudnessRangeRequest,
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
 * actual `sampleRate`: K-weighting is sample-rate dependent, so a wrong rate
 * yields wrong loudness.
 */
export function lufs(request: LufsRequest): LufsResult;
export function lufs(
  samples: Float32Array,
  sampleRate: number,
  options?: Omit<LufsRequest, 'samples' | 'sampleRate'>,
): LufsResult;
export function lufs(...args: unknown[]): LufsResult {
  const request = sampleOneShot<LufsRequest>('lufs', args);
  assertAudioInput('lufs', request.samples, request.sampleRate, request);
  return toLufsResult(addon.lufs(request.samples, request.sampleRate));
}

/** Per-block momentary (400 ms) LUFS series. Pass the buffer's actual `sampleRate`. */
export function momentaryLufs(request: MomentaryLufsRequest): Float32Array;
export function momentaryLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: Omit<MomentaryLufsRequest, 'samples' | 'sampleRate'>,
): Float32Array;
export function momentaryLufs(...args: unknown[]): Float32Array {
  const request = sampleOneShot<MomentaryLufsRequest>('momentaryLufs', args);
  assertAudioInput('momentaryLufs', request.samples, request.sampleRate, request);
  return addon.momentaryLufs(request.samples, request.sampleRate);
}

/** Per-block short-term (3 s) LUFS series. Pass the buffer's actual `sampleRate`. */
export function shortTermLufs(request: ShortTermLufsRequest): Float32Array;
export function shortTermLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: Omit<ShortTermLufsRequest, 'samples' | 'sampleRate'>,
): Float32Array;
export function shortTermLufs(...args: unknown[]): Float32Array {
  const request = sampleOneShot<ShortTermLufsRequest>('shortTermLufs', args);
  assertAudioInput('shortTermLufs', request.samples, request.sampleRate, request);
  return addon.shortTermLufs(request.samples, request.sampleRate);
}
