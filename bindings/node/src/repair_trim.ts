/**
 * Silence trimming, with the range detector it shares its geometry with.
 */

import type { MasteringRepairSamplesRequest } from './_repair_common.js';
import { addon } from './native.js';
import type { TrimRange, TrimSilenceStereoResult } from './types.js';
import { assertSampleRate } from './validation.js';

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}

export interface MasteringRepairTrimSilenceRequest
  extends MasteringRepairSamplesRequest,
    TrimSilenceOptions {}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate?: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate = 22050,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairTrimSilence', resolvedSampleRate);
  return addon.masteringRepairTrimSilence(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairTrimSilenceStereo`. */
export interface MasteringRepairTrimSilenceStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline silence trimmer for a stereo pair (peak threshold or LUFS-gated),
 * cutting both channels to one shared range.
 *
 * Each channel is scanned on its own and the two ranges are unioned, so the
 * pair keeps whatever *either* channel calls signal and both outputs come back
 * the same length. The scan never reads a downmix: `0.5 * (left + right)`
 * halves material carried by one channel alone, which can drop it under the
 * gate, and cancels an antiphase pair to exactly zero, which would read
 * full-level audio in both channels as silence. Trimming is destructive, so the
 * rule errs toward keeping.
 *
 * The returned channels are SHORTER than the input -- that is the point of the
 * entry, and it is what separates it from every other repair stereo processor,
 * which hand back exactly what they were given. When neither channel carries
 * signal the result is two EMPTY arrays and a success, not an error.
 *
 * `report.range` is the union that was applied. `leftRange` and `rightRange`
 * are the per-channel scans it was formed from, so a caller can see which
 * channel decided each edge. A channel carrying nothing reports an empty range
 * -- `(length, length)` rather than `(0, 0)` -- and contributes nothing to the
 * union.
 *
 * Which option is live depends on `mode`: `threshold` is read only by `'peak'`,
 * and `gateLufs` and `windowMs` only by `'lufsGated'`. That gated mode compares
 * an UNWEIGHTED RMS over a window centred on each sample against `gateLufs`, so
 * the figure it gates on is dBFS rather than a BS.1770 loudness, and `windowMs`
 * sizes that window and does nothing else. The window is clipped at the buffer
 * ends, so a sample near either edge is judged on a shorter one.
 *
 * `paddingSamples` widens the kept range in both directions and is clamped to
 * the buffer, so it can never reach past either end; a pass that kept nothing
 * is not padded. A negative count is refused by name rather than folded into 0.
 *
 * @example
 * ```ts
 * const { left, right, report, leftRange, rightRange } = masteringRepairTrimSilenceStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   paddingSamples: 256,
 * });
 * console.log(left.length, report.removedHeadSamples, leftRange.first, rightRange.first);
 * ```
 */
export function masteringRepairTrimSilenceStereo(
  request: MasteringRepairTrimSilenceStereoRequest,
): TrimSilenceStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairTrimSilenceStereo', resolvedSampleRate);
  return addon.masteringRepairTrimSilenceStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
}

/** Request form of `masteringRepairDetectTrimRange`. */
export interface MasteringRepairDetectTrimRangeRequest
  extends MasteringRepairSamplesRequest,
    TrimSilenceOptions {}

/**
 * Measure the range a trim pass would keep, without trimming.
 *
 * The `paddingSamples` asked for is already INSIDE the returned range, so this
 * is the range {@link masteringRepairTrimSilence} would cut to rather than the
 * detected extent of the signal. A buffer with nothing above the threshold
 * reports `(length, length)` -- an empty range at the far end, not `(0, 0)`.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRange({ samples, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRange(
  request: MasteringRepairDetectTrimRangeRequest,
): TrimRange {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectTrimRange', resolvedSampleRate);
  return addon.masteringRepairDetectTrimRange(request.samples, resolvedSampleRate, request);
}

/** Request form of `masteringRepairDetectTrimRangeStereo`. */
export interface MasteringRepairDetectTrimRangeStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Measure the one range a stereo trim pass would cut both channels to.
 *
 * Each channel is scanned on its own and the two ranges are unioned, so the
 * pair keeps whatever *either* channel calls signal. A channel with nothing
 * above the threshold contributes NO EDGE at all rather than an edge at the
 * buffer's end: the union of a silent channel and an active one is the active
 * channel's range exactly, so one silent channel does not widen the result.
 *
 * A downmix is not read -- summing to mono halves material carried by one
 * channel alone and cancels an antiphase pair outright, either of which would
 * read full-level audio as silence.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRangeStereo({ left, right, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRangeStereo(
  request: MasteringRepairDetectTrimRangeStereoRequest,
): TrimRange {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringRepairDetectTrimRangeStereo', resolvedSampleRate);
  return addon.masteringRepairDetectTrimRangeStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request,
  );
}
