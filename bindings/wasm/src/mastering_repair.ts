import { getSonareModule } from './module_state';
import type { RoomEstimateResult } from './public_types_acoustic';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  HumDetection,
  MasteringRepairDeclickStereoResult,
  MasteringRepairDeclipStereoResult,
  MasteringRepairDecrackleStereoResult,
  MasteringRepairDehumStereoResult,
  MasteringRepairDenoiseClassicalLinkedResult,
  MasteringRepairDenoiseClassicalStereoResult,
  MasteringRepairDereverbClassicalLinkedResult,
  MasteringRepairDereverbClassicalStereoResult,
  MasteringRepairTrimSilenceStereoResult,
  NoiseDetection,
  ReverbDetection,
  TrimRange,
} from './public_types_mastering';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Mastering repair (declick, denoise_classical, declip, decrackle, dehum,
// dereverb_classical, trim_silence) — hand-written bindings.
// ============================================================================

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}
export interface MasteringRepairDeclickRequest extends DeclickOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDeclickStereo`. */
export interface MasteringRepairDeclickStereoRequest extends DeclickOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declicker for a stereo pair.
 *
 * A run either channel's own detection selects is repaired in BOTH channels —
 * a common-mode click repaired on one side only would move the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own AR model, which is why `leftReport` and `rightReport`
 * genuinely differ. Prefer this over calling `masteringRepairDeclick` on each
 * channel separately whenever a click may land in only one channel — a
 * per-channel pass never repairs the other side's image-shifting click.
 */
export function masteringRepairDeclickStereo(
  request: MasteringRepairDeclickStereoRequest,
): MasteringRepairDeclickStereoResult;
export function masteringRepairDeclickStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DeclickOptions,
): MasteringRepairDeclickStereoResult;
export function masteringRepairDeclickStereo(
  left: Float32Array | MasteringRepairDeclickStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DeclickOptions = {},
): MasteringRepairDeclickStereoResult {
  const request: MasteringRepairDeclickStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDeclickStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra' | 'spp';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  hopLength?: number;
  ddAlpha?: number;
  reductionDb?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  noiseEstimationQuantile?: number;
  speechPresenceGain?: boolean;
  gainSmoothing?: boolean;
}
export interface MasteringRepairDenoiseClassicalRequest extends DenoiseClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDenoiseClassicalStereo`. */
export interface MasteringRepairDenoiseClassicalStereoRequest extends DenoiseClassicalOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Request form of `masteringRepairDenoiseClassicalLinked`. */
export interface MasteringRepairDenoiseClassicalLinkedRequest extends DenoiseClassicalOptions {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  sampleRate?: number;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclickOptions,
): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array | MasteringRepairDeclickRequest,
  sampleRate?: number,
  options: DeclickOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclick(request.samples, request.sampleRate, request);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DenoiseClassicalOptions,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array | MasteringRepairDenoiseClassicalRequest,
  sampleRate?: number,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDenoiseClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Offline STFT-domain classical denoiser for a stereo pair, driven by one channel-linked
 * gain mask.
 *
 * The mask is built from the channel-summed power and applied unchanged to both channels, so
 * the pass cannot move an interchannel level or phase difference. That is also why the result
 * carries a single `report` rather than one per channel: a pair would be two copies of one
 * measurement and would read as though the two could differ.
 *
 * `report.detected` is therefore a *pair-level* measurement, and the only absolute one in the
 * result. Its levels are dBFS on the channel-summed power, so two identical channels read
 * `10*log10(2)` — about 3.01 dB — above the same material through
 * {@link masteringRepairDenoiseClassical}. A stereo floor is comparable only against another
 * stereo floor, never against a mono one.
 *
 * Needs at least `nFft` samples and REJECTS a shorter input, which is the opposite of
 * {@link masteringRepairDereverbClassicalStereo} — that one pads.
 *
 * Which options are live depends on `mode`: `overSubtraction` and `spectralFloor` are read
 * only by `spectralSubtraction`, and `speechPresenceGain` and `gainSmoothing` only by the
 * other two, so at the default `logMmse` the first pair does nothing.
 *
 * @example
 * ```ts
 * const { left, right, report } = masteringRepairDenoiseClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(report.detected.floorDbfs, report.meanReductionDb);
 * ```
 */
