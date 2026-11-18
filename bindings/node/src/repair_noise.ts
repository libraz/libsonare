/**
 * Broadband and tonal noise repair: denoise and dehum, with their detectors.
 */

import {
  assertRepairGeometry,
  dehumReport,
  denoiseReport,
  humDetection,
  noiseDetection,
  pairedResult,
  repairNativeOptions,
  sharedReportResult,
} from './internal/repair_common.js';
import { addon } from './native.js';
import type {
  DehumResult,
  DenoiseResult,
  HumDetection,
  MasteringRepairDehumResult,
  MasteringRepairDenoiseClassicalResult,
  MasteringRepairSamplesRequest,
  NoiseDetection,
} from './types.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertPositiveInteger,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra' | 'spp';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  /** Gain estimator. Default `'logMmse'`. */
  mode?: DenoiseClassicalMode;
  /** Noise PSD estimator. Default `'quantile'`. */
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  /** STFT size in samples; a power of two. Default `1024`. */
  nFft?: number;
  /** Hop in samples, in (0, nFft / 2]. */
  hopLength?: number;
  /** Decision-directed a priori SNR smoothing factor, in [0, 1). Default `0.98`. */
  ddAlpha?: number;
  /** Deepest attenuation applied to any bin, in dB, from 0 to 120. Default `26`. */
  reductionDb?: number;
  /** Over-subtraction factor, from 0 to 16; read only by `'spectralSubtraction'`. Default `2`. */
  overSubtraction?: number;
  /** Floor multiplier relative to the noise PSD, in [0, 1]; read only by `'spectralSubtraction'`. Default `0.05`. */
  spectralFloor?: number;
  /** Fraction of frames taken as noise-only when estimating the noise spectrum, in (0, 1]. Default `0.1`. */
  noiseEstimationQuantile?: number;
  /** Whether the speech-presence gain is applied. Default `true`. */
  speechPresenceGain?: boolean;
  /** Whether the gain mask is smoothed. Default `true`. */
  gainSmoothing?: boolean;
}

/** Request form of `masteringRepairDenoiseClassical`. */
export interface MasteringRepairDenoiseClassicalRequest
  extends MasteringRepairSamplesRequest,
    DenoiseClassicalOptions {}

/**
 * Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). Returns the
 * repaired audio and what the pass did.
 */
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
  assertRepairGeometry('masteringRepairDenoiseClassical', r);
  return addon.masteringRepairDenoiseClassical(r.samples, r.sampleRate, repairNativeOptions(r));
}

/** Request form of `masteringRepairDenoiseClassicalStereo`. */
export interface MasteringRepairDenoiseClassicalStereoRequest extends DenoiseClassicalOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline STFT-domain classical denoiser for a stereo pair, driven by one
 * channel-linked gain mask.
 *
 * The mask is built from the channel-summed power and applied unchanged to
 * both channels, so the pass cannot move an interchannel level or phase
 * difference. That is also why the result carries a single report rather
 * than one per channel: a pair would be two copies of one measurement and
 * would read as though the two could differ.
 *
 * `report.detected` is therefore a *pair-level* measurement. Its levels are
 * absolute dBFS taken on the channel-summed power, so two identical channels
 * read about 3 dB above the same material through
 * {@link masteringRepairDenoiseClassical}. A stereo floor is comparable only
 * against another stereo floor, never against a mono one.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back
 * to the input length, as {@link masteringRepairDereverbClassicalStereo} does.
 *
 * Which options are live depends on `mode`: `overSubtraction` and
 * `spectralFloor` are read only by `spectralSubtraction`, and
 * `speechPresenceGain` and `gainSmoothing` only by the other two, so at the
 * default `logMmse` the first pair does nothing.
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
  assertRepairGeometry('masteringRepairDenoiseClassicalStereo', r);
  return sharedReportResult(
    addon.masteringRepairDenoiseClassicalStereo(
      r.left,
      r.right,
      r.sampleRate,
      repairNativeOptions(r),
    ),
    denoiseReport,
  );
}

