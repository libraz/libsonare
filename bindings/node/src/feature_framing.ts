import { resolvePositiveIntegerOption } from './_feature_options.js';
import type { FeatureSamplesRequest } from './feature_spectral.js';
import type { ValuesRequest } from './feature_units.js';
import { addon } from './native.js';

export interface TrimSilenceRequest {
  samples: Float32Array;
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
  return addon.trimSilence(
    request.samples,
    request.topDb ?? 60,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
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
  return addon.splitSilence(
    request.samples,
    request.topDb ?? 60,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
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