export function masteringRepairDenoiseClassicalStereo(
  request: MasteringRepairDenoiseClassicalStereoRequest,
): MasteringRepairDenoiseClassicalStereoResult;
export function masteringRepairDenoiseClassicalStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DenoiseClassicalOptions,
): MasteringRepairDenoiseClassicalStereoResult;
export function masteringRepairDenoiseClassicalStereo(
  left: Float32Array | MasteringRepairDenoiseClassicalStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DenoiseClassicalOptions = {},
): MasteringRepairDenoiseClassicalStereoResult {
  const request: MasteringRepairDenoiseClassicalStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDenoiseClassicalStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/**
 * Offline STFT-domain classical denoiser for any number of channels, driven by one
 * channel-linked gain mask.
 *
 * The N-channel form of {@link masteringRepairDenoiseClassicalStereo}, carrying the same
 * guarantee over the whole set: the mask is built from the channel-summed power and applied
 * unchanged to every channel, so no interchannel level or phase difference moves however many
 * channels there are. One `report` for the set, and one output per input channel in input order.
 *
 * A single channel reproduces {@link masteringRepairDenoiseClassical} bit for bit, and two
 * reproduce {@link masteringRepairDenoiseClassicalStereo} plane for plane — `channels[0]` is the
 * left plane and `channels[1]` the right.
 *
 * `report.detected` carries absolute levels and they are the SET's: the floor is referred to the
 * summed mean square of every channel, so N identical channels read `10*log10(N)` above
 * one of them — about 3.01 dB at two channels and 4.77 dB at three. Compare a floor only against
 * one measured over the same number of channels. Every other field of the report is a fraction
 * and does not move with the channel count.
 *
 * Needs at least `nFft` samples and REJECTS a shorter input, which is the opposite of
 * {@link masteringRepairDereverbClassicalLinked} — that one pads.
 *
 * @example
 * ```ts
 * const { channels, report } = masteringRepairDenoiseClassicalLinked({
 *   channels: [frontLeft, frontRight, centre],
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(channels.length, report.detected.floorDbfs);
 * ```
 */
export function masteringRepairDenoiseClassicalLinked(
  request: MasteringRepairDenoiseClassicalLinkedRequest,
): MasteringRepairDenoiseClassicalLinkedResult;
export function masteringRepairDenoiseClassicalLinked(
  channels: Float32Array[],
  sampleRate: number,
  config?: DenoiseClassicalOptions,
): MasteringRepairDenoiseClassicalLinkedResult;
export function masteringRepairDenoiseClassicalLinked(
  channels: Float32Array[] | MasteringRepairDenoiseClassicalLinkedRequest,
  sampleRate?: number,
  config: DenoiseClassicalOptions = {},
): MasteringRepairDenoiseClassicalLinkedResult {
  const request: MasteringRepairDenoiseClassicalLinkedRequest = Array.isArray(channels)
    ? { channels, sampleRate, ...config }
    : channels;
  const { channels: input, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDenoiseClassicalLinked(input, rate ?? 22050, options);
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}
export interface MasteringRepairDeclipRequest extends DeclipOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDeclipStereo`. */
export interface MasteringRepairDeclipStereoRequest extends DeclipOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}
export interface MasteringRepairDecrackleRequest extends DecrackleOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDecrackleStereo`. */
export interface MasteringRepairDecrackleStereoRequest extends DecrackleOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * How `masteringRepairDehum` removes the harmonic series.
 *
 * `subtract` tracks each harmonic's amplitude and phase and subtracts the tone they describe,
 * so material sitting at the same frequency but uncorrelated with the tracked series survives.
 * `notch` cascades one RBJ notch per harmonic and removes everything inside each notch's
 * bandwidth, hum or programme alike.
 */
export type DehumMode = 'subtract' | 'notch';

/** Options for `masteringRepairDehum`. */
export interface DehumOptions {
  fundamentalHz?: number;
  harmonics?: number;
  q?: number;
  adaptive?: boolean;
  searchRangeHz?: number;
  adaptation?: number;
  frameSize?: number;
  pllBandwidth?: number;
  /** Defaults to `'subtract'`. */
  mode?: DehumMode;
}
export interface MasteringRepairDehumRequest extends DehumOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDehumStereo`. */
export interface MasteringRepairDehumStereoRequest extends DehumOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
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

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}
export interface MasteringRepairTrimSilenceRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairTrimSilenceStereo`. */
export interface MasteringRepairTrimSilenceStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declipper.
 *
 * Only clipped runs of at most 512 consecutive samples are reconstructed with the LPC solver.
 * The cap is a fixed sample count: it is not derived from `lpcOrder`, from `sampleRate`, or from
 * any other option, so its duration depends on the rate (~10.7 ms at 48 kHz). A longer run is
 * filled with cubic / linear interpolation instead, which keeps the solver's dense matrices
 * bounded by the cap rather than by the input. Exceeding the cap silently changes the
 * reconstruction method rather than throwing: `lpcOrder`, `iterations` and `lpcBlend` have no
 * effect on the interpolated run.
 */
export function masteringRepairDeclip(request: MasteringRepairDeclipRequest): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array | MasteringRepairDeclipRequest,
  sampleRate?: number,
  options: DeclipOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclip(request.samples, request.sampleRate, request);
}

/**
 * Offline LPC-based declipper for a stereo pair.
 *
 * Takes the union of both channels' clipped runs. Each channel reconstructs
 * the whole of every union run it has at least one clipped sample in; a
 * channel with none is left untouched there — reconstructing unclipped audio
 * to match the other side would replace real samples with an estimate. A
 * plateau clipped in only one channel therefore produces no linking:
 * `linkedRuns` is non-zero only where both channels are clipped in the same
 * region with different extents.
 */
export function masteringRepairDeclipStereo(
  request: MasteringRepairDeclipStereoRequest,
): MasteringRepairDeclipStereoResult;
export function masteringRepairDeclipStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DeclipOptions,
): MasteringRepairDeclipStereoResult;
export function masteringRepairDeclipStereo(
  left: Float32Array | MasteringRepairDeclipStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DeclipOptions = {},
): MasteringRepairDeclipStereoResult {
  const request: MasteringRepairDeclipStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDeclipStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array | MasteringRepairDecrackleRequest,
  sampleRate?: number,
  options: DecrackleOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDecrackle(request.samples, request.sampleRate, request);
}

