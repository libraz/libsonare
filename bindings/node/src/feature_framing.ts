import { resolvePositiveIntegerOption } from './_feature_options.js';
import type { FeatureSamplesRequest } from './feature_spectral.js';
import type { ValuesRequest } from './feature_units.js';
import { addon } from './native.js';
import type { SilenceCommonReport } from './types_features.js';
import { assertPositiveInteger } from './validation.js';

export interface TrimSilenceRequest {
  samples: Float32Array;
  topDb?: number;
  frameLength?: number;
  hopLength?: number;
}

export interface SplitSilenceCommonRequest {
  signals: Float32Array[];
  topDb?: number;
  frameLength?: number;
  hopLength?: number;
}
export interface FrameSignalRequest {
  samples: Float32Array;
  frameLength: number;
  hopLength: number;
}

/** Input for pre/de-emphasis filters. `zi` is the initial delay value. */
export interface EmphasisRequest {
  samples: Float32Array;
  coef?: number;
  zi?: number;
}

export function trim(
  request: FeatureSamplesRequest & {
    thresholdDb?: number;
    frameLength?: number;
    hopLength?: number;
  },
): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate?: number,
  thresholdDb?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function trim(
  samples:
    | Float32Array
    | (FeatureSamplesRequest & {
        thresholdDb?: number;
        frameLength?: number;
        hopLength?: number;
      }),
  sampleRate = 22050,
  thresholdDb = -60.0,
  frameLength?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, thresholdDb, frameLength, hopLength }
      : samples;
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'trim',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'trim',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.trim(
    request.samples,
    request.sampleRate ?? 22050,
    request.thresholdDb ?? -60.0,
    resolvedFrameLength,
    resolvedHopLength,
  );
}

export function preemphasis(request: EmphasisRequest): Float32Array;
export function preemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function preemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, coef, zi } : samples;
  return request.zi === undefined
    ? addon.preemphasis(request.samples, request.coef ?? 0.97)
    : addon.preemphasis(request.samples, request.coef ?? 0.97, request.zi);
}

export function deemphasis(request: EmphasisRequest): Float32Array;
export function deemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function deemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, coef, zi } : samples;
  return request.zi === undefined
    ? addon.deemphasis(request.samples, request.coef ?? 0.97)
    : addon.deemphasis(request.samples, request.coef ?? 0.97, request.zi);
}

export function trimSilence(request: TrimSilenceRequest): {
  audio: Float32Array;
  startSample: number;
  endSample: number;
};
export function trimSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): { audio: Float32Array; startSample: number; endSample: number };
export function trimSilence(
  samples: Float32Array | TrimSilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): { audio: Float32Array; startSample: number; endSample: number } {
  const request =
    samples instanceof Float32Array ? { samples, topDb, frameLength, hopLength } : samples;
  // The framing rule `trim` itself enforces: both positive, no other domain.
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'trimSilence',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'trimSilence',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.trimSilence(
    request.samples,
    request.topDb ?? 60,
    resolvedFrameLength,
    resolvedHopLength,
  );
}

