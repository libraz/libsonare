/**
 * Reverberation repair, with its detector and the room-driven configuration.
 */

import {
  assertRepairGeometry,
  repairNativeOptions,
  sharedReportResult,
} from './internal/repair_common.js';
import { addon } from './native.js';
import type {
  DereverbResult,
  MasteringRepairDereverbClassicalResult,
  MasteringRepairSamplesRequest,
  ReverbDetection,
  RoomEstimateResult,
} from './types.js';
import { assertAudioChannels, assertAudioInput, requestOnly, sampleOneShot } from './validation.js';

/** A complete dereverb config, as `masteringRepairDereverbConfigForRoom` returns it. */
export type DereverbClassicalConfig = Required<DereverbClassicalOptions>;

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  /** Late-reverberation threshold relative to a bin's own power, in [0, 1]; `0` suppresses every bin carrying late energy. Default `0`. */
  threshold?: number;
  /** Share of the computed suppression applied, from 0 (none) to 1 (all). Default `1`. */
  attenuation?: number;
  /** STFT size in samples; a power of two. Default `1024`. */
  nFft?: number;
  /** Hop in samples, in (0, nFft / 2]. */
  hopLength?: number;
  /** Assumed reverberation time in seconds, in (0, 10]. Default `0.4`. */
  t60Sec?: number;
  /** Delay after which reverberation counts as late, in milliseconds, from 0 to 500. Default `50`. */
  lateDelayMs?: number;
  /** Over-subtraction factor, from 0 to 16. Default `1`. */
  overSubtraction?: number;
  /** Floor multiplier relative to the input power, in [0, 1]. Default `0.08`. */
  spectralFloor?: number;
  /** Whether the WPE stage runs. Default `false`. */
  wpeEnabled?: boolean;
  /** WPE iterations, from 1 to 16. Default `2`. */
  wpeIterations?: number;
  /** WPE filter taps, from 1 to 32. Default `3`. */
  wpeTaps?: number;
  /** WPE strength, in [0, 1]. Default `0.7`. */
  wpeStrength?: number;
}

/** Request form of `masteringRepairDereverbClassical`. */
export interface MasteringRepairDereverbClassicalRequest
  extends MasteringRepairSamplesRequest,
    DereverbClassicalOptions {}

/** Request form of `masteringRepairDereverbConfigForRoom`. */
export interface MasteringRepairDereverbConfigForRoomRequest extends DereverbClassicalOptions {
  /** The measured room, from `estimateRoom`. Only `volume` and `rt60Bands` are read. */
  estimate: RoomEstimateResult;
}

/**
 * Offline classical dereverberator (spectral subtraction + optional WPE). Returns the repaired
 * audio and what the pass did.
 */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): DereverbResult;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): DereverbResult;
export function masteringRepairDereverbClassical(...args: unknown[]): DereverbResult {
  const r = sampleOneShot<MasteringRepairDereverbClassicalRequest>(
    'masteringRepairDereverbClassical',
    args,
  );
  assertAudioInput('masteringRepairDereverbClassical', r.samples, r.sampleRate, r);
  assertRepairGeometry('masteringRepairDereverbClassical', r);
  return addon.masteringRepairDereverbClassical(r.samples, r.sampleRate, repairNativeOptions(r));
}

/** Request form of `masteringRepairDereverbClassicalStereo`. */
export interface MasteringRepairDereverbClassicalStereoRequest extends DereverbClassicalOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline classical dereverberator for a stereo pair (spectral subtraction
 * plus an optional WPE pre-stage), driven by one channel-linked mask.
 *
 * The mask is built from the channel-summed power, and the WPE stage
 * accumulates over both channels and applies one predictor set to each, so
 * neither stage can move an interchannel level or phase difference. That is
 * also why the result carries a single report rather than one per channel.
 *
 * Every field of that report is a ratio or a fraction, so unlike the denoise
 * pair nothing in it shifts with the channel count: a stereo figure here is
 * comparable against a mono one.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back
 * to the input length, as {@link masteringRepairDenoiseClassicalStereo} does.
 *
 * Two report fields are gated on the WPE stage, which is off unless
 * `wpeEnabled` is set: `detected.latePredictability` is then absent, and
 * `wpePredictorNorm` is exactly 0 as its measurement.
 * `detected.lateDecayRatioDb` runs the other way from
 * what its name suggests — less negative means the material sustains across
 * the module's late lag, so a reverberant input reads *higher* than the same
 * material dry.
 *
 * @example
 * ```ts
 * const { channels, reports } = masteringRepairDereverbClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(reports[0].detected.lateDecayRatioDb, reports[0].wpePredictorNorm);
 * ```
 */
