import { addon } from './native.js';
import { assertSampleRate } from './validation.js';

export type NormalizeMode = 'peak' | 'rms';

export interface NormalizeRequest {
  samples: Float32Array;
  sampleRate?: number;
  /** Finite target level at or below 0 dBFS. Default 0. */
  targetDb?: number;
  /** Normalization statistic. Defaults to peak normalization. */
  mode?: NormalizeMode;
}

export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate?: number,
  targetDb?: number,
  mode?: NormalizeMode,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate = 22050,
  targetDb = 0.0,
  mode?: NormalizeMode,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, targetDb, mode } : samples;
  const resolvedMode = resolveNormalizeMode('normalize', request.mode);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('normalize', resolvedSampleRate);
  return addon.normalize(
    request.samples,
    resolvedSampleRate,
    request.targetDb ?? 0.0,
    resolvedMode,
  );
}

function resolveNormalizeMode(fnName: string, value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: mode must be 'peak' or 'rms'`);
  }
  if (value !== 'peak' && value !== 'rms') {
    throw new RangeError(`${fnName}: mode must be 'peak' or 'rms'`);
  }
  return value;
}

/** Canonical request form for {@link normalizeStereo}. */
export interface NormalizeStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  /**
   * Finite target level at or below 0 dBFS. Defaults to 0 for `mode: 'peak'`
   * and -20 for `mode: 'rms'`, matching the library and the other surfaces.
   */
  targetDb?: number;
  /** Normalization statistic. Defaults to peak normalization. */
  mode?: NormalizeMode;
}

/** A normalized channel pair and the one gain that produced it. */
export interface NormalizeStereoResult {
  left: Float32Array;
  right: Float32Array;
  /**
   * The gain both channels were given, in dB. One figure rather than a pair
   * because it is one decision; a silent pair comes back untouched at 0.
   */
  appliedGainDb: number;
}

/**
 * Normalize a stereo pair on a level measured across BOTH channels.
 *
 * The gain is one decision applied to both channels, so the stereo balance is
 * preserved. A per-channel gain would lift the quieter side until the two
 * levels matched, which is a balance change rather than a normalization.
 *
 * `mode` selects what is driven to `targetDb`:
 *
 * - `'peak'` (default) takes the peak of the pair, so the louder channel
 *   reaches `targetDb` and the other keeps its distance below it.
 * - `'rms'` takes the root mean square over the two channels' samples
 *   together — the quadratic mean of the per-channel figures, not their
 *   average — and hard-clips the result to [-1, 1].
 *
 * A silent pair is left untouched and reports `appliedGainDb === 0`.
 *
 * @param request.left Left channel.
 * @param request.right Right channel.
 * @param request.sampleRate Sample rate shared by both channels. Default 22050.
 * @param request.targetDb Finite target level at or below 0 dBFS. Defaults to 0
 *   for `mode: 'peak'` and -20 for `mode: 'rms'`.
 * @param request.mode `'peak'` (default) or `'rms'`.
 * @returns The normalized pair and the gain that was applied.
 * @throws SonareError with `code` `ErrorCode.InvalidParameter` for a pair that
 *   cannot be processed together — either channel empty, or the two channels of
 *   unequal length — and for a `targetDb` that is not finite or sits above
 *   0 dBFS.
 *
 * @example
 * ```ts
 * const { left, right, appliedGainDb } = normalizeStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   targetDb: -1,
 * });
 * ```
 */
export function normalizeStereo(request: NormalizeStereoRequest): NormalizeStereoResult {
  const resolvedMode = resolveNormalizeMode('normalizeStereo', request.mode);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('normalizeStereo', resolvedSampleRate);
  // Mode-dependent, unlike the mono `normalize` on this surface, which defaults
  // to 0 dB in both modes. 0 dBFS RMS is not a usable default -- the peaks sit
  // well above the RMS, so effectively all of them clip -- and the library, the
  // Python surface and the CLI all default RMS to -20. A new entry point takes
  // the shared default rather than inheriting a surface-local one.
  const resolvedTargetDb = request.targetDb ?? (resolvedMode === 'rms' ? -20.0 : 0.0);
  return addon.normalizeStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    resolvedTargetDb,
    resolvedMode,
  );
}
