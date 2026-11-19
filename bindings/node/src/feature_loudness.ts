import { withEntry } from './internal/entry.js';
import { interleaveMeterChannels, lufsSeriesFromNative } from './internal/metering_native.js';
import { addon } from './native.js';
import type { LufsResult, LufsSeriesResult } from './types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, assertSampleRate, requestOnly, sampleOneShot } from './validation.js';

/** Request for {@link lufs}. */
export interface LufsRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request for {@link momentaryLufs}. */
export type MomentaryLufsRequest = LufsRequest;

/** Request for {@link shortTermLufs}. */
export type ShortTermLufsRequest = LufsRequest;

/** Request for {@link lufsMultichannel}. */
export interface LufsMultichannelRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Sample rate of the channels in Hz. Required. */
  sampleRate: number;
}

/** Request for {@link lufsSeriesMultichannel}. */
export interface LufsSeriesMultichannelRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Sample rate of the channels in Hz. Required. */
  sampleRate: number;
}

/** Request for {@link ebur128LoudnessRangeLu}. */
export interface Ebur128LoudnessRangeLuRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/**
 * Channel-weighted multichannel integrated loudness + LRA (BS.1770 / EBU R128)
 * over planar channels of equal length.
 *
 * Pass the channels' actual `sampleRate`: K-weighting is sample-rate dependent, so a
 * wrong rate yields wrong loudness.
 */
export function lufsMultichannel(request: LufsMultichannelRequest): LufsResult;
export function lufsMultichannel(...args: unknown[]): LufsResult {
  return withEntry('lufsMultichannel', () => {
    const r = requestOnly<LufsMultichannelRequest>('lufsMultichannel', args);
    assertSampleRate(r.sampleRate);
    // The addon reads one interleaved buffer and a channel count.
    const { data, numChannels } = interleaveMeterChannels(r.channels, r, r.sampleRate);
    return addon.lufsInterleaved(data, numChannels, r.sampleRate);
  });
}

/**
 * Per-block momentary (400 ms) and short-term (3 s) LUFS series for planar
 * channels of equal length, measured with ITU-R BS.1770-4 channel summing.
 *
 * This is not recoverable from {@link momentaryLufs} / {@link shortTermLufs}:
 * those measure one channel each, and the standard sums the K-weighted
 * per-channel block energies rather than mixing per-channel loudness in dB. Both
 * series come out of one K-weighting pass. For one channel they match the
 * mono meters element for element.
 *
 * Pass the channels' actual `sampleRate`: K-weighting is sample-rate dependent.
 *
 * @example
 * ```ts
 * const { momentaryLufs, shortTermLufs } = lufsSeriesMultichannel({
 *   channels: [left, right],
 *   sampleRate: 48000,
 * });
 * ```
 */
export function lufsSeriesMultichannel(request: LufsSeriesMultichannelRequest): LufsSeriesResult;
export function lufsSeriesMultichannel(...args: unknown[]): LufsSeriesResult {
  return withEntry('lufsSeriesMultichannel', () => {
    const r = requestOnly<LufsSeriesMultichannelRequest>('lufsSeriesMultichannel', args);
    assertSampleRate(r.sampleRate);
    const { data, numChannels } = interleaveMeterChannels(r.channels, r, r.sampleRate);
    return lufsSeriesFromNative(addon.lufsSeriesInterleaved(data, numChannels, r.sampleRate));
  });
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: K-weighting is sample-rate dependent.
 */
export function ebur128LoudnessRangeLu(request: Ebur128LoudnessRangeLuRequest): number;
export function ebur128LoudnessRangeLu(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<Ebur128LoudnessRangeLuRequest>,
): number;
export function ebur128LoudnessRangeLu(...args: unknown[]): number {
  return withEntry('ebur128LoudnessRangeLu', () => {
    const r = sampleOneShot<Ebur128LoudnessRangeLuRequest>('ebur128LoudnessRangeLu', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.ebur128LoudnessRangeLu(r.samples, r.sampleRate);
  });
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
  options?: OptionsOf<LufsRequest>,
): LufsResult;
export function lufs(...args: unknown[]): LufsResult {
  return withEntry('lufs', () => {
    const request = sampleOneShot<LufsRequest>('lufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.lufs(request.samples, request.sampleRate);
  });
}

/** Per-block momentary (400 ms) LUFS series. Pass the buffer's actual `sampleRate`. */
export function momentaryLufs(request: MomentaryLufsRequest): Float32Array;
export function momentaryLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MomentaryLufsRequest>,
): Float32Array;
export function momentaryLufs(...args: unknown[]): Float32Array {
  return withEntry('momentaryLufs', () => {
    const request = sampleOneShot<MomentaryLufsRequest>('momentaryLufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.momentaryLufs(request.samples, request.sampleRate);
  });
}

/** Per-block short-term (3 s) LUFS series. Pass the buffer's actual `sampleRate`. */
export function shortTermLufs(request: ShortTermLufsRequest): Float32Array;
export function shortTermLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ShortTermLufsRequest>,
): Float32Array;
export function shortTermLufs(...args: unknown[]): Float32Array {
  return withEntry('shortTermLufs', () => {
    const request = sampleOneShot<ShortTermLufsRequest>('shortTermLufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.shortTermLufs(request.samples, request.sampleRate);
  });
}
