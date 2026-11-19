/**
 * Impulsive-defect repair: declick, declip and decrackle, with their detectors.
 */

import { withEntry } from './internal/entry.js';
import { clipDetectionFromNative, declipReportFromNative } from './internal/metering_native.js';
import { pairedResult, repairNativeOptions } from './internal/repair_common.js';
import { addon } from './native.js';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  DeclickResult,
  DeclipResult,
  DecrackleResult,
  MasteringRepairDeclickResult,
  MasteringRepairDeclipResult,
  MasteringRepairDecrackleResult,
  MasteringRepairSamplesRequest,
} from './types.js';
import { assertAudioInput, requestOnly, sampleOneShot } from './validation.js';

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  /** Click detection threshold as a linear sample magnitude, in (0, 10]. Default `0.8`. */
  threshold?: number;
  /** Ratio a click edge must exceed against the sample-to-sample level beside it, in (0, 100]. Default `4`. */
  neighborRatio?: number;
  /** Longest run treated as a click, in samples, from 1 to 512. Default `8`. */
  maxClickSamples?: number;
  /** Order of the LPC model that fills a click, from 0 to 2048. Default `20`. */
  lpcOrder?: number;
  /** How far the LPC residual must stand above its typical level to mark a click, in (0, 1000]. Default `8`. */
  residualRatio?: number;
}

/** Request form of `masteringRepairDeclick`. */
export interface MasteringRepairDeclickRequest
  extends MasteringRepairSamplesRequest,
    DeclickOptions {}

/** Offline LPC-based declicker. Returns the repaired audio and what the pass did. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): DeclickResult;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclickOptions,
): DeclickResult;
export function masteringRepairDeclick(...args: unknown[]): DeclickResult {
  return withEntry('masteringRepairDeclick', () => {
    const r = sampleOneShot<MasteringRepairDeclickRequest>('masteringRepairDeclick', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.masteringRepairDeclick(r.samples, r.sampleRate, repairNativeOptions(r));
  });
}

/** Request form of `masteringRepairDeclickStereo`. */
export interface MasteringRepairDeclickStereoRequest extends DeclickOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline LPC-based declicker for a stereo pair, repairing the union of both
 * channels' detected runs.
 *
 * A run either channel's detector selects is repaired in BOTH channels, so a
 * common-mode click repaired on one side alone never moves the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own LPC model, which is why `reports[0]` and `reports[1]`
 * can genuinely differ. `linkedRuns` on either report counts the runs
 * repaired because of the OTHER channel's own detection -- 0 whenever nothing
 * was borrowed, and always 0 from {@link masteringRepairDeclick}. Merged runs
 * can exceed `maxClickSamples`: that cap governs what may be selected, not
 * how far a selection reaches once both channels agree.
 */
export function masteringRepairDeclickStereo(
  request: MasteringRepairDeclickStereoRequest,
): MasteringRepairDeclickResult;
export function masteringRepairDeclickStereo(...args: unknown[]): MasteringRepairDeclickResult {
  return withEntry('masteringRepairDeclickStereo', () => {
    const r = requestOnly<MasteringRepairDeclickStereoRequest>(
      'masteringRepairDeclickStereo',
      args,
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return pairedResult(
      addon.masteringRepairDeclickStereo(r.left, r.right, r.sampleRate, repairNativeOptions(r)),
    );
  });
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  /** Clip level in dBFS, at most 0. Default about -0.175 (0.98 linear). */
  clipThresholdDb?: number;
  /** Order of the LPC model that fills a clipped run, from 0 up. Default `36`. */
  lpcOrder?: number;
  /** Outer reconstruction rounds, from 1 to 8. Default `2`. */
  iterations?: number;
  /** Weight of the LPC prediction in the fill, in [0, 1]; the interpolation fallback gets the remainder. Default `0.65`. */
  lpcBlend?: number;
}

/** Request form of `masteringRepairDeclip`. */
export interface MasteringRepairDeclipRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  /** Detection threshold, in (0, 1000]: a deviation from the local median in `'median'` mode, a cap on the shrinkage threshold in `'waveletShrinkage'` mode. Default `0.4`. */
  threshold?: number;
  /** Repair algorithm. Default `'median'`. */
  mode?: DecrackleMode;
  /** Haar decomposition levels, from 1 to 24. Default `4`. */
  levels?: number;
}

/** Request form of `masteringRepairDecrackle`. */
export interface MasteringRepairDecrackleRequest
  extends MasteringRepairSamplesRequest,
    DecrackleOptions {}

