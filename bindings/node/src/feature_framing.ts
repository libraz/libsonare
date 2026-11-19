import { refuse, refuseType, withEntry } from './internal/entry.js';
import { toMatrix2D, toNativeIndexList } from './internal/feature_matrix.js';
import { resolvePositiveIntegerOption } from './internal/feature_options.js';
import { addon } from './native.js';
import type {
  Matrix2D,
  NormType,
  SplitSilenceCommonWithReportResult,
  TrimResult,
} from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertAudioSamples,
  assertFiniteScalar,
  assertFloat32Array,
  assertIntegerValue,
  assertPositiveInteger,
  requestOnly,
  resolveOptionalNonNegative,
  sampleOneShot,
} from './validation.js';

/** Request for {@link trimSilence}. */
export interface TrimSilenceRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Threshold under the peak, in dB. Default 60. */
  topDb?: number;
  /** Framing window in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Request for {@link splitSilence}. */
export type SplitSilenceRequest = TrimSilenceRequest;

/** Request for {@link splitSilenceCommon}. */
export interface SplitSilenceCommonRequest {
  /** Signals to compare; lengths may differ. */
  signals: Float32Array[];
  /** Threshold below each signal's peak, in dB. Default `60`. */
  topDb?: number;
  /** Framing window in samples. Default `2048`. */
  frameLength?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link splitSilenceCommonWithReport}. */
export type SplitSilenceCommonWithReportRequest = SplitSilenceCommonRequest;

/** An empty or non-array `signals` is the addon's own `TypeError`; only each element's content is checked here. */
function assertSilenceSignals(request: SplitSilenceCommonRequest): void {
  if (Array.isArray(request.signals)) {
    request.signals.forEach((signal, i) => {
      assertAudioSamples(signal, request, `signals[${i}]`);
    });
  }
}

/** Request for {@link frameSignal}. */
export interface FrameSignalRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Required for rule uniformity; framing itself is rate-independent. */
  sampleRate: number;
  /** Frame size in samples. */
  frameLength: number;
  /** Hop between frames in samples. */
  hopLength: number;
}

/** Request for {@link preemphasis}. `zi` is the initial delay value. */
export interface PreemphasisRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Filter coefficient. Default 0.97. */
  coef?: number;
  /** Initial filter state. */
  zi?: number;
}

/** Request for {@link deemphasis}. */
export type DeemphasisRequest = PreemphasisRequest;

/** Request for {@link padCenter}. */
export interface PadCenterRequest {
  /** Input values. */
  values: Float32Array;
  /** Length of the result in elements. */
  targetSize: number;
  /** Literal fill value for every added element. Default 0. */
  padValue?: number;
}

/** Request for {@link fixLength}. */
export type FixLengthRequest = PadCenterRequest;

/** Request for {@link fixFrames}. */
export interface FixFramesRequest {
  /** Frame indices to bound. */
  frames: Int32Array | ArrayLike<number>;
  /** Lower bound. Default 0. */
  xMin?: number;
  /** Upper bound; omit for none. Negative is refused. */
  xMax?: number;
  /** Pad to the bounds instead of clipping. Default true. */
  pad?: boolean;
}

/** Request for {@link vectorNormalize}. */
export interface VectorNormalizeRequest {
  /** Values to normalize. */
  values: Float32Array;
  /** Norm applied. Default `'max'`. */
  norm?: NormType;
  /** Magnitude below which a norm is treated as zero and the value left unscaled. Default `0`. */
  threshold?: number;
}

const NORM_ORDINALS: Readonly<Record<NormType, number>> = { max: 0, l1: 1, l2: 2, power: 3 };

/** Request for {@link trim}. */
export interface TrimRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Silence threshold in dBFS. Default -60. */
  thresholdDb?: number;
  /** Framing window in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/**
 * Trim silence from the beginning and end of `samples`.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, a non-finite `thresholdDb` or a non-positive frame or hop
 */
