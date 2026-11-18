/**
 * Broadband and tonal noise repair: denoise and dehum, with their detectors.
 */

import { pairedResult, repairNativeOptions, sharedReportResult } from './internal/repair_common.js';
import { getSonareModule } from './module_state.js';
import type {
  DehumResult,
  DenoiseResult,
  HumDetection,
  MasteringRepairDehumResult,
  MasteringRepairDenoiseClassicalResult,
  NoiseDetection,
} from './public_types_repair.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertPositiveInteger,
  requestOnly,
  sampleOneShot,
  type ValidateOptions,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra' | 'spp';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions extends ValidateOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  /** Hop in samples, in (0, nFft / 2]. */
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
  sampleRate: number;
}

/** Request form of `masteringRepairDenoiseClassicalLinked`. */
export interface MasteringRepairDenoiseClassicalLinkedRequest extends DenoiseClassicalOptions {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  sampleRate: number;
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): DenoiseResult;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DenoiseClassicalOptions,
): DenoiseResult;
export function masteringRepairDenoiseClassical(...args: unknown[]): DenoiseResult {
  const r = sampleOneShot<MasteringRepairDenoiseClassicalRequest>(
    'masteringRepairDenoiseClassical',
    args,
  );
  assertAudioInput('masteringRepairDenoiseClassical', r.samples, r.sampleRate, r);
  return requireModule().masteringRepairDenoiseClassical(
    r.samples,
    r.sampleRate,
    repairNativeOptions(r),
  );
}

/**
 * Offline STFT-domain classical denoiser for a stereo pair, driven by one channel-linked
 * gain mask.
 *
 * The mask is built from the channel-summed power and applied unchanged to both channels, so
 * the pass cannot move an interchannel level or phase difference. That is also why the result
 * carries a single report rather than one per channel: a pair would be two copies of one
 * measurement and would read as though the two could differ.
 *
 * `reports[0].detected` is therefore a *pair-level* measurement, and the only absolute one in the
 * result. Its levels are dBFS on the channel-summed power, so two identical channels read
 * `10*log10(2)` — about 3.01 dB — above the same material through
 * {@link masteringRepairDenoiseClassical}. A stereo floor is comparable only against another
 * stereo floor, never against a mono one.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back to the input
 * length, as {@link masteringRepairDereverbClassicalStereo} does.
 *
 * Which options are live depends on `mode`: `overSubtraction` and `spectralFloor` are read
 * only by `spectralSubtraction`, and `speechPresenceGain` and `gainSmoothing` only by the
 * other two, so at the default `logMmse` the first pair does nothing.
 *
 * @example
 * ```ts
 * const { channels, reports } = masteringRepairDenoiseClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(reports[0].detected.floorDbfs, reports[0].meanReductionDb);
 * ```
 */
export function masteringRepairDenoiseClassicalStereo(
  request: MasteringRepairDenoiseClassicalStereoRequest,
): MasteringRepairDenoiseClassicalResult;
export function masteringRepairDenoiseClassicalStereo(
  ...args: unknown[]
): MasteringRepairDenoiseClassicalResult {
  const r = requestOnly<MasteringRepairDenoiseClassicalStereoRequest>(
    'masteringRepairDenoiseClassicalStereo',
    args,
  );
  assertAudioInput('masteringRepairDenoiseClassicalStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringRepairDenoiseClassicalStereo', r.right, r.sampleRate, r, 'right');
  return sharedReportResult(
    requireModule().masteringRepairDenoiseClassicalStereo(
      r.left,
      r.right,
      r.sampleRate,
      repairNativeOptions(r),
    ),
  );
}

/**
 * Offline STFT-domain classical denoiser for any number of channels, driven by one
 * channel-linked gain mask.
 *
 * The N-channel form of {@link masteringRepairDenoiseClassicalStereo}, carrying the same
 * guarantee over the whole set: the mask is built from the channel-summed power and applied
 * unchanged to every channel, so no interchannel level or phase difference moves however many
 * channels there are. One report for the set, and one output per input channel in input order.
 *
 * A single channel reproduces {@link masteringRepairDenoiseClassical} bit for bit, and two
 * reproduce {@link masteringRepairDenoiseClassicalStereo} plane for plane — `channels[0]` is the
 * left plane and `channels[1]` the right.
 *
 * `reports[0].detected` carries absolute levels and they are the SET's: the floor is referred to the
 * summed mean square of every channel, so N identical channels read `10*log10(N)` above
 * one of them — about 3.01 dB at two channels and 4.77 dB at three. Compare a floor only against
 * one measured over the same number of channels. Every other field of the report is a fraction
 * and does not move with the channel count.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back to the input
 * length, as {@link masteringRepairDereverbClassicalLinked} does.
 *
 * @example
 * ```ts
 * const { channels, reports } = masteringRepairDenoiseClassicalLinked({
 *   channels: [frontLeft, frontRight, centre],
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(channels.length, reports[0].detected.floorDbfs);
 * ```
 */