/**
 * Offline crackle suppressor (median or wavelet-shrinkage) for a stereo pair.
 *
 * Crackle is surface damage: the two channels carry different scratches at different instants,
 * so there is no common event for a shared decision to agree about. Each channel is decrackled
 * on its own -- there is no linking, unlike {@link masteringRepairDeclickStereo} and
 * {@link masteringRepairDeclipStereo} -- and this entry point exists to keep the reports and
 * the channel-length contract in one place.
 */
export function masteringRepairDecrackleStereo(
  request: MasteringRepairDecrackleStereoRequest,
): MasteringRepairDecrackleStereoResult;
export function masteringRepairDecrackleStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DecrackleOptions,
): MasteringRepairDecrackleStereoResult;
export function masteringRepairDecrackleStereo(
  left: Float32Array | MasteringRepairDecrackleStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DecrackleOptions = {},
): MasteringRepairDecrackleStereoResult {
  const request: MasteringRepairDecrackleStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDecrackleStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array | MasteringRepairDehumRequest,
  sampleRate?: number,
  options: DehumOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDehum(request.samples, request.sampleRate, request);
}

/**
 * Offline mains-hum remover for a stereo pair.
 *
 * With `adaptive` set, mains hum is one physical source, so the tracker reads the channel
 * mean and both cascades follow the one frequency it finds: `appliedFundamentalHz` and
 * `fundamentalDriftHz` come back identical in both reports by construction, while each
 * report's `detected` still measures that channel's own input and each channel keeps its
 * own filter state, so neither channel's transient rings through the other. With `adaptive`
 * clear, which is the default, nothing is shared and the two channels are filtered
 * independently at the configured frequency.
 */