/**
 * Offline LPC-based declipper. Returns the repaired audio and what the pass did.
 *
 * Only clipped runs of at most 512 consecutive samples are reconstructed with the LPC solver.
 * The cap is a fixed sample count: it is not derived from `lpcOrder`, from `sampleRate`, or from
 * any other option, so its duration depends on the rate (~10.7 ms at 48 kHz). A longer run is
 * filled with cubic / linear interpolation instead, which keeps the solver's dense matrices
 * bounded by the cap rather than by the input. Exceeding the cap silently changes the
 * reconstruction method rather than throwing: `lpcOrder`, `iterations` and `lpcBlend` have no
 * effect on the interpolated run.
 */
export function masteringRepairDeclip(request: MasteringRepairDeclipRequest): DeclipResult;
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): DeclipResult;
export function masteringRepairDeclip(...args: unknown[]): DeclipResult {
  return withEntry('masteringRepairDeclip', () => {
    const r = sampleOneShot<MasteringRepairDeclipRequest>('masteringRepairDeclip', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const native = addon.masteringRepairDeclip(r.samples, r.sampleRate, repairNativeOptions(r));
    return { samples: native.samples, report: declipReportFromNative(native.report) };
  });
}

/** Request form of `masteringRepairDeclipStereo`. */
export interface MasteringRepairDeclipStereoRequest extends DeclipOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline LPC-based declipper for a stereo pair, reconstructing the union of
 * both channels' clipped runs.
 *
 * Each channel reconstructs the whole of every union run it has at least one
 * clipped sample in; a channel with no clipped sample in a run is left
 * untouched there, since reconstructing unclipped audio to match the other
 * side would replace real samples with an estimate. `linkedRuns` on either
 * report counts the runs whose reconstruction reached past this channel's own
 * clipped samples because the other channel's run was wider -- a clipped
 * plateau in only one channel produces no linking at all, and it is always 0
 * from {@link masteringRepairDeclip}. See {@link masteringRepairDeclip} for
 * the 512-sample LPC-vs-interpolation cap, which applies per channel here.
 */
export function masteringRepairDeclipStereo(
  request: MasteringRepairDeclipStereoRequest,
): MasteringRepairDeclipResult;
export function masteringRepairDeclipStereo(...args: unknown[]): MasteringRepairDeclipResult {
  return withEntry('masteringRepairDeclipStereo', () => {
    const r = requestOnly<MasteringRepairDeclipStereoRequest>('masteringRepairDeclipStereo', args);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    const paired = pairedResult(
      addon.masteringRepairDeclipStereo(r.left, r.right, r.sampleRate, repairNativeOptions(r)),
    );
    return {
      channels: paired.channels,
      reports: (paired.reports as Parameters<typeof declipReportFromNative>[0][]).map(
        declipReportFromNative,
      ),
    };
  });
}

/**
 * Offline crackle suppressor (median or wavelet-shrinkage). Returns the repaired audio and what the
 * pass did.
 */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): DecrackleResult;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): DecrackleResult;
