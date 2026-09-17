/**
 * Impulsive-defect repair: declick, declip and decrackle, with their detectors.
 */

import { getSonareModule } from './module_state';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  MasteringRepairDeclickStereoResult,
  MasteringRepairDeclipStereoResult,
  MasteringRepairDecrackleStereoResult,
} from './public_types_repair';

function requireModule() {
  return getSonareModule();
}

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

/** Request form of `masteringRepairDetectClicks`. */
export interface MasteringRepairDetectClicksRequest extends DeclickOptions {
  samples: Float32Array;
  sampleRate: number;
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