export function masteringRepairDehumStereo(
  request: MasteringRepairDehumStereoRequest,
): MasteringRepairDehumStereoResult;
export function masteringRepairDehumStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DehumOptions,
): MasteringRepairDehumStereoResult;
export function masteringRepairDehumStereo(
  left: Float32Array | MasteringRepairDehumStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DehumOptions = {},
): MasteringRepairDehumStereoResult {
  const request: MasteringRepairDehumStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDehumStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
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

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairTrimSilence(request.samples, request.sampleRate, request);
}

/**
 * Offline silence trimmer for a stereo pair, cutting both channels to one shared range.
 *
 * Each channel is scanned on its own and the two ranges are UNIONED, so the pair keeps
 * whatever either channel calls signal and both outputs come back the same length. The scan
 * never reads a downmix: `0.5 * (left + right)` halves material carried by one channel alone,
 * which can drop it under the gate, and cancels an antiphase pair to exactly zero, which would
 * read full-level audio in both channels as silence. Trimming is destructive, so the rule errs
 * toward keeping.
 *
 * The only repair stereo entry that SHORTENS its input: `result.left.length` is the output
 * length, and the input's says nothing about it. A pair in which neither channel carries signal
 * returns two EMPTY arrays and succeeds — it does not throw and does not return null.
 *
 * `report.range` is the union that was applied to both channels. `leftRange` and `rightRange`
 * are the per-channel scans it was formed from, so a caller can see which channel decided each
 * edge; a channel carrying nothing reports an empty range and contributes nothing to the union.
 * With nothing kept, `report.range` is `(inputLength, inputLength)`, so `removedHeadSamples` is
 * the whole input and `removedTailSamples` is 0 — the two still sum to the input length and
 * only the split between the ends is arbitrary.
 *
 * Which option is live depends on `mode`: `threshold` is read ONLY by `'peak'`, and `gateLufs`
 * and `windowMs` ONLY by `'lufsGated'`. Changing an option the active mode does not read is
 * silently inert rather than an error.
 *
 * That gated mode compares an UNWEIGHTED RMS over a window centred on each sample against
 * `gateLufs`, so the figure it gates on is dBFS rather than a BS.1770 loudness, and `windowMs`
 * sizes that window and does nothing else. The window is clipped at the buffer ends, so a
 * sample near either edge is judged on a shorter one.
 *
 * `paddingSamples` widens the kept range in both directions and is clamped to the buffer, so it
 * can never reach past either end; a pass that kept nothing is not padded. A NEGATIVE count is
 * refused by name rather than absorbed into 0 or into the default — the underlying field is
 * unsigned, and a negative one would arrive as an enormous count instead.
 *
 * @example
 * ```ts
 * const { left, right, report, leftRange, rightRange } = masteringRepairTrimSilenceStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   mode: 'peak',
 *   threshold: 0.01,
 * });
 * console.log(left.length, report.removedHeadSamples, leftRange.first, rightRange.first);
 * ```
 */
