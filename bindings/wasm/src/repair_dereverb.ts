/**
 * Reverberation repair, with its detector and the room-driven configuration.
 */

import { refuseType, withEntry } from './internal/entry.js';
import { repairNativeOptions, sharedReportResult } from './internal/repair_common.js';
import { getSonareModule } from './module_state.js';
import type { RoomEstimateResult } from './public_types_acoustic.js';
import type {
  DereverbResult,
  MasteringRepairDereverbClassicalResult,
  ReverbDetection,
} from './public_types_repair.js';
import { assertAudioChannels, assertAudioInput, requestOnly, sampleOneShot } from './validation.js';

function requireModule() {
  return getSonareModule();
}

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
export interface MasteringRepairDereverbClassicalRequest extends DereverbClassicalOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/** The dereverb settings as the room helper takes and returns them. */
type DereverbClassicalConfigOptions = DereverbClassicalOptions;

/** A complete dereverb config, as `masteringRepairDereverbConfigForRoom` returns it. */
export type DereverbClassicalConfig = Required<DereverbClassicalConfigOptions>;

/** Request form of `masteringRepairDereverbClassicalStereo`. */
export interface MasteringRepairDereverbClassicalStereoRequest extends DereverbClassicalOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/** Request form of `masteringRepairDereverbClassicalLinked`. */
export interface MasteringRepairDereverbClassicalLinkedRequest extends DereverbClassicalOptions {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  /** Sample rate in Hz. */
  sampleRate: number;
}

