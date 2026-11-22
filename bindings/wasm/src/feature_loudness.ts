/**
 * Programme loudness over planar multichannel audio, as the broadcast standards define
 * it.
 */

import { withEntry } from './internal/entry.js';
import {
  interleaveMeterChannels,
  lufsSeriesFromNative,
  measured,
} from './internal/metering_native.js';
import { getSonareModule } from './module_state.js';
import type { LufsResult, LufsSeriesResult } from './public_types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, assertSampleRate, requestOnly, sampleOneShot } from './validation.js';

/** Request form of {@link lufsMultichannel}. */
export interface LufsMultichannelRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Sample rate of the channels in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link lufsSeriesMultichannel}. */
export interface LufsSeriesMultichannelRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Sample rate of the channels in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link ebur128LoudnessRangeLu}. */
export interface Ebur128LoudnessRangeLuRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/**
 * Channel-weighted multichannel integrated loudness + LRA (ITU-R BS.1770 /
 * EBU R128) over planar channels of equal length.
 *
 * Pass the channels' actual `sampleRate`: K-weighting is sample-rate dependent, so a
 * wrong rate yields wrong loudness.
 */
export function lufsMultichannel(request: LufsMultichannelRequest): LufsResult;
export function lufsMultichannel(...args: unknown[]): LufsResult {
  return withEntry('lufsMultichannel', () => {
    const r = requestOnly<LufsMultichannelRequest>('lufsMultichannel', args);
    assertSampleRate(r.sampleRate);
    // Native reads one interleaved buffer and a channel count.
    const { data, numChannels } = interleaveMeterChannels(r.channels, r, r.sampleRate);
    return getSonareModule().lufsInterleaved(data, numChannels, r.sampleRate);
  });
}

/**
 * Per-block momentary (400 ms) and short-term (3 s) LUFS series for planar
 * channels of equal length, measured with ITU-R BS.1770-4 channel summing.
 *
 * This is not recoverable from `momentaryLufs` / `shortTermLufs` of {@link lufsMultichannel}:
 * those are the final window only, and the standard sums the K-weighted per-channel block
 * energies rather than mixing per-channel loudness in dB. Both series come out
 * of one K-weighting pass. For one channel they match the mono meters element for element.
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
    return lufsSeriesFromNative(
      getSonareModule().lufsSeriesInterleaved(data, numChannels, r.sampleRate),
    );
  });
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: K-weighting is sample-rate dependent. `undefined`
 * when no short-term window passes the gate: a signal shorter than 3 s, or silence.
 */
export function ebur128LoudnessRangeLu(request: Ebur128LoudnessRangeLuRequest): number | undefined;
export function ebur128LoudnessRangeLu(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<Ebur128LoudnessRangeLuRequest>,
): number | undefined;
export function ebur128LoudnessRangeLu(...args: unknown[]): number | undefined {
  return withEntry('ebur128LoudnessRangeLu', () => {
    const r = sampleOneShot<Ebur128LoudnessRangeLuRequest>('ebur128LoudnessRangeLu', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return measured(getSonareModule().ebur128LoudnessRangeLu(r.samples, r.sampleRate));
  });
}
