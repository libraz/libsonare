/**
 * Reverberation repair, with its detector and the room-driven configuration.
 */

import { getSonareModule } from './module_state';
import type { RoomEstimateResult } from './public_types_acoustic';
import type {
  MasteringRepairDereverbClassicalLinkedResult,
  MasteringRepairDereverbClassicalStereoResult,
  ReverbDetection,
} from './public_types_repair';

function requireModule() {
  return getSonareModule();
}

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  threshold?: number;
  attenuation?: number;
  nFft?: number;
  hopLength?: number;
  t60Sec?: number;
  lateDelayMs?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  wpeEnabled?: boolean;
  wpeIterations?: number;
  wpeTaps?: number;
  wpeStrength?: number;
}

export interface MasteringRepairDereverbClassicalRequest extends DereverbClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDereverbClassicalStereo`. */
export interface MasteringRepairDereverbClassicalStereoRequest extends DereverbClassicalOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Request form of `masteringRepairDereverbClassicalLinked`. */
export interface MasteringRepairDereverbClassicalLinkedRequest extends DereverbClassicalOptions {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  sampleRate?: number;
}

/** Request form of `masteringRepairDereverbConfigForRoom`. */
export interface MasteringRepairDereverbConfigForRoomRequest extends DereverbClassicalOptions {
  /** The measured room, from `estimateRoom`. Only `volume` and `rt60Bands` are read. */
  estimate: RoomEstimateResult;
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array | MasteringRepairDereverbClassicalRequest,
  sampleRate?: number,
  options: DereverbClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDereverbClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Offline classical dereverberator for a stereo pair (spectral subtraction plus an optional
 * WPE pre-stage), driven by one channel-linked mask.
 *
 * The mask is built from the channel-summed power, and the WPE stage accumulates over both
 * channels and applies one predictor set to each, so neither stage can move an interchannel
 * level or phase difference. That is also why the result carries a single `report` rather than
 * one per channel.
 *
 * Every field of that report is a ratio or a fraction, so unlike the denoise pair nothing in
 * it shifts with the channel count: a stereo figure here is comparable against a mono one.
 *
 * An input shorter than `nFft` is PADDED for analysis rather than rejected, the opposite of
 * {@link masteringRepairDenoiseClassicalStereo}.
 *
 * Two report fields are gated on the WPE stage, which is off unless `wpeEnabled` is set:
 * `detected.latePredictability` and `wpePredictorNorm` are then both exactly 0, which is the
 * measurement rather than an unset field. `detected.lateDecayRatioDb` runs the other way from
 * what its name suggests — less negative means the material sustains across the module's late
 * lag, so a reverberant input reads *higher* than the same material dry.
 *
 * `threshold` and `attenuation` are both validated to `[0, 1]`, so the strongest gate this
 * accepts is `threshold: 0.99`, not an arbitrarily large number.
 *
 * @example
 * ```ts
 * const { left, right, report } = masteringRepairDereverbClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(report.detected.lateDecayRatioDb, report.wpePredictorNorm);
 * ```
 */
export function masteringRepairDereverbClassicalStereo(
  request: MasteringRepairDereverbClassicalStereoRequest,
): MasteringRepairDereverbClassicalStereoResult;
export function masteringRepairDereverbClassicalStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DereverbClassicalOptions,
): MasteringRepairDereverbClassicalStereoResult;
export function masteringRepairDereverbClassicalStereo(
  left: Float32Array | MasteringRepairDereverbClassicalStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DereverbClassicalOptions = {},
): MasteringRepairDereverbClassicalStereoResult {
  const request: MasteringRepairDereverbClassicalStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDereverbClassicalStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/**
 * Offline classical dereverberator for any number of channels (spectral subtraction plus an
 * optional WPE pre-stage), driven by one channel-linked mask.
 *
 * The N-channel form of {@link masteringRepairDereverbClassicalStereo}: the mask is built from
 * the channel-summed power and the WPE stage accumulates over every channel and applies one
 * predictor set to each, so neither stage can move an interchannel level or phase difference
 * however many channels there are. One `report` for the set, and one output per input channel in
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
 * An input shorter than `nFft` is PADDED for analysis rather than rejected — again the opposite
 * of that entry, which refuses one.
 *
 * Two report fields are gated on the WPE stage, which is off unless `wpeEnabled` is set:
 * `detected.latePredictability` and `wpePredictorNorm` are then both exactly 0, which is the
 * measurement rather than an unset field.
 *
 * @example
 * ```ts
 * const { channels, report } = masteringRepairDereverbClassicalLinked({
 *   channels: [frontLeft, frontRight, centre],
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(channels.length, report.detected.lateDecayRatioDb);
 * ```
 */
export function masteringRepairDereverbClassicalLinked(
  request: MasteringRepairDereverbClassicalLinkedRequest,
): MasteringRepairDereverbClassicalLinkedResult;
export function masteringRepairDereverbClassicalLinked(
  channels: Float32Array[],
  sampleRate: number,
  config?: DereverbClassicalOptions,
): MasteringRepairDereverbClassicalLinkedResult;
export function masteringRepairDereverbClassicalLinked(
  channels: Float32Array[] | MasteringRepairDereverbClassicalLinkedRequest,
  sampleRate?: number,
  config: DereverbClassicalOptions = {},
): MasteringRepairDereverbClassicalLinkedResult {
  const request: MasteringRepairDereverbClassicalLinkedRequest = Array.isArray(channels)
    ? { channels, sampleRate, ...config }
    : channels;
  const { channels: input, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDereverbClassicalLinked(input, rate ?? 22050, options);
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
 * @param estimate - The measured room, from {@link estimateRoom}. Only `volume` and
 *   `rt60Bands` are read. The request form carries it as `estimate` alongside the config
 *   fields.
 * @param config - The config to point at the room; omitted fields take the library default.
 * @returns A complete dereverb config.
 *
 * @example
 * ```ts
 * const estimate = estimateRoom(samples, sampleRate);
 * const config = masteringRepairDereverbConfigForRoom(estimate);
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
): Required<DereverbClassicalOptions>;
export function masteringRepairDereverbConfigForRoom(
  estimate: RoomEstimateResult,
  config?: DereverbClassicalOptions,
): Required<DereverbClassicalOptions>;
export function masteringRepairDereverbConfigForRoom(
  estimate: RoomEstimateResult | MasteringRepairDereverbConfigForRoomRequest,
  config: DereverbClassicalOptions = {},
): Required<DereverbClassicalOptions> {
  const request = 'estimate' in estimate ? estimate : { estimate, ...config };
  return requireModule().masteringRepairDereverbConfigForRoom(request.estimate, request);
}

/** Request form of `masteringRepairDetectReverb`. */
export interface MasteringRepairDetectReverbRequest extends DereverbClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/**
 * Measures reverberation without dereverberating.
 *
 * NOT an ISO 3382 reverberation time — use `estimateRoom` for a graded RT60. This reports what
 * {@link masteringRepairDereverbClassical} itself measures while deciding how much to subtract.
 *
 * A buffer shorter than `nFft` is PADDED for analysis, as the repair pads it, which is the
 * opposite of {@link masteringRepairDetectNoiseFloor}.
 *
 * `latePredictability` comes from the WPE stage, which runs only under `wpeEnabled` — clear by
 * default — and then only its covariance and solve; the prediction is never subtracted. A
 * default-config call therefore reports exactly 0 there as its measurement.
 */
export function masteringRepairDetectReverb(
  request: MasteringRepairDetectReverbRequest,
): ReverbDetection;
export function masteringRepairDetectReverb(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): ReverbDetection;
export function masteringRepairDetectReverb(
  samples: Float32Array | MasteringRepairDetectReverbRequest,
  sampleRate?: number,
  options: DereverbClassicalOptions = {},
): ReverbDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectReverb(request.samples, request.sampleRate, request);
}