export function masteringRepairDereverbClassicalStereo(
  request: MasteringRepairDereverbClassicalStereoRequest,
): MasteringRepairDereverbClassicalResult;
export function masteringRepairDereverbClassicalStereo(
  ...args: unknown[]
): MasteringRepairDereverbClassicalResult {
  const r = requestOnly<MasteringRepairDereverbClassicalStereoRequest>(
    'masteringRepairDereverbClassicalStereo',
    args,
  );
  assertAudioInput('masteringRepairDereverbClassicalStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringRepairDereverbClassicalStereo', r.right, r.sampleRate, r, 'right');
  assertRepairGeometry('masteringRepairDereverbClassicalStereo', r);
  return sharedReportResult(
    addon.masteringRepairDereverbClassicalStereo(
      r.left,
      r.right,
      r.sampleRate,
      repairNativeOptions(r),
    ),
  );
}

/** Request form of `masteringRepairDereverbClassicalLinked`. */
export interface MasteringRepairDereverbClassicalLinkedRequest extends DereverbClassicalOptions {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline classical dereverberator for any number of channels (spectral
 * subtraction plus an optional WPE pre-stage), driven by one channel-linked
 * mask.
 *
 * The N-channel form of {@link masteringRepairDereverbClassicalStereo}, sharing
 * both stages across the whole set rather than just a pair: one mask over the
 * channel-summed power, and one WPE predictor set fitted over every channel's
 * statistics, so neither stage can move an interchannel level or phase
 * difference. That is also why the result carries a single report. One channel
 * reproduces {@link masteringRepairDereverbClassical} sample for sample, and two
 * reproduce {@link masteringRepairDereverbClassicalStereo} plane for plane,
 * `channels[0]` being the left.
 *
 * Every field of the report is a ratio or a fraction, so unlike
 * {@link masteringRepairDenoiseClassicalLinked} nothing here shifts with the
 * channel count: a figure measured over six channels is comparable against a
 * mono one.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back
 * to the input length, as {@link masteringRepairDenoiseClassicalLinked} does.
 *
 * Every channel must be the same length: the library takes one length for the
 * set, so a disagreement is refused here rather than silently truncated.
 *
 * Two report fields are gated on the WPE stage, which is off unless
 * `wpeEnabled` is set: `detected.latePredictability` is then absent, and
 * `wpePredictorNorm` is exactly 0 as its measurement.
 * `detected.lateDecayRatioDb` runs the other way from what its name suggests —
 * less negative means the material sustains across the module's late lag, so a
 * reverberant input reads *higher* than the same material dry.
 *
 * @example
 * ```ts
 * declare const centre: Float32Array;
 * const { channels, reports } = masteringRepairDereverbClassicalLinked({
 *   channels: [left, right, centre],
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(channels.length, reports[0].wpePredictorNorm);
 * ```
 */
export function masteringRepairDereverbClassicalLinked(
  request: MasteringRepairDereverbClassicalLinkedRequest,
): MasteringRepairDereverbClassicalResult;
export function masteringRepairDereverbClassicalLinked(
  ...args: unknown[]
): MasteringRepairDereverbClassicalResult {
  const r = requestOnly<MasteringRepairDereverbClassicalLinkedRequest>(
    'masteringRepairDereverbClassicalLinked',
    args,
  );
  assertAudioChannels('masteringRepairDereverbClassicalLinked', r.channels, r.sampleRate, r);
  assertRepairGeometry('masteringRepairDereverbClassicalLinked', r);
  return sharedReportResult(
    addon.masteringRepairDereverbClassicalLinked(r.channels, r.sampleRate, repairNativeOptions(r)),
  );
}

/**
 * Point a dereverb config at a measured room.
 *
 * The pair to {@link estimateRoom}, which measures a recording blind. Returns a complete
 * config for {@link masteringRepairDereverbClassical}, so the caller does not have to know
 * which reverberation-time band to use or how the late delay relates to room size.
 *
 * What the room decides is *where* the tail is. Exactly two fields come back changed from
 * what was passed in:
 *
 * - `t60Sec` — the mid-frequency reverberation time, the average of the 500 Hz and 1 kHz
 *   octaves an ISO 3382 room is quoted by.
 * - `lateDelayMs` — Polack's mixing time, sqrt(volume) in milliseconds, past which the
 *   response is a diffuse tail rather than separable early reflections.
 *
 * How *much* to remove is taste rather than measurement, so `attenuation`, `threshold`,
 * `overSubtraction` and `spectralFloor` are never written. A measurement that did not
 * converge leaves its own field alone, so a partial estimate still configures the half it
 * measured; a low-`confidence` estimate is still applied, because whether to trust it is
 * the caller's call.
 *
 * Every field of `config` that is omitted falls back to the library's own dereverb default,
 * matching {@link masteringRepairDereverbClassical}, so calling this with only an estimate
 * returns a config that is ready to run. The C ABI underneath reads and writes the whole
 * config and takes every field literally — it has no "zero means default" rule — which is
 * why an omitted field resolves to its default here rather than to zero.
 *
 * @param request - `estimate` is the measured room, from {@link estimateRoom}; only `volume`
 *   and `rt60Bands` are read. The other fields are the config to point at the room; omitted
 *   fields take the library default.
 * @returns A complete dereverb config.
 *
 * @example
 * ```ts
 * const estimate = estimateRoom({ samples, sampleRate });
 * const config = masteringRepairDereverbConfigForRoom({ estimate });
 * const { samples: clean } = masteringRepairDereverbClassical(samples, sampleRate, config);
 * ```
 *
 * When NEITHER mid band converged, `t60Sec` falls back to the average of whatever
 * bands did, so a low-band-only estimate configures something rather than nothing --
 * that value is no longer a mid-frequency figure. Only `volume`, `rt60Bands` and the
 * band count are read; the rest of the estimate is ignored.
 */
export function masteringRepairDereverbConfigForRoom(
  request: MasteringRepairDereverbConfigForRoomRequest,
): DereverbClassicalConfig;
export function masteringRepairDereverbConfigForRoom(...args: unknown[]): DereverbClassicalConfig {
  const r = requestOnly<MasteringRepairDereverbConfigForRoomRequest>(
    'masteringRepairDereverbConfigForRoom',
    args,
  );
  if (typeof r.estimate !== 'object' || r.estimate === null) {
    throw new TypeError('masteringRepairDereverbConfigForRoom: estimate must be a room estimate');
  }
  assertRepairGeometry('masteringRepairDereverbConfigForRoom', r);
  const { estimate, ...config } = r;
  return addon.masteringRepairDereverbConfigForRoom(estimate, config);
}

/** Request form of `masteringRepairDetectReverb`. */
export interface MasteringRepairDetectReverbRequest
  extends MasteringRepairSamplesRequest,
    DereverbClassicalOptions {}

/**
 * Measure reverberation without dereverberating.
 *
 * Runs the STFT and the module's own late-lag decay statistic. A buffer shorter
 * than `nFft` is PADDED for analysis, as the repair pads it -- the opposite of
 * {@link masteringRepairDetectNoiseFloor}, which refuses one.
 *
 * NOT an ISO 3382 reverberation time; use {@link estimateRoom} for a graded
 * RT60. `latePredictability` comes from the WPE stage, which runs only under
 * `wpeEnabled` and then only its covariance and solve, so a default-config
 * call omits it.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectReverb({ samples, sampleRate: 48000 });
 * console.log(detected.lateDecayRatioDb, detected.latePredictability);
 * ```
 */
export function masteringRepairDetectReverb(
  request: MasteringRepairDetectReverbRequest,
): ReverbDetection;
export function masteringRepairDetectReverb(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): ReverbDetection;
export function masteringRepairDetectReverb(...args: unknown[]): ReverbDetection {
  const r = sampleOneShot<MasteringRepairDetectReverbRequest>('masteringRepairDetectReverb', args);
  assertAudioInput('masteringRepairDetectReverb', r.samples, r.sampleRate, r);
  assertRepairGeometry('masteringRepairDetectReverb', r);
  return addon.masteringRepairDetectReverb(r.samples, r.sampleRate, repairNativeOptions(r));
}
