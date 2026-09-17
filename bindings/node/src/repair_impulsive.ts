/**
 * Impulsive-defect repair: declick, declip and decrackle, with their detectors.
 */

import type { MasteringRepairSamplesRequest } from './_repair_common.js';
import { addon } from './native.js';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  DeclickStereoResult,
  DeclipStereoResult,
  DecrackleStereoResult,
} from './types.js';
import { assertSampleRate } from './validation.js';

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}

export interface MasteringRepairDeclickRequest
  extends MasteringRepairSamplesRequest,
    DeclickOptions {}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate?: number,
  options?: DeclickOptions,
): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array | MasteringRepairDeclickRequest,
  sampleRate = 22050,
  options: DeclickOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDeclick', resolvedSampleRate);
  return addon.masteringRepairDeclick(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDeclickStereo`. */
export interface MasteringRepairDeclickStereoRequest extends DeclickOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declicker for a stereo pair, repairing the union of both
 * channels' detected runs.
 *
 * A run either channel's detector selects is repaired in BOTH channels, so a
 * common-mode click repaired on one side alone never moves the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own LPC model, which is why `leftReport` and `rightReport`
 * can genuinely differ. `linkedRuns` on either report counts the runs
 * repaired because of the OTHER channel's own detection -- 0 whenever nothing
 * was borrowed, and always 0 from {@link masteringRepairDeclick}. Merged runs
 * can exceed `maxClickSamples`: that cap governs what may be selected, not
 * how far a selection reaches once both channels agree.
 */
export function masteringRepairDeclickStereo(
  request: MasteringRepairDeclickStereoRequest,
): DeclickStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDeclickStereo', resolvedSampleRate);
  return addon.masteringRepairDeclickStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}

export interface MasteringRepairDeclipRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}

export interface MasteringRepairDecrackleRequest
  extends MasteringRepairSamplesRequest,
    DecrackleOptions {}

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
  sampleRate?: number,
  options?: DeclipOptions,
): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array | MasteringRepairDeclipRequest,
  sampleRate = 22050,
  options: DeclipOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDeclip', resolvedSampleRate);
  return addon.masteringRepairDeclip(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDeclipStereo`. */
export interface MasteringRepairDeclipStereoRequest extends DeclipOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
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
): DeclipStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDeclipStereo', resolvedSampleRate);
  return addon.masteringRepairDeclipStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate?: number,
  options?: DecrackleOptions,
): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array | MasteringRepairDecrackleRequest,
  sampleRate = 22050,
  options: DecrackleOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDecrackle', resolvedSampleRate);
  return addon.masteringRepairDecrackle(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDecrackleStereo`. */
export interface MasteringRepairDecrackleStereoRequest extends DecrackleOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
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
): DecrackleStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDecrackleStereo', resolvedSampleRate);
  return addon.masteringRepairDecrackleStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
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
): ClickDetection {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectClicks', resolvedSampleRate);
  return addon.masteringRepairDetectClicks(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDetectClipping`. */
export interface MasteringRepairDetectClippingRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/**
 * Measure clipping without repairing.
 *
 * Counts samples at or past `clipThreshold`; no other option reaches the
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
 * against `clipThreshold`, so they count the apex of any waveform that
 * reaches it -- a full-scale sine reports thousands of "clipped" samples
 * having never been clipped -- and they miss material that was clipped in one
 * tool and attenuated in the next, which leaves nothing at the threshold. The
 * flat fields instead find runs of at least 3 consecutive bit-identical
 * samples within 1 dB of the signal's peak, so they survive a gain change and
 * do not fire on a sine: measured, a clipped tone attenuated to 0.25 reports
 * `sampleCount: 0` alongside `flatRunCount: 440` and `flatLevel: 0.25`, while
 * an unclipped full-scale sine reports `sampleCount: 2820` alongside
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
 * console.log(detected.sampleCount, detected.runCount);
 * ```
 */
export function masteringRepairDetectClipping(
  request: MasteringRepairDetectClippingRequest,
): ClipDetection {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectClipping', resolvedSampleRate);
  return addon.masteringRepairDetectClipping(request.samples, resolvedSampleRate, request);
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
): CrackleDetection {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectCrackle', resolvedSampleRate);
  return addon.masteringRepairDetectCrackle(request.samples, resolvedSampleRate, request);
}