export function masteringRepairDenoiseClassicalLinked(
  request: MasteringRepairDenoiseClassicalLinkedRequest,
): MasteringRepairDenoiseClassicalResult;
export function masteringRepairDenoiseClassicalLinked(
  ...args: unknown[]
): MasteringRepairDenoiseClassicalResult {
  const r = requestOnly<MasteringRepairDenoiseClassicalLinkedRequest>(
    'masteringRepairDenoiseClassicalLinked',
    args,
  );
  assertAudioChannels('masteringRepairDenoiseClassicalLinked', r.channels, r.sampleRate, r);
  return sharedReportResult(
    requireModule().masteringRepairDenoiseClassicalLinked(
      r.channels,
      r.sampleRate,
      repairNativeOptions(r),
    ),
  );
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
export interface DehumOptions extends ValidateOptions {
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
  sampleRate: number;
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): DehumResult;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): DehumResult;
export function masteringRepairDehum(...args: unknown[]): DehumResult {
  const r = sampleOneShot<MasteringRepairDehumRequest>('masteringRepairDehum', args);
  assertAudioInput('masteringRepairDehum', r.samples, r.sampleRate, r);
  return requireModule().masteringRepairDehum(r.samples, r.sampleRate, repairNativeOptions(r));
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
): MasteringRepairDehumResult;
export function masteringRepairDehumStereo(...args: unknown[]): MasteringRepairDehumResult {
  const r = requestOnly<MasteringRepairDehumStereoRequest>('masteringRepairDehumStereo', args);
  assertAudioInput('masteringRepairDehumStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringRepairDehumStereo', r.right, r.sampleRate, r, 'right');
  return pairedResult(
    requireModule().masteringRepairDehumStereo(
      r.left,
      r.right,
      r.sampleRate,
      repairNativeOptions(r),
    ),
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
  sampleRate: number;
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
 * A buffer shorter than `nFft` is padded for analysis, as {@link masteringRepairDetectReverb}
 * pads one.
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
export function masteringRepairDetectNoiseFloor(...args: unknown[]): NoiseDetection {
  const r = sampleOneShot<MasteringRepairDetectNoiseFloorRequest>(
    'masteringRepairDetectNoiseFloor',
    args,
  );
  assertAudioInput('masteringRepairDetectNoiseFloor', r.samples, r.sampleRate, r);
  return requireModule().masteringRepairDetectNoiseFloor(
    r.samples,
    r.sampleRate,
    repairNativeOptions(r),
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
 * @param request - `nFft` is the STFT size the bins belong to (a positive power of two, the
 *   same rule {@link masteringRepairDetectNoiseFloor} applies; default 1024). `sampleRate` is
 *   required, in Hz, a positive integer.
 * @returns 33 bin indices, low to high — one more than the 32 bands: the first bin of every
 *   band plus the one-past-the-end bin of the last, which is `nFft / 2 + 1`. Non-decreasing.
 * @throws `TypeError` for a missing `sampleRate`; `RangeError` if `nFft` is not a positive power of two or `sampleRate` is not positive.
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
  request: MasteringRepairNoiseBandBinsRequest,
): Int32Array;
export function masteringRepairNoiseBandBins(...args: unknown[]): Int32Array {
  const r = requestOnly<MasteringRepairNoiseBandBinsRequest>('masteringRepairNoiseBandBins', args);
  // Geometry only: the core needs a positive rate, not the audio-analysis range.
  if (typeof r.sampleRate !== 'number') {
    throw new TypeError('masteringRepairNoiseBandBins: sampleRate must be a number');
  }
  assertPositiveInteger('masteringRepairNoiseBandBins', r.sampleRate, 'sampleRate');
  return requireModule().masteringRepairNoiseBandBins(r.nFft ?? 1024, r.sampleRate);
}

/**
 * Measures hum without filtering.
 *
 * Always runs the estimation path, whatever `adaptive` says: the fixed path notches the
 * configured frequency without ever looking for hum, so a detector following the flag would
 * hand back its own input. `fundamentalProminence` is the winning candidate's projected energy
 * over the median candidate (a linear ratio, not dB), so `1.0` means no peak: none was found, or the winner was not a stationary line through most analysis frames (a repeating kick reads 1.0). It is not a lock flag.
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
export function masteringRepairDetectHum(...args: unknown[]): HumDetection {
  const r = sampleOneShot<MasteringRepairDetectHumRequest>('masteringRepairDetectHum', args);
  assertAudioInput('masteringRepairDetectHum', r.samples, r.sampleRate, r);
  return requireModule().masteringRepairDetectHum(r.samples, r.sampleRate, repairNativeOptions(r));
}