export function splitSilence(request: TrimSilenceRequest): Int32Array;
export function splitSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): Int32Array;
export function splitSilence(
  samples: Float32Array | TrimSilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  const request =
    samples instanceof Float32Array ? { samples, topDb, frameLength, hopLength } : samples;
  // Both positive, as trimSilence: `split` applies the same framing rule.
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'splitSilence',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'splitSilence',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.splitSilence(
    request.samples,
    request.topDb ?? 60,
    resolvedFrameLength,
    resolvedHopLength,
  );
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
export function splitSilenceCommon(request: SplitSilenceCommonRequest): Int32Array {
  // Both positive, as splitSilence: `split_silence_common` applies the same
  // framing rule to every signal.
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'splitSilenceCommon',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'splitSilenceCommon',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.splitSilenceCommon(
    request.signals,
    request.topDb ?? 60,
    resolvedFrameLength,
    resolvedHopLength,
  );
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
 * const { intervals, report } = splitSilenceCommonWithReport({ signals, topDb });
 * if (intervals.length === 2 && report.silenceCeilingDb < topDb) {
 *   // The takes do have a shared dip; this threshold was too loose to see it.
 *   splitSilenceCommonWithReport({ signals, topDb: report.silenceCeilingDb - 5 });
 * }
 * ```
 */
export function splitSilenceCommonWithReport(request: SplitSilenceCommonRequest): {
  intervals: Int32Array;
  report: SilenceCommonReport;
} {
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'splitSilenceCommonWithReport',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'splitSilenceCommonWithReport',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.splitSilenceCommonWithReport(
    request.signals,
    request.topDb ?? 60,
    resolvedFrameLength,
    resolvedHopLength,
  );
}

export function frameSignal(request: FrameSignalRequest): { nFrames: number; frames: Float32Array };
export function frameSignal(
  samples: Float32Array,
  frameLength?: number,
  hopLength?: number,
): { nFrames: number; frames: Float32Array };
export function frameSignal(
  samples: Float32Array | FrameSignalRequest,
  frameLength = 0,
  hopLength = 0,
): { nFrames: number; frames: Float32Array } {
  const request = samples instanceof Float32Array ? { samples, frameLength, hopLength } : samples;
  // Both positive, as the sibling framing entries. The positional form's 0 is
  // not a default the core has -- it refuses one -- so it is refused by name here.
  assertPositiveInteger('frameSignal', request.frameLength, 'frameLength');
  assertPositiveInteger('frameSignal', request.hopLength, 'hopLength');
  return addon.frameSignal(request.samples, request.frameLength, request.hopLength);
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
export function padCenter(
  request: ValuesRequest & { targetSize: number; padValue?: number },
): Float32Array;
export function padCenter(
  values: Float32Array,
  targetSize?: number,
  padValue?: number,
): Float32Array;
export function padCenter(
  values: Float32Array | (ValuesRequest & { targetSize: number; padValue?: number }),
  targetSize = 0,
  padValue = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, targetSize, padValue } : values;
  return addon.padCenter(request.values, request.targetSize, request.padValue ?? 0);
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
export function fixLength(
  request: ValuesRequest & { targetSize: number; padValue?: number },
): Float32Array;
export function fixLength(
  values: Float32Array,
  targetSize?: number,
  padValue?: number,
): Float32Array;
export function fixLength(
  values: Float32Array | (ValuesRequest & { targetSize: number; padValue?: number }),
  targetSize = 0,
  padValue = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, targetSize, padValue } : values;
  return addon.fixLength(request.values, request.targetSize, request.padValue ?? 0);
}

export function fixFrames(request: {
  frames: Int32Array | number[];
  xMin?: number;
  xMax?: number;
  pad?: boolean;
}): Int32Array;
export function fixFrames(
  frames: Int32Array | number[],
  xMin?: number,
  xMax?: number,
  pad?: boolean,
): Int32Array;
export function fixFrames(
  frames:
    | Int32Array
    | number[]
    | { frames: Int32Array | number[]; xMin?: number; xMax?: number; pad?: boolean },
  xMin = 0,
  xMax = -1,
  pad = true,
): Int32Array {
  const request =
    frames instanceof Int32Array || Array.isArray(frames) ? { frames, xMin, xMax, pad } : frames;
  return addon.fixFrames(
    request.frames,
    request.xMin ?? 0,
    request.xMax ?? -1,
    request.pad ?? true,
  );
}

export function vectorNormalize(
  request: ValuesRequest & { normType?: number; threshold?: number },
): Float32Array;
export function vectorNormalize(
  values: Float32Array,
  normType?: number,
  threshold?: number,
): Float32Array;
export function vectorNormalize(
  values: Float32Array | (ValuesRequest & { normType?: number; threshold?: number }),
  normType = 0,
  threshold = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, normType, threshold } : values;
  return addon.vectorNormalize(request.values, request.normType ?? 0, request.threshold ?? 0);
}
