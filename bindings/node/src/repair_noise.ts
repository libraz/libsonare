/**
 * Broadband and tonal noise repair: denoise and dehum, with their detectors.
 */

import { assertRepairGeometry, type MasteringRepairSamplesRequest } from './_repair_common.js';
import { addon } from './native.js';
import type {
  DehumStereoResult,
  DenoiseLinkedResult,
  DenoiseStereoResult,
  HumDetection,
  NoiseDetection,
} from './types.js';
import { assertPositiveInteger, assertSampleRate } from './validation.js';

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

export interface MasteringRepairDenoiseClassicalRequest
  extends MasteringRepairSamplesRequest,
    DenoiseClassicalOptions {}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate?: number,
  options?: DenoiseClassicalOptions,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array | MasteringRepairDenoiseClassicalRequest,
  sampleRate = 22050,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDenoiseClassical', resolvedSampleRate);
  assertRepairGeometry('masteringRepairDenoiseClassical', request);
  return addon.masteringRepairDenoiseClassical(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDenoiseClassicalStereo`. */
export interface MasteringRepairDenoiseClassicalStereoRequest extends DenoiseClassicalOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline STFT-domain classical denoiser for a stereo pair, driven by one
 * channel-linked gain mask.
 *
 * The mask is built from the channel-summed power and applied unchanged to
 * both channels, so the pass cannot move an interchannel level or phase
 * difference. That is also why the result carries a single `report` rather
 * than one per channel: a pair would be two copies of one measurement and
 * would read as though the two could differ.
 *
 * `report.detected` is therefore a *pair-level* measurement. Its levels are
 * absolute dBFS taken on the channel-summed power, so two identical channels
 * read about 3 dB above the same material through
 * {@link masteringRepairDenoiseClassical}. A stereo floor is comparable only
 * against another stereo floor, never against a mono one.
 *
 * Needs at least `nFft` samples and REJECTS a shorter input, which is the
 * opposite of {@link masteringRepairDereverbClassicalStereo} — that one pads.
 *
 * Which options are live depends on `mode`: `overSubtraction` and
 * `spectralFloor` are read only by `spectralSubtraction`, and
 * `speechPresenceGain` and `gainSmoothing` only by the other two, so at the
 * default `logMmse` the first pair does nothing.
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
): DenoiseStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDenoiseClassicalStereo', resolvedSampleRate);
  assertRepairGeometry('masteringRepairDenoiseClassicalStereo', request);
  return addon.masteringRepairDenoiseClassicalStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
}

/** Request form of `masteringRepairDenoiseClassicalLinked`. */
export interface MasteringRepairDenoiseClassicalLinkedRequest extends DenoiseClassicalOptions {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  sampleRate?: number;
}

/**
 * Offline STFT-domain classical denoiser for any number of channels, driven by
 * one channel-linked gain mask.
 *
 * The N-channel form of {@link masteringRepairDenoiseClassicalStereo}, carrying
 * the same guarantee for the whole set: the mask is built from the
 * channel-summed power and applied unchanged to every channel, so no
 * interchannel level or phase difference moves however many channels there are.
 * That is also why the result carries a single `report` rather than one per
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
 * Needs at least `nFft` samples and REJECTS a shorter input, which is the
 * opposite of {@link masteringRepairDereverbClassicalLinked} — that one pads.
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
 * const { channels, report } = masteringRepairDenoiseClassicalLinked({
 *   channels: [left, right, centre],
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(channels.length, report.detected.floorDbfs);
 * ```
 */
export function masteringRepairDenoiseClassicalLinked(
  request: MasteringRepairDenoiseClassicalLinkedRequest,
): DenoiseLinkedResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDenoiseClassicalLinked', resolvedSampleRate);
  assertRepairGeometry('masteringRepairDenoiseClassicalLinked', request);
  return addon.masteringRepairDenoiseClassicalLinked(request.channels, resolvedSampleRate, request);
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

export interface MasteringRepairDehumRequest extends MasteringRepairSamplesRequest, DehumOptions {}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate?: number,
  options?: DehumOptions,
): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array | MasteringRepairDehumRequest,
  sampleRate = 22050,
  options: DehumOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDehum', resolvedSampleRate);
  return addon.masteringRepairDehum(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDehumStereo`. */
export interface MasteringRepairDehumStereoRequest extends DehumOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline mains-hum remover for a stereo pair.
 *
 * With `adaptive` set, mains hum is one physical source: the tracker reads
 * the channel mean and both cascades follow the one frequency it finds, so
 * `leftReport.appliedFundamentalHz` and `rightReport.appliedFundamentalHz`
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
): DehumStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDehumStereo', resolvedSampleRate);
  return addon.masteringRepairDehumStereo(request.left, request.right, resolvedSampleRate, request);
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
 * Needs at least `nFft` samples and REJECTS a shorter buffer, unlike
 * {@link masteringRepairDetectReverb}, which pads one. The levels are absolute
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
): NoiseDetection {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectNoiseFloor', resolvedSampleRate);
  assertRepairGeometry('masteringRepairDetectNoiseFloor', request);
  return addon.masteringRepairDetectNoiseFloor(request.samples, resolvedSampleRate, request);
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
   * Sample rate the bins belong to; positive.
   *
   * @defaultValue 22050
   */
  sampleRate?: number;
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
): Int32Array {
  // Geometry only, no audio read: the core requires sample_rate > 0, not the
  // [8000, 384000] audio-analysis bound the other repair entries carry. Only
  // what the caller supplied, because the whole request travels to the addon's
  // own options reader -- resolving a default here would check this file's guess
  // at one the reader owns.
  if (request.sampleRate !== undefined) {
    assertPositiveInteger('masteringRepairNoiseBandBins', request.sampleRate, 'sampleRate');
  }
  assertRepairGeometry('masteringRepairNoiseBandBins', request);
  return addon.masteringRepairNoiseBandBins(request);
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
 * candidate -- 1.0 means no peak was found at all, and it is not a lock flag.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectHum({ samples, sampleRate: 48000, fundamentalHz: 60 });
 * console.log(detected.fundamentalHz, detected.fundamentalProminence);
 * ```
 */
export function masteringRepairDetectHum(request: MasteringRepairDetectHumRequest): HumDetection {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectHum', resolvedSampleRate);
  return addon.masteringRepairDetectHum(request.samples, resolvedSampleRate, request);
}