export function masteringRepairTrimSilenceStereo(
  request: MasteringRepairTrimSilenceStereoRequest,
): MasteringRepairTrimSilenceStereoResult;
export function masteringRepairTrimSilenceStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: TrimSilenceOptions,
): MasteringRepairTrimSilenceStereoResult;
export function masteringRepairTrimSilenceStereo(
  left: Float32Array | MasteringRepairTrimSilenceStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: TrimSilenceOptions = {},
): MasteringRepairTrimSilenceStereoResult {
  const request: MasteringRepairTrimSilenceStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairTrimSilenceStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

// ============================================================================
// Repair detection — measure without repairing
// ============================================================================

/** Request form of `masteringRepairDetectClicks`. */
export interface MasteringRepairDetectClicksRequest extends DeclickOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectNoiseFloor`. */
export interface MasteringRepairDetectNoiseFloorRequest extends DenoiseClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairNoiseBandBins`. */
export interface MasteringRepairNoiseBandBinsRequest {
  nFft?: number;
  sampleRate?: number;
}

/** Request form of `masteringRepairDetectClipping`. */
export interface MasteringRepairDetectClippingRequest extends DeclipOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectCrackle`. */
export interface MasteringRepairDetectCrackleRequest extends DecrackleOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectHum`. */
export interface MasteringRepairDetectHumRequest extends DehumOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectReverb`. */
export interface MasteringRepairDetectReverbRequest extends DereverbClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectTrimRange`. */
export interface MasteringRepairDetectTrimRangeRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectTrimRangeStereo`. */
export interface MasteringRepairDetectTrimRangeStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Measures clicks without repairing.
 *
 * Runs the same LPC analysis {@link masteringRepairDeclick} runs, so a run counted here is one
 * the repair would act on — a cheaper threshold-only scan would report runs it leaves alone.
 * That also means the options that shape the repair shape the count: a large `rejected` says
 * `maxClickSamples` or `neighborRatio` is too tight for this material, not that the material is
 * clean.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectClicks({ samples, sampleRate: 48000 });
 * if (detected.perSecond > 1) samples = masteringRepairDeclick({ samples, sampleRate: 48000 });
 * ```
 */
export function masteringRepairDetectClicks(
  request: MasteringRepairDetectClicksRequest,
): ClickDetection;
export function masteringRepairDetectClicks(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclickOptions,
): ClickDetection;
export function masteringRepairDetectClicks(
  samples: Float32Array | MasteringRepairDetectClicksRequest,
  sampleRate?: number,
  options: DeclickOptions = {},
): ClickDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectClicks(request.samples, request.sampleRate, request);
}

/**
 * Measures the noise floor without denoising.
 *
 * Runs the STFT and the configured noise estimator — the two stages
 * {@link masteringRepairDenoiseClassical} runs — and stops before the gain mask, which is why
 * no attenuation figure appears here.
 *
 * Needs at least `nFft` samples and THROWS for a shorter buffer, the opposite of
 * {@link masteringRepairDetectReverb}, which pads one.
 *
 * `floorDbfs` is an absolute level, so it is comparable only against another figure measured
 * over the same channel count.
 */