/** Request form of `masteringRepairDenoiseClassicalLinked`. */
export interface MasteringRepairDenoiseClassicalLinkedRequest extends DenoiseClassicalOptions {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline STFT-domain classical denoiser for any number of channels, driven by
 * one channel-linked gain mask.
 *
 * The N-channel form of {@link masteringRepairDenoiseClassicalStereo}, carrying
 * the same guarantee for the whole set: the mask is built from the
 * channel-summed power and applied unchanged to every channel, so no
 * interchannel level or phase difference moves however many channels there are.
 * That is also why the result carries a single report rather than one per
 * channel. One channel reproduces {@link masteringRepairDenoiseClassical}
 * sample for sample, and two reproduce
 * {@link masteringRepairDenoiseClassicalStereo} plane for plane, `channels[0]`
 * being the left.
 *
 * `report.detected` is a measurement of the SET and its levels are absolute
 * dBFS taken on the summed power, so N identical channels read `10*log10(N)` dB
 * above one of them alone — about 3 dB at two channels and 4.77 dB at three.
 * The attenuation figures on the report are fractions and do not move. Compare a
 * floor only against another measured over the same number of channels.
 *
 * An input shorter than `nFft` is padded for analysis and the output trimmed back
 * to the input length, as {@link masteringRepairDereverbClassicalLinked} does.
 *
 * Every channel must be the same length: the library takes one length for the
 * set, so a disagreement is refused here rather than silently truncated.
 *
 * Which options are live depends on `mode`: `overSubtraction` and
 * `spectralFloor` are read only by `spectralSubtraction`, and
 * `speechPresenceGain` and `gainSmoothing` only by the other two, so at the
 * default `logMmse` the first pair does nothing.
 *
 * @example
 * ```ts
 * declare const centre: Float32Array;
 * const { channels, reports } = masteringRepairDenoiseClassicalLinked({
 *   channels: [left, right, centre],
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
  assertRepairGeometry('masteringRepairDenoiseClassicalLinked', r);
  return sharedReportResult(
    addon.masteringRepairDenoiseClassicalLinked(r.channels, r.sampleRate, repairNativeOptions(r)),
    denoiseReport,
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
export interface DehumOptions {
  /** Hum fundamental in Hz, in (0, 5000]. Default `50`. */
  fundamentalHz?: number;
  /** Number of harmonics removed, from 1 to 16. Default `4`. */
  harmonics?: number;
  /** Selectivity of the removal, in (0, 100]: the notch Q in `'notch'` mode, the cancellation rate in `'subtract'` mode. Default `20`. */
  q?: number;
  /** Track the fundamental within `searchRangeHz` instead of holding it fixed. Default `false`. */
  adaptive?: boolean;
  /** Half-width of the fundamental search in Hz, from 0 to 100. Default `2`. */
  searchRangeHz?: number;
  /** Adaptation rate of the tracker, in [0, 1]. Default `0.25`. */
  adaptation?: number;
  /** Tracking frame length in samples, from 16 to 16384. Default `2048`. */
  frameSize?: number;
  /** PLL loop bandwidth as a fraction of the tracked frequency, in [0, 1]; adaptive tracking only. Default `0.01`. */
  pllBandwidth?: number;
  /** Defaults to `'subtract'`. */
  mode?: DehumMode;
}

/** Request form of `masteringRepairDehum`. */
export interface MasteringRepairDehumRequest extends MasteringRepairSamplesRequest, DehumOptions {}

/** Offline mains-hum remover. Returns the repaired audio and what the pass did. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): DehumResult;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): DehumResult;
export function masteringRepairDehum(...args: unknown[]): DehumResult {
  const r = sampleOneShot<MasteringRepairDehumRequest>('masteringRepairDehum', args);
  assertAudioInput('masteringRepairDehum', r.samples, r.sampleRate, r);
  return addon.masteringRepairDehum(r.samples, r.sampleRate, repairNativeOptions(r));
}

/** Request form of `masteringRepairDehumStereo`. */
export interface MasteringRepairDehumStereoRequest extends DehumOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline mains-hum remover for a stereo pair.
 *
 * With `adaptive` set, mains hum is one physical source: the tracker reads
 * the channel mean and both cascades follow the one frequency it finds, so
 * `reports[0].appliedFundamentalHz` and `reports[1].appliedFundamentalHz`
 * (and `fundamentalDriftHz`) are identical by construction, even when the two
 * channels carry different hum. Only the frequency is shared -- each channel
 * keeps its own filter state, and each report's `detected` still measures
 * that channel's own input, so `detected.fundamentalHz` can differ between
 * channels while `appliedFundamentalHz` agrees. Without `adaptive`, which is
 * the default, nothing is shared and the two channels are filtered
 * independently at the configured frequency; `fundamentalDriftHz` is then
 * exactly 0, the measurement rather than an unset field.
 */
export function masteringRepairDehumStereo(
  request: MasteringRepairDehumStereoRequest,
): MasteringRepairDehumResult;
export function masteringRepairDehumStereo(...args: unknown[]): MasteringRepairDehumResult {
  const r = requestOnly<MasteringRepairDehumStereoRequest>('masteringRepairDehumStereo', args);
  assertAudioInput('masteringRepairDehumStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringRepairDehumStereo', r.right, r.sampleRate, r, 'right');
  return pairedResult(
    addon.masteringRepairDehumStereo(r.left, r.right, r.sampleRate, repairNativeOptions(r)),
    dehumReport,
  );
}

/** Request form of `masteringRepairDetectNoiseFloor`. */
export interface MasteringRepairDetectNoiseFloorRequest
  extends MasteringRepairSamplesRequest,
    DenoiseClassicalOptions {}

/**
 * Measure the noise floor without denoising.
 *
 * Runs the STFT and the configured noise estimator -- the two stages
 * {@link masteringRepairDenoiseClassical} runs -- and stops before the gain
 * mask, which is why the attenuation figures are not here: nothing was
 * attenuated.
 *
 * A buffer shorter than `nFft` is padded for analysis, as
 * {@link masteringRepairDetectReverb} pads one. The levels are absolute
 * dBFS, so they are comparable only against another measurement of the same
 * channel count.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectNoiseFloor({ samples, sampleRate: 48000 });
 * console.log(detected.floorDbfs, detected.bandFloorDbfs.length);
 * ```
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
  assertRepairGeometry('masteringRepairDetectNoiseFloor', r);
  return noiseDetection(
    addon.masteringRepairDetectNoiseFloor(r.samples, r.sampleRate, repairNativeOptions(r)),
  );
}

