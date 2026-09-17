/**
 * Silence trimming, with the range detector it shares its geometry with.
 */

import { getSonareModule } from './module_state';
import type { MasteringRepairTrimSilenceStereoResult, TrimRange } from './public_types_repair';

function requireModule() {
  return getSonareModule();
}

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

export interface MasteringRepairTrimSilenceRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairTrimSilenceStereo`. */
export interface MasteringRepairTrimSilenceStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairTrimSilence(request.samples, request.sampleRate, request);
}

/**
 * Offline silence trimmer for a stereo pair, cutting both channels to one shared range.
 *
 * Each channel is scanned on its own and the two ranges are UNIONED, so the pair keeps
 * whatever either channel calls signal and both outputs come back the same length. The scan
 * never reads a downmix: `0.5 * (left + right)` halves material carried by one channel alone,
 * which can drop it under the gate, and cancels an antiphase pair to exactly zero, which would
 * read full-level audio in both channels as silence. Trimming is destructive, so the rule errs
 * toward keeping.
 *
 * The only repair stereo entry that SHORTENS its input: `result.left.length` is the output
 * length, and the input's says nothing about it. A pair in which neither channel carries signal
 * returns two EMPTY arrays and succeeds — it does not throw and does not return null.
 *
 * `report.range` is the union that was applied to both channels. `leftRange` and `rightRange`
 * are the per-channel scans it was formed from, so a caller can see which channel decided each
 * edge; a channel carrying nothing reports an empty range and contributes nothing to the union.
 * With nothing kept, `report.range` is `(inputLength, inputLength)`, so `removedHeadSamples` is
 * the whole input and `removedTailSamples` is 0 — the two still sum to the input length and
 * only the split between the ends is arbitrary.
 *
 * Which option is live depends on `mode`: `threshold` is read ONLY by `'peak'`, and `gateLufs`
 * and `windowMs` ONLY by `'lufsGated'`. Changing an option the active mode does not read is
 * silently inert rather than an error.
 *
 * That gated mode compares an UNWEIGHTED RMS over a window centred on each sample against
 * `gateLufs`, so the figure it gates on is dBFS rather than a BS.1770 loudness, and `windowMs`
 * sizes that window and does nothing else. The window is clipped at the buffer ends, so a
 * sample near either edge is judged on a shorter one.
 *
 * `paddingSamples` widens the kept range in both directions and is clamped to the buffer, so it
 * can never reach past either end; a pass that kept nothing is not padded. A NEGATIVE count is
 * refused by name rather than absorbed into 0 or into the default — the underlying field is
 * unsigned, and a negative one would arrive as an enormous count instead.
 *
 * @example
 * ```ts
 * const { left, right, report, leftRange, rightRange } = masteringRepairTrimSilenceStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   mode: 'peak',
 *   threshold: 0.01,
 * });
 * console.log(left.length, report.removedHeadSamples, leftRange.first, rightRange.first);
 * ```
 */
export function masteringRepairTrimSilenceStereo(
  request: MasteringRepairTrimSilenceStereoRequest,
): MasteringRepairTrimSilenceStereoResult;
export function masteringRepairTrimSilenceStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: TrimSilenceOptions,
): MasteringRepairTrimSilenceStereoResult;
export function masteringRepairTrimSilenceStereo(
  left: Float32Array | MasteringRepairTrimSilenceStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: TrimSilenceOptions = {},
): MasteringRepairTrimSilenceStereoResult {
  const request: MasteringRepairTrimSilenceStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairTrimSilenceStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Request form of `masteringRepairDetectTrimRange`. */
export interface MasteringRepairDetectTrimRangeRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDetectTrimRangeStereo`. */
export interface MasteringRepairDetectTrimRangeStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Measures the range {@link masteringRepairTrimSilence} would keep, without trimming.
 *
 * The padding `paddingSamples` asks for is already INSIDE the returned range, so this is the
 * range the repair would cut to rather than the detected extent of the signal. A buffer with
 * nothing above the threshold reports `(length, length)`.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRange({ samples, sampleRate: 48000, threshold: 0.01 });
 * const keptSeconds = (range.lastExclusive - range.first) / 48000;
 * ```
 */
export function masteringRepairDetectTrimRange(
  request: MasteringRepairDetectTrimRangeRequest,
): TrimRange;
export function masteringRepairDetectTrimRange(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): TrimRange;
export function masteringRepairDetectTrimRange(
  samples: Float32Array | MasteringRepairDetectTrimRangeRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): TrimRange {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDetectTrimRange(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Measures the one range a stereo trim pass would cut both channels to.
 *
 * Each channel is scanned on its own and the two ranges are UNIONED, so the pair keeps whatever
 * either channel calls signal. A channel with nothing above the threshold contributes NO EDGE
 * rather than an edge at the buffer's end: the union of a silent channel and an active one is
 * the active channel's range exactly, where a naive `min`/`max` would push `lastExclusive` out
 * to the buffer end and keep the whole tail.
 *
 * A downmix is not read: summing to mono halves material carried by one channel alone and
 * cancels an antiphase pair outright, either of which would read full-level audio as silence.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRangeStereo({ left, right, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRangeStereo(
  request: MasteringRepairDetectTrimRangeStereoRequest,
): TrimRange;
export function masteringRepairDetectTrimRangeStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: TrimSilenceOptions,
): TrimRange;
export function masteringRepairDetectTrimRangeStereo(
  left: Float32Array | MasteringRepairDetectTrimRangeStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: TrimSilenceOptions = {},
): TrimRange {
  const request: MasteringRepairDetectTrimRangeStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDetectTrimRangeStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}