export function trim(request: TrimRequest): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<TrimRequest>,
): Float32Array;
export function trim(...args: unknown[]): Float32Array {
  return withEntry('trim', () => {
    const request = sampleOneShot<TrimRequest>('trim', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const thresholdDb = request.thresholdDb ?? -60.0;
    assertFiniteScalar(thresholdDb, 'thresholdDb');
    const frameLength = resolvePositiveIntegerOption('frameLength', request.frameLength, 2048);
    const hopLength = resolvePositiveIntegerOption('hopLength', request.hopLength, 512);
    return addon.trim(request.samples, request.sampleRate, thresholdDb, frameLength, hopLength);
  });
}

/** First-order pre-emphasis filter. */
export function preemphasis(request: PreemphasisRequest): Float32Array;
export function preemphasis(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PreemphasisRequest>,
): Float32Array;
export function preemphasis(...args: unknown[]): Float32Array {
  return withEntry('preemphasis', () => {
    const request = sampleOneShot<PreemphasisRequest>('preemphasis', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return request.zi === undefined
      ? addon.preemphasis(request.samples, request.coef ?? 0.97)
      : addon.preemphasis(request.samples, request.coef ?? 0.97, request.zi);
  });
}

/** Inverse of {@link preemphasis}. */
export function deemphasis(request: DeemphasisRequest): Float32Array;
export function deemphasis(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DeemphasisRequest>,
): Float32Array;
export function deemphasis(...args: unknown[]): Float32Array {
  return withEntry('deemphasis', () => {
    const request = sampleOneShot<DeemphasisRequest>('deemphasis', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return request.zi === undefined
      ? addon.deemphasis(request.samples, request.coef ?? 0.97)
      : addon.deemphasis(request.samples, request.coef ?? 0.97, request.zi);
  });
}

// Framing rule every silence entry shares: both positive, no other domain.
function resolveSilenceFraming(
  frameLength: number | undefined,
  hopLength: number | undefined,
): { frameLength: number; hopLength: number } {
  return {
    frameLength: resolvePositiveIntegerOption('frameLength', frameLength, 2048),
    hopLength: resolvePositiveIntegerOption('hopLength', hopLength, 512),
  };
}

/** Trims leading and trailing silence. */
export function trimSilence(request: TrimSilenceRequest): TrimResult;
export function trimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<TrimSilenceRequest>,
): TrimResult;
export function trimSilence(...args: unknown[]): TrimResult {
  return withEntry('trimSilence', () => {
    const request = sampleOneShot<TrimSilenceRequest>('trimSilence', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const framing = resolveSilenceFraming(request.frameLength, request.hopLength);
    return addon.trimSilence(
      request.samples,
      request.topDb ?? 60,
      framing.frameLength,
      framing.hopLength,
    );
  });
}

/** Non-silent intervals as flat `[start, end, ...]` sample pairs. */
export function splitSilence(request: SplitSilenceRequest): Int32Array;
export function splitSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SplitSilenceRequest>,
): Int32Array;
export function splitSilence(...args: unknown[]): Int32Array {
  return withEntry('splitSilence', () => {
    const request = sampleOneShot<SplitSilenceRequest>('splitSilence', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const framing = resolveSilenceFraming(request.frameLength, request.hopLength);
    return addon.splitSilence(
      request.samples,
      request.topDb ?? 60,
      framing.frameLength,
      framing.hopLength,
    );
  });
}

/**
 * Lists the intervals where any of `request.signals` is sounding, merged
 * where they touch, so every gap between the returned intervals is silent in
 * every signal at once. What several takes of one phrase share is the
 * silence, not the sound: the result is the union of each signal's own
 * {@link splitSilence} intervals, never their intersection, so a cut placed
 * in a gap never lands mid-phrase in any one of them.
 *
 * A signal shorter than the longest contributes nothing past its own end,
 * the same as being silent there, so takes of unequal length need no padding.
 * Passing a single signal returns exactly what {@link splitSilence} would.
 *
 * @param request - The signals to compare and the shared threshold/framing,
 *   matching {@link splitSilence}'s field names and defaults.
 * @throws {TypeError} `request.signals` is empty, or one of its elements is
 *   not a `Float32Array`.
 */
export function splitSilenceCommon(request: SplitSilenceCommonRequest): Int32Array;
export function splitSilenceCommon(...args: unknown[]): Int32Array {
  return withEntry('splitSilenceCommon', () => {
    const request = requestOnly<SplitSilenceCommonRequest>('splitSilenceCommon', args);
    assertSilenceSignals(request);
    // As splitSilence: `split_silence_common` applies the same framing rule to every signal.
    const framing = resolveSilenceFraming(request.frameLength, request.hopLength);
    return addon.splitSilenceCommon(
      request.signals,
      request.topDb ?? 60,
      framing.frameLength,
      framing.hopLength,
    );
  });
}

/**
 * {@link splitSilenceCommon} plus the figures that say why those are the gaps.
 *
 * Identical intervals, identical arguments, identical refusals; the only
 * difference is the `report`. The plain entry point stays because a caller
 * cutting takes has no use for the diagnosis — reach for this one when a single
 * interval covering everything needs explaining, and read
 * {@link SilenceCommonReport.silenceCeilingDb} against the `topDb` passed in.
 *
 * @param request - As {@link splitSilenceCommon}.
 * @throws {TypeError} `request.signals` is empty, or one of its elements is
 *   not a `Float32Array`.
 * @example
 * ```ts
 * const topDb = 60;
 * const signals = channels; // takes of one part, sample-aligned
 * const { intervals, report } = splitSilenceCommonWithReport({ signals, topDb });
 * if (intervals.length === 2 && report.silenceCeilingDb < topDb) {
 *   // The takes do have a shared dip; this threshold was too loose to see it.
 *   splitSilenceCommonWithReport({ signals, topDb: report.silenceCeilingDb - 5 });
 * }
 * ```
 */
export function splitSilenceCommonWithReport(
  request: SplitSilenceCommonWithReportRequest,
): SplitSilenceCommonWithReportResult;
export function splitSilenceCommonWithReport(
  ...args: unknown[]
): SplitSilenceCommonWithReportResult {
  return withEntry('splitSilenceCommonWithReport', () => {
    const request = requestOnly<SplitSilenceCommonWithReportRequest>(
      'splitSilenceCommonWithReport',
      args,
    );
    assertSilenceSignals(request);
    const framing = resolveSilenceFraming(request.frameLength, request.hopLength);
    return addon.splitSilenceCommonWithReport(
      request.signals,
      request.topDb ?? 60,
      framing.frameLength,
      framing.hopLength,
    );
  });
}

/** Slices a signal into overlapping frames. Returns rows = `frameLength` samples, cols = frames. */
export function frameSignal(request: FrameSignalRequest): Matrix2D;
export function frameSignal(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<FrameSignalRequest>,
): Matrix2D;
export function frameSignal(...args: unknown[]): Matrix2D {
  return withEntry('frameSignal', () => {
    const request = sampleOneShot<FrameSignalRequest>('frameSignal', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    // Both positive, as the sibling framing entries; neither has a default.
    assertPositiveInteger(request.frameLength, 'frameLength');
    assertPositiveInteger(request.hopLength, 'hopLength');
    const r = addon.frameSignal(request.samples, request.frameLength, request.hopLength);
    return toMatrix2D(r.frames, request.frameLength, r.nFrames);
  });
}

/**
 * Centres `values` inside `targetSize`, filling both sides with `padValue`.
 *
 * @param request - The values, the length to reach, and the fill value.
 * @param request.padValue - The literal value written into every added element,
 *   default `0`. It is a quantity with no sentinel — there is no spelling of
 *   "unspecified" — so a non-finite value is out of domain rather than a request
 *   for a default.
 * @throws {RangeError} `padValue` is not a finite number the 32-bit float range
 *   can hold.
 * @example
 * ```ts
 * padCenter({ values: Float32Array.from([1, 2]), targetSize: 4, padValue: 7 });
 * // Float32Array [7, 1, 2, 7]
 * ```
 */
export function padCenter(request: PadCenterRequest): Float32Array;
export function padCenter(...args: unknown[]): Float32Array {
  return withEntry('padCenter', () => {
    const r = requestOnly<PadCenterRequest>('padCenter', args);
    assertFloat32Array(r.values, 'values');
    assertIntegerValue(r.targetSize, 'targetSize');
    return addon.padCenter(r.values, r.targetSize, r.padValue ?? 0);
  });
}

/**
 * Pads `values` on the right with `padValue`, or truncates it, to `targetSize`.
 *
 * @param request - The values, the length to reach, and the fill value.
 * @param request.padValue - As in {@link padCenter}: the literal value written
 *   into every added element, default `0`, with no non-finite spelling. Unused
 *   when `values` is already at least `targetSize` long.
 * @throws {RangeError} `padValue` is not a finite number the 32-bit float range
 *   can hold.
 * @example
 * ```ts
 * fixLength({ values: Float32Array.from([1, 2]), targetSize: 4, padValue: 7 });
 * // Float32Array [1, 2, 7, 7]
 * ```
 */
export function fixLength(request: FixLengthRequest): Float32Array;
export function fixLength(...args: unknown[]): Float32Array {
  return withEntry('fixLength', () => {
    const r = requestOnly<FixLengthRequest>('fixLength', args);
    assertFloat32Array(r.values, 'values');
    assertIntegerValue(r.targetSize, 'targetSize');
    return addon.fixLength(r.values, r.targetSize, r.padValue ?? 0);
  });
}

/** Clips or pads a list of frame indices to `[xMin, xMax]`. */
export function fixFrames(request: FixFramesRequest): Int32Array;
export function fixFrames(...args: unknown[]): Int32Array {
  return withEntry('fixFrames', () => {
    const r = requestOnly<FixFramesRequest>('fixFrames', args);
    return addon.fixFrames(
      toNativeIndexList(r.frames, 'frames'),
      r.xMin ?? 0,
      resolveOptionalNonNegative(r.xMax, 'xMax'),
      r.pad ?? true,
    );
  });
}

/**
 * Normalises `values` by the chosen norm.
 *
 * @throws `TypeError` for a `norm` that is not a string, `RangeError` for an unknown token.
 */
export function vectorNormalize(request: VectorNormalizeRequest): Float32Array;
export function vectorNormalize(...args: unknown[]): Float32Array {
  return withEntry('vectorNormalize', () => {
    const r = requestOnly<VectorNormalizeRequest>('vectorNormalize', args);
    assertFloat32Array(r.values, 'values');
    const norm = r.norm ?? 'max';
    if (typeof norm !== 'string') {
      throw refuseType('norm must be a string');
    }
    if (!Object.hasOwn(NORM_ORDINALS, norm)) {
      throw refuse(`unknown norm '${norm}'`);
    }
    return addon.vectorNormalize(r.values, NORM_ORDINALS[norm], r.threshold ?? 0);
  });
}
