import { refuse, refuseType, withEntry } from './internal/entry.js';
import { withRespelled } from './internal/respell.js';
import { addon } from './native.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, requestObject, sampleOneShot } from './validation.js';

/** Level statistic `normalize` targets: `'peak'` or `'rms'`. */
export type NormalizeMode = 'peak' | 'rms';

/** Request for {@link normalize}. */
export interface NormalizeRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Finite target level at or below 0 dBFS. Defaults to 0 for `mode: 'peak'` and
   * -20 for `mode: 'rms'`.
   */
  targetDb?: number;
  /** Normalization statistic. Defaults to peak normalization. */
  mode?: NormalizeMode;
}

/**
 * Normalize audio to a target peak or RMS level.
 *
 * @throws {TypeError} when `sampleRate` is missing or `mode` is not a string
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, an unknown `mode`
 *   or a `targetDb` that is not finite or sits above 0 dBFS
 */
export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<NormalizeRequest>,
): Float32Array;
export function normalize(...args: unknown[]): Float32Array {
  return withEntry('normalize', () => {
    const request = sampleOneShot<NormalizeRequest>('normalize', args);
    const resolvedMode = resolveNormalizeMode(request.mode);
    assertAudioInput(request.samples, request.sampleRate, request);
    // The core names the signal it measures `audio`; the caller wrote `samples`.
    return withRespelled({ audio: { name: 'samples', scale: 1 } }, () =>
      addon.normalize(
        request.samples,
        request.sampleRate,
        request.targetDb ?? (resolvedMode === 'rms' ? -20.0 : 0.0),
        resolvedMode,
      ),
    );
  });
}

function resolveNormalizeMode(value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw refuseType(`mode must be one of peak, rms`);
  }
  if (value !== 'peak' && value !== 'rms') {
    throw refuse(`mode must be one of peak, rms`);
  }
  return value;
}

/** Canonical request form for {@link normalizeStereo}. */
export interface NormalizeStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
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
  /** Normalized left channel. */
  left: Float32Array;
  /** Normalized right channel. */
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
 * @param request.sampleRate Sample rate shared by both channels, required.
 * @param request.targetDb Finite target level at or below 0 dBFS. Defaults to 0
 *   for `mode: 'peak'` and -20 for `mode: 'rms'`.
 * @param request.mode `'peak'` (default) or `'rms'`.
 * @returns The normalized pair and the gain that was applied.
 * @throws `TypeError` when `sampleRate` is missing.
 * @throws `RangeError` for a pair that
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
  return withEntry('normalizeStereo', () => {
    requestObject('normalizeStereo', request, 'request', true);
    const resolvedMode = resolveNormalizeMode(request.mode);
    assertAudioInput(request.left, request.sampleRate, request, 'left');
    assertAudioInput(request.right, request.sampleRate, request, 'right');
    // Mode-dependent, as in the mono `normalize`: 0 dBFS RMS is not a usable
    // default -- the peaks sit well above the RMS, so effectively all of them clip.
    const resolvedTargetDb = request.targetDb ?? (resolvedMode === 'rms' ? -20.0 : 0.0);
    return addon.normalizeStereo(
      request.left,
      request.right,
      request.sampleRate,
      resolvedTargetDb,
      resolvedMode,
    );
  });
}
