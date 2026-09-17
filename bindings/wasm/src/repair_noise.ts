/**
 * Broadband and tonal noise repair: denoise and dehum, with their detectors.
 */

import { getSonareModule } from './module_state';
import type {
  HumDetection,
  MasteringRepairDehumStereoResult,
  MasteringRepairDenoiseClassicalLinkedResult,
  MasteringRepairDenoiseClassicalStereoResult,
  NoiseDetection,
} from './public_types_repair';

function requireModule() {
  return getSonareModule();
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

/** Request form of `masteringRepairDetectHum`. */
export interface MasteringRepairDetectHumRequest extends DehumOptions {
  samples: Float32Array;
  sampleRate: number;
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