export function masteringRepairDecrackle(...args: unknown[]): DecrackleResult {
  return withEntry('masteringRepairDecrackle', () => {
    const r = sampleOneShot<MasteringRepairDecrackleRequest>('masteringRepairDecrackle', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.masteringRepairDecrackle(r.samples, r.sampleRate, repairNativeOptions(r));
  });
}

/** Request form of `masteringRepairDecrackleStereo`. */
export interface MasteringRepairDecrackleStereoRequest extends DecrackleOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/**
 * Offline crackle suppressor for a stereo pair (median or wavelet-shrinkage),
 * each channel decrackled on its own.
 *
 * Crackle is surface damage: the two channels carry different scratches at
 * different instants, so there is no common event for a shared decision to
 * agree about and neither mode carries state across channels. Unlike
 * {@link masteringRepairDeclickStereo} and {@link masteringRepairDeclipStereo},
 * no run is ever widened to match the other channel and no report field
 * counts such a widening.
 */
export function masteringRepairDecrackleStereo(
  request: MasteringRepairDecrackleStereoRequest,
): MasteringRepairDecrackleResult;
export function masteringRepairDecrackleStereo(...args: unknown[]): MasteringRepairDecrackleResult {
  return withEntry('masteringRepairDecrackleStereo', () => {
    const r = requestOnly<MasteringRepairDecrackleStereoRequest>(
      'masteringRepairDecrackleStereo',
      args,
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return pairedResult(
      addon.masteringRepairDecrackleStereo(r.left, r.right, r.sampleRate, repairNativeOptions(r)),
    );
  });
}

/** Request form of `masteringRepairDetectClicks`. */
export interface MasteringRepairDetectClicksRequest
  extends MasteringRepairSamplesRequest,
    DeclickOptions {}

/**
 * Measure clicks without repairing.
 *
 * Runs the same LPC analysis {@link masteringRepairDeclick} runs, so a run
 * counted here is one the repair would act on -- a cheaper threshold-only scan
 * would report runs it leaves alone. The options are the declicker's own, and
 * they select what is counted: a large `rejected` says the configured run
 * length or neighbour ratio is too tight for this material, not that the
 * material is clean.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectClicks({ samples, sampleRate: 48000 });
 * console.log(detected.count, detected.perSecond);
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
export function masteringRepairDetectClicks(...args: unknown[]): ClickDetection {
  return withEntry('masteringRepairDetectClicks', () => {
    const r = sampleOneShot<MasteringRepairDetectClicksRequest>(
      'masteringRepairDetectClicks',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.masteringRepairDetectClicks(r.samples, r.sampleRate, repairNativeOptions(r));
  });
}

/** Request form of `masteringRepairDetectClipping`. */
export interface MasteringRepairDetectClippingRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/**
 * Measure clipping without repairing.
 *
 * Counts samples at or past `clipThresholdDb`; no other option reaches the
 * result, so `lpcOrder`, `iterations` and `lpcBlend` are accepted and change
 * nothing -- they describe a reconstruction that does not run here.
 * `sampleRate` is validated without being read, since no field of the result
 * is a rate.
 *
 * `longestRunSamples` past 512 is the run {@link masteringRepairDeclip} would
 * fill by interpolation rather than with the LPC solver.
 *
 * `flatRunCount`, `longestFlatRunSamples`, `flatSampleCount` and `flatLevel`
 * answer a different question from the four fields above: those are read
 * against `clipThresholdDb`, so they count the apex of any waveform that
 * reaches it -- a full-scale sine reports thousands of "clipped" samples
 * having never been clipped -- and they miss material that was clipped in one
 * tool and attenuated in the next, which leaves nothing at the threshold. The
 * flat fields instead find runs of at least 3 consecutive bit-identical
 * samples (at or above -40 dBFS) within 1 dB of the flat level -- the largest
 * run level once the two highest runs are set aside, so louder unclipped audio
 * cannot move it -- so they survive a gain change and
 * do not fire on a sine: measured, a clipped tone attenuated to 0.25 reports
 * `clippedSamples: 0` alongside `flatRunCount: 440` and `flatLevel: 0.25`, while
 * an unclipped full-scale sine reports `clippedSamples: 2820` alongside
 * `flatRunCount: 0`. A genuinely flat-topped waveform -- a square or pulse
 * train, a fully limited master -- counts as clipped here too and cannot be
 * told apart from real clipping in the time domain: the false positive.
 *
 * The reverse error is the one to plan around: anything that moves samples
 * independently erases a real flat top, so `flatRunCount: 0` is not proof the
 * material was never clipped. Resampling and lossy coding both do this, and so
 * does averaging a stereo pair into mono before calling this entry -- the two
 * channels are not bit-identical, so a plateau that is level in each channel
 * stops being level once they are summed. Detect each channel on its own and
 * combine the reports; do not detect on a downmix.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectClipping({ samples, sampleRate: 48000 });
 * console.log(detected.clippedSamples, detected.runCount);
 * ```
 */
export function masteringRepairDetectClipping(
  request: MasteringRepairDetectClippingRequest,
): ClipDetection;
export function masteringRepairDetectClipping(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): ClipDetection;
export function masteringRepairDetectClipping(...args: unknown[]): ClipDetection {
  return withEntry('masteringRepairDetectClipping', () => {
    const r = sampleOneShot<MasteringRepairDetectClippingRequest>(
      'masteringRepairDetectClipping',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return clipDetectionFromNative(
      addon.masteringRepairDetectClipping(r.samples, r.sampleRate, repairNativeOptions(r)),
    );
  });
}

/** Request form of `masteringRepairDetectCrackle`. */
export interface MasteringRepairDetectCrackleRequest
  extends MasteringRepairSamplesRequest,
    DecrackleOptions {}

/**
 * Measure crackle without repairing.
 *
 * Measured by the median criterion whatever `mode` is set to: wavelet
 * shrinkage removes crackle without ever deciding a sample is crackle, so
 * these counts do not describe what `'waveletShrinkage'` would repair.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectCrackle({ samples, sampleRate: 48000 });
 * console.log(detected.sampleCount, detected.perSecond);
 * ```
 */
export function masteringRepairDetectCrackle(
  request: MasteringRepairDetectCrackleRequest,
): CrackleDetection;
export function masteringRepairDetectCrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): CrackleDetection;
export function masteringRepairDetectCrackle(...args: unknown[]): CrackleDetection {
  return withEntry('masteringRepairDetectCrackle', () => {
    const r = sampleOneShot<MasteringRepairDetectCrackleRequest>(
      'masteringRepairDetectCrackle',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.masteringRepairDetectCrackle(r.samples, r.sampleRate, repairNativeOptions(r));
  });
}