/** Request form of `masteringRepairNoiseBandBins`. */
export interface MasteringRepairNoiseBandBinsRequest {
  /**
   * FFT size the bins belong to; a positive power of two, the rule
   * {@link masteringRepairDetectNoiseFloor} applies to its own `nFft`.
   *
   * @defaultValue 1024
   */
  nFft?: number;
  /**
   * Sample rate in Hz the bins belong to; a positive integer. Required.
   */
  sampleRate: number;
}

/**
 * Bin boundaries of the grid {@link masteringRepairDetectNoiseFloor} reports
 * `bandFloorDbfs` on.
 *
 * Band `k` covers the one-sided STFT bins `[bins[k], bins[k + 1])`, and bin `b`
 * sits at `b * sampleRate / nFft` Hz. The returned array is one longer than the
 * band count, so the last entry is the one-past-the-end bin of the top band.
 *
 * The geometric band edges are rounded to bins, so a band narrower than the bin
 * spacing comes out EMPTY -- `bins[k] === bins[k + 1]` -- and its
 * `bandFloorDbfs[k]` is the floor sentinel because no bin landed in it, not
 * because that region was quiet. Telling those two apart is what this entry is
 * for; nothing in the detection result distinguishes them. Empty bands are
 * confined to the low end, where the geometric edges are closest together, but
 * they are not a run starting at band 0 -- the lowest edge is clamped to the DC
 * bin, which usually leaves band 0 holding it. Test each band rather than
 * scanning until the first non-empty one. How many there are depends on both
 * `nFft` and `sampleRate`, since the grid's top edge is Nyquist.
 *
 * Only the analysis geometry decides the grid, so no denoise options are taken
 * and no audio is read.
 *
 * @example
 * ```ts
 * const bins = masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 48000 });
 * const detected = masteringRepairDetectNoiseFloor({ samples, sampleRate: 48000 });
 * const measured = detected.bandFloorDbfs.filter((_, k) => bins[k] < bins[k + 1]);
 * ```
 */
export function masteringRepairNoiseBandBins(
  request: MasteringRepairNoiseBandBinsRequest,
): Int32Array;
export function masteringRepairNoiseBandBins(...args: unknown[]): Int32Array {
  const r = requestOnly<MasteringRepairNoiseBandBinsRequest>('masteringRepairNoiseBandBins', args);
  // Geometry only, no audio read: the core requires sample_rate > 0, not the audio-analysis range.
  if (typeof r.sampleRate !== 'number') {
    throw new TypeError('masteringRepairNoiseBandBins: sampleRate must be a number');
  }
  assertPositiveInteger('masteringRepairNoiseBandBins', r.sampleRate, 'sampleRate');
  assertRepairGeometry('masteringRepairNoiseBandBins', r);
  return addon.masteringRepairNoiseBandBins({ nFft: r.nFft, sampleRate: r.sampleRate });
}

/** Request form of `masteringRepairDetectHum`. */
export interface MasteringRepairDetectHumRequest
  extends MasteringRepairSamplesRequest,
    DehumOptions {}

/**
 * Measure mains hum without filtering.
 *
 * Always measured through the estimation path, whatever `adaptive` is set to:
 * the fixed path notches the configured frequency without ever looking for
 * hum, so a detector following the flag would hand back its own input.
 * `fundamentalProminence` is the winning candidate's energy over the median
 * candidate (a linear ratio, not dB) -- 1.0 means no peak: none was found, or the winner was not a stationary line through most analysis frames (a repeating kick reads 1.0);
 * it is not a lock flag.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectHum({ samples, sampleRate: 48000, fundamentalHz: 60 });
 * console.log(detected.fundamentalHz, detected.fundamentalProminence);
 * ```
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
  return humDetection(
    addon.masteringRepairDetectHum(r.samples, r.sampleRate, repairNativeOptions(r)),
  );
}