export function masteringRepairDetectNoiseFloor(
  request: MasteringRepairDetectNoiseFloorRequest,
): NoiseDetection;
export function masteringRepairDetectNoiseFloor(
  samples: Float32Array,
  sampleRate: number,
  options?: DenoiseClassicalOptions,
): NoiseDetection;
export function masteringRepairDetectNoiseFloor(
  samples: Float32Array | MasteringRepairDetectNoiseFloorRequest,
  sampleRate?: number,
  options: DenoiseClassicalOptions = {},
): NoiseDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectNoiseFloor(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Bin boundaries of the grid {@link masteringRepairDetectNoiseFloor} reports `bandFloorDbfs` on.
 *
 * Band `k` covers the one-sided STFT bins `[bins[k], bins[k + 1])`, and bin `b` sits at
 * `b * sampleRate / nFft` Hz.
 *
 * The geometric band edges are rounded to bins, so a band narrower than the bin spacing comes
 * out EMPTY — `bins[k] === bins[k + 1]` — and its `bandFloorDbfs[k]` reads as the floor
 * sentinel because no bin landed in it, NOT because that region was quiet. Telling those two
 * apart is what this grid is for, and the rounding cannot be recovered from the band count
 * alone.
 *
 * Nothing but the analysis geometry decides the grid, so no denoise config is taken: one call
 * describes every floor measured at that `nFft` and `sampleRate`, whatever mode or estimator
 * produced it.
 *
 * @param nFft - STFT size the bins belong to; a positive power of two, the same rule
 *   {@link masteringRepairDetectNoiseFloor} applies to its config, so every grid returned here
 *   is one that entry can report on. Defaults to 1024.
 * @param sampleRate - Sample rate the bins belong to, in Hz; positive. Defaults to 22050.
 * @returns 33 bin indices, low to high — one more than the 32 bands: the first bin of every
 *   band plus the one-past-the-end bin of the last, which is `nFft / 2 + 1`. Non-decreasing.
 * @throws If `nFft` is not a positive power of two, or `sampleRate` is not positive.
 *
 * @example
 * ```ts
 * const bins = masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 48000 });
 * const floor = masteringRepairDetectNoiseFloor({ samples, sampleRate: 48000, nFft: 1024 });
 * floor.bandFloorDbfs.forEach((level, k) => {
 *   if (bins[k] === bins[k + 1]) return; // empty band: `level` is the sentinel, not a measurement
 *   console.log((bins[k] * 48000) / 1024, level);
 * });
 * ```
 */
export function masteringRepairNoiseBandBins(
  request?: MasteringRepairNoiseBandBinsRequest,
): Int32Array;
export function masteringRepairNoiseBandBins(nFft?: number, sampleRate?: number): Int32Array;
export function masteringRepairNoiseBandBins(
  nFft?: number | MasteringRepairNoiseBandBinsRequest,
  sampleRate?: number,
): Int32Array {
  // Both positional arguments are optional, so a default on `nFft` would swallow
  // an explicit `undefined` and take the request branch with `sampleRate` in hand
  // but unreachable. Discriminate on the value instead.
  const request = typeof nFft === 'object' && nFft !== null ? nFft : { nFft, sampleRate };
  return requireModule().masteringRepairNoiseBandBins(
    request.nFft ?? 1024,
    request.sampleRate ?? 22050,
  );
}

/**
 * Measures clipping without repairing.
 *
 * Counts samples at or past `clipThreshold`. No other option reaches the result — `lpcOrder`,
 * `iterations` and `lpcBlend` are validated and then unread, since nothing here is
 * reconstructed. `sampleRate` is validated without being read for the same reason: no field of
 * the result is a rate.
 *
 * Compare `longestRunSamples` against the 512-sample LPC cap to tell in advance which runs
 * {@link masteringRepairDeclip} would fill by interpolation rather than with the solver.
 *
 * `flatRunCount`, `longestFlatRunSamples`, `flatSampleCount` and `flatLevel` answer a different
 * question from the four fields above: those are read against `clipThreshold`, so they count the
 * apex of any waveform that reaches it — a full-scale sine reports thousands of "clipped" samples
 * having never been clipped — and they miss material clipped in one tool and attenuated in the
 * next, which leaves nothing at the threshold. The flat-top fields instead count runs of at least
 * 3 consecutive bit-identical samples whose level sits within 1 dB of the signal's peak, so they
 * catch a clipped-then-attenuated waveform that `sampleCount` reports as clean.
 *
 * Two opposite errors follow from what a flat top actually is. A genuinely flat-topped waveform —
 * a square or pulse train, a fully limited master — counts as clipped here too and cannot be told
 * apart from clipping in the time domain: a false positive. In the other direction, anything that
 * moves the two channels' samples independently before this runs erases a real flat top, so a
 * zero reading is not proof the material was never clipped — a stereo downmix does this, and so
 * do resampling and lossy coding, because the plateau stops being exactly level once each sample
 * is nudged on its own. **Detect each channel of a stereo signal separately, before any downmix,
 * never on the mixed-down result.**
 */
export function masteringRepairDetectClipping(
  request: MasteringRepairDetectClippingRequest,
): ClipDetection;
export function masteringRepairDetectClipping(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): ClipDetection;
export function masteringRepairDetectClipping(
  samples: Float32Array | MasteringRepairDetectClippingRequest,
  sampleRate?: number,
  options: DeclipOptions = {},
): ClipDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectClipping(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Measures crackle without repairing.
 *
 * Measured by the median criterion whatever `mode` is set to: wavelet shrinkage removes crackle
 * without ever deciding that a sample is crackle, so these counts do not describe what a
 * wavelet-mode repair would remove. A caller therefore gets the same answer before choosing a
 * mode.
 */
export function masteringRepairDetectCrackle(
  request: MasteringRepairDetectCrackleRequest,
): CrackleDetection;
export function masteringRepairDetectCrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): CrackleDetection;
export function masteringRepairDetectCrackle(
  samples: Float32Array | MasteringRepairDetectCrackleRequest,
  sampleRate?: number,
  options: DecrackleOptions = {},
): CrackleDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectCrackle(request.samples, request.sampleRate, request);
}