/** Request form of `masteringRepairDereverbConfigForRoom`. */
export interface MasteringRepairDereverbConfigForRoomRequest
  extends DereverbClassicalConfigOptions {
  /** The measured room, from `estimateRoom`. Only `volume` and `rt60Bands` are read. */
  estimate: RoomEstimateResult;
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): DereverbResult;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): DereverbResult;
export function masteringRepairDereverbClassical(...args: unknown[]): DereverbResult {
  return withEntry('masteringRepairDereverbClassical', () => {
    const r = sampleOneShot<MasteringRepairDereverbClassicalRequest>(
      'masteringRepairDereverbClassical',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return requireModule().masteringRepairDereverbClassical(
      r.samples,
      r.sampleRate,
      repairNativeOptions(r),
    );
  });
}

/**
 * Offline classical dereverberator for a stereo pair (spectral subtraction plus an optional
 * WPE pre-stage), driven by one channel-linked mask.
 *
 * The mask is built from the channel-summed power, and the WPE stage accumulates over both
 * channels and applies one predictor set to each, so neither stage can move an interchannel
 * level or phase difference. That is also why the result carries a single report rather than
 * one per channel.
 *
 * Every field of that report is a ratio or a fraction, so unlike the denoise pair nothing in
 * it shifts with the channel count: a stereo figure here is comparable against a mono one.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back to the input
 * length, as {@link masteringRepairDenoiseClassicalStereo} does.
 *
 * Two report fields are gated on the WPE stage, which is off unless `wpeEnabled` is set:
 * `detected.latePredictability` is then absent, and `wpePredictorNorm` is exactly 0 as its
 * measurement. `detected.lateDecayRatioDb` runs the other way from
 * what its name suggests — less negative means the material sustains across the module's late
 * lag, so a reverberant input reads *higher* than the same material dry.
 *
 * `threshold` and `attenuation` are both validated to `[0, 1]`, so the strongest gate this
 * accepts is `threshold: 0.99`, not an arbitrarily large number.
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
  return withEntry('masteringRepairDereverbClassicalStereo', () => {
    const r = requestOnly<MasteringRepairDereverbClassicalStereoRequest>(
      'masteringRepairDereverbClassicalStereo',
      args,
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return sharedReportResult(
      requireModule().masteringRepairDereverbClassicalStereo(
        r.left,
        r.right,
        r.sampleRate,
        repairNativeOptions(r),
      ),
    );
  });
}

/**
 * Offline classical dereverberator for any number of channels (spectral subtraction plus an
 * optional WPE pre-stage), driven by one channel-linked mask.
 *
 * The N-channel form of {@link masteringRepairDereverbClassicalStereo}: the mask is built from
 * the channel-summed power and the WPE stage accumulates over every channel and applies one
 * predictor set to each, so neither stage can move an interchannel level or phase difference
 * however many channels there are. One report for the set, and one output per input channel in
 * input order.
 *
 * A single channel reproduces {@link masteringRepairDereverbClassical} bit for bit, and two
 * reproduce {@link masteringRepairDereverbClassicalStereo} plane for plane — `channels[0]` is the
 * left plane and `channels[1]` the right.
 *
 * Every field of the report is a ratio or a fraction, so nothing in it shifts with the channel
 * count: a figure measured over a set is comparable against a mono one. That is the opposite of
 * {@link masteringRepairDenoiseClassicalLinked}, whose `detected` levels are absolute and move by
 * `10*log10(N)`.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back to the input
 * length, as that entry does.
 *
 * Two report fields are gated on the WPE stage, which is off unless `wpeEnabled` is set:
 * `detected.latePredictability` is then absent, and `wpePredictorNorm` is exactly 0 as its
 * measurement.
 *
 * @example
 * ```ts
 * declare const frontLeft: Float32Array, frontRight: Float32Array, centre: Float32Array;
 * const { channels, reports } = masteringRepairDereverbClassicalLinked({
 *   channels: [frontLeft, frontRight, centre],
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(channels.length, reports[0].detected.lateDecayRatioDb);
 * ```
 */
export function masteringRepairDereverbClassicalLinked(
  request: MasteringRepairDereverbClassicalLinkedRequest,
): MasteringRepairDereverbClassicalResult;
export function masteringRepairDereverbClassicalLinked(
  ...args: unknown[]
): MasteringRepairDereverbClassicalResult {
  return withEntry('masteringRepairDereverbClassicalLinked', () => {
    const r = requestOnly<MasteringRepairDereverbClassicalLinkedRequest>(
      'masteringRepairDereverbClassicalLinked',
      args,
    );
    assertAudioChannels(r.channels, r.sampleRate, r);
    return sharedReportResult(
      requireModule().masteringRepairDereverbClassicalLinked(
        r.channels,
        r.sampleRate,
        repairNativeOptions(r),
      ),
    );
  });
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
 * const clean = masteringRepairDereverbClassical(samples, sampleRate, config);
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
  return withEntry('masteringRepairDereverbConfigForRoom', () => {
    const r = requestOnly<MasteringRepairDereverbConfigForRoomRequest>(
      'masteringRepairDereverbConfigForRoom',
      args,
    );
    if (typeof r.estimate !== 'object' || r.estimate === null) {
      throw refuseType('estimate must be a room estimate');
    }
    const { estimate, ...config } = r;
    return requireModule().masteringRepairDereverbConfigForRoom(estimate, config);
  });
}

/** Request form of `masteringRepairDetectReverb`. */
export interface MasteringRepairDetectReverbRequest extends DereverbClassicalOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Measures reverberation without dereverberating.
 *
 * NOT an ISO 3382 reverberation time — use `estimateRoom` for a graded RT60. This reports what
 * {@link masteringRepairDereverbClassical} itself measures while deciding how much to subtract.
 *
 * A buffer shorter than `nFft` is padded for analysis, as the repair pads it and as
 * {@link masteringRepairDetectNoiseFloor} does.
 *
 * `latePredictability` comes from the WPE stage, which runs only under `wpeEnabled` — clear by
 * default — and then only its covariance and solve; the prediction is never subtracted. A
 * default-config call therefore omits it.
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
  return withEntry('masteringRepairDetectReverb', () => {
    const r = sampleOneShot<MasteringRepairDetectReverbRequest>(
      'masteringRepairDetectReverb',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return requireModule().masteringRepairDetectReverb(
      r.samples,
      r.sampleRate,
      repairNativeOptions(r),
    );
  });
}