/**
 * Measures hum without filtering.
 *
 * Always runs the estimation path, whatever `adaptive` says: the fixed path notches the
 * configured frequency without ever looking for hum, so a detector following the flag would
 * hand back its own input. `fundamentalProminence` is the winning candidate's projected energy
 * over the median candidate, so `1.0` means no peak was found at all — it is not a lock flag.
 *
 * `harmonicDbfs` is measured at every `k*f0` the sample rate carries, not only the ones a
 * cascade would notch; a `k*f0` at or past Nyquist reads the dB floor because nothing is there
 * to measure.
 */
export function masteringRepairDetectHum(request: MasteringRepairDetectHumRequest): HumDetection;
export function masteringRepairDetectHum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): HumDetection;
export function masteringRepairDetectHum(
  samples: Float32Array | MasteringRepairDetectHumRequest,
  sampleRate?: number,
  options: DehumOptions = {},
): HumDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectHum(request.samples, request.sampleRate, request);
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

/**
 * Measures the range {@link masteringRepairTrimSilence} would keep, without trimming.
 *
 * The padding `paddingSamples` asks for is already INSIDE the returned range, so this is the
 * range the repair would cut to rather than the detected extent of the signal. A buffer with
 * nothing above the threshold reports `(length, length)`.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRange({ samples, sampleRate: 48000, threshold: 0.01 });
 * const keptSeconds = (range.lastExclusive - range.first) / 48000;
 * ```
 */
export function masteringRepairDetectTrimRange(
  request: MasteringRepairDetectTrimRangeRequest,
): TrimRange;
export function masteringRepairDetectTrimRange(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): TrimRange;
export function masteringRepairDetectTrimRange(
  samples: Float32Array | MasteringRepairDetectTrimRangeRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): TrimRange {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectTrimRange(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Measures the one range a stereo trim pass would cut both channels to.
 *
 * Each channel is scanned on its own and the two ranges are UNIONED, so the pair keeps whatever
 * either channel calls signal. A channel with nothing above the threshold contributes NO EDGE
 * rather than an edge at the buffer's end: the union of a silent channel and an active one is
 * the active channel's range exactly, where a naive `min`/`max` would push `lastExclusive` out
 * to the buffer end and keep the whole tail.
 *
 * A downmix is not read: summing to mono halves material carried by one channel alone and
 * cancels an antiphase pair outright, either of which would read full-level audio as silence.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRangeStereo({ left, right, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRangeStereo(
  request: MasteringRepairDetectTrimRangeStereoRequest,
): TrimRange;
export function masteringRepairDetectTrimRangeStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: TrimSilenceOptions,
): TrimRange;
export function masteringRepairDetectTrimRangeStereo(
  left: Float32Array | MasteringRepairDetectTrimRangeStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: TrimSilenceOptions = {},
): TrimRange {
  const request: MasteringRepairDetectTrimRangeStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDetectTrimRangeStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}
