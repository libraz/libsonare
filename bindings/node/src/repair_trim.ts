/**
 * Silence trimming, with the range detector it shares its geometry with.
 */

import { type Respelled, withEntry } from './internal/entry.js';
import { repairNativeOptions } from './internal/repair_common.js';
import { withRespelled } from './internal/respell.js';
import { addon } from './native.js';
import type {
  MasteringRepairSamplesRequest,
  MasteringRepairTrimSilenceResult,
  TrimRange,
  TrimSilenceResult,
} from './types.js';
import { assertAudioInput, requestOnly, resolveSampleBound, sampleOneShot } from './validation.js';

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  /** Peak gate in dBFS, at most 0. Default -60. Read only by `'peak'`. */
  thresholdDb?: number;
  /** Kept margin around the signal, in samples. Not with `paddingSec`. */
  paddingSamples?: number;
  /** Kept margin in seconds, rounded to the nearest sample. Not with `paddingSamples`. */
  paddingSec?: number;
  /** Trim criterion. Default `'peak'`. */
  mode?: TrimSilenceMode;
  /** Gate level in dBFS, from -144 to 0; read only by `'lufsGated'`. Default `-60`. */
  gateLufs?: number;
  /** Gate window in milliseconds, in (0, 10000]; read only by `'lufsGated'`. Default `400`. */
  windowMs?: number;
}

/** Request form of `masteringRepairTrimSilence`. */
export interface MasteringRepairTrimSilenceRequest
  extends MasteringRepairSamplesRequest,
    TrimSilenceOptions {}

/** The addon options, with `paddingSec` folded into `paddingSamples`, which is what the addon reads. */
function nativeTrimOptions(
  request: object,
  sampleRate: number,
): { options: Record<string, unknown>; respelled: Respelled } {
  const native = repairNativeOptions(request);
  const padding = resolveSampleBound(
    native.paddingSamples as number | undefined,
    native.paddingSec as number | undefined,
    sampleRate,
    'paddingSamples',
    'paddingSec',
    'signed',
  );
  delete native.paddingSec;
  if (padding.value !== undefined) {
    native.paddingSamples = padding.value;
  }
  return { options: native, respelled: padding.respelled };
}

/**
 * Offline silence trimmer (peak threshold or LUFS-gated). Returns the kept audio and what the pass
 * did; a trim that kept nothing gives an empty `samples`, not an error.
 */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): TrimSilenceResult;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): TrimSilenceResult;
export function masteringRepairTrimSilence(...args: unknown[]): TrimSilenceResult {
  return withEntry('masteringRepairTrimSilence', () => {
    const r = sampleOneShot<MasteringRepairTrimSilenceRequest>('masteringRepairTrimSilence', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const trim = nativeTrimOptions(r, r.sampleRate);
    return withRespelled(trim.respelled, () =>
      addon.masteringRepairTrimSilence(r.samples, r.sampleRate, trim.options),
    );
  });
}

/** Request form of `masteringRepairTrimSilenceStereo`. */
export interface MasteringRepairTrimSilenceStereoRequest extends TrimSilenceOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
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
 * `reports[0].range` is the union that was applied. `ranges` is `[left, right]`:
 * the the per-channel scans it was formed from, so a caller can see which
 * channel decided each edge. A channel carrying nothing reports an empty range
 * -- `(length, length)` rather than `(0, 0)` -- and contributes nothing to the
 * union.
 *
 * Which option is live depends on `mode`: `thresholdDb` is read only by `'peak'`,
 * and `gateLufs` and `windowMs` only by `'lufsGated'`. That gated mode compares
 * an UNWEIGHTED RMS over a window centred on each sample against `gateLufs`, so
 * the figure it gates on is dBFS rather than a BS.1770 loudness, and `windowMs`
 * sizes that window and does nothing else. The window is clipped at the buffer
 * ends, so a sample near either edge is judged on a shorter one.
 *
 * `paddingSamples` widens the kept range in both directions and is clamped to
 * the buffer, so it can never reach past either end; a pass that kept nothing
 * is not padded. A negative count is refused by name rather than folded into 0.
 * `paddingSec` spells the same margin in seconds, rounded to the nearest sample;
 * give `paddingSamples` or `paddingSec`, not both.
 *
 * @example
 * ```ts
 * const { channels, reports, ranges } = masteringRepairTrimSilenceStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   paddingSamples: 256,
 * });
 * console.log(channels[0].length, reports[0].removedHeadSamples, ranges[0].first, ranges[1].first);
 * ```
 */
export function masteringRepairTrimSilenceStereo(
  request: MasteringRepairTrimSilenceStereoRequest,
): MasteringRepairTrimSilenceResult;
export function masteringRepairTrimSilenceStereo(
  ...args: unknown[]
): MasteringRepairTrimSilenceResult {
  return withEntry('masteringRepairTrimSilenceStereo', () => {
    const r = requestOnly<MasteringRepairTrimSilenceStereoRequest>(
      'masteringRepairTrimSilenceStereo',
      args,
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    const trim = nativeTrimOptions(r, r.sampleRate);
    const native = withRespelled(trim.respelled, () =>
      addon.masteringRepairTrimSilenceStereo(r.left, r.right, r.sampleRate, trim.options),
    );
    return {
      channels: [native.left, native.right],
      reports: [native.report],
      ranges: [native.leftRange, native.rightRange],
    };
  });
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
): TrimRange;
export function masteringRepairDetectTrimRange(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): TrimRange;
export function masteringRepairDetectTrimRange(...args: unknown[]): TrimRange {
  return withEntry('masteringRepairDetectTrimRange', () => {
    const r = sampleOneShot<MasteringRepairDetectTrimRangeRequest>(
      'masteringRepairDetectTrimRange',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    const trim = nativeTrimOptions(r, r.sampleRate);
    return withRespelled(trim.respelled, () =>
      addon.masteringRepairDetectTrimRange(r.samples, r.sampleRate, trim.options),
    );
  });
}

/** Request form of `masteringRepairDetectTrimRangeStereo`. */
export interface MasteringRepairDetectTrimRangeStereoRequest extends TrimSilenceOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz. */
  sampleRate: number;
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
): TrimRange;
export function masteringRepairDetectTrimRangeStereo(...args: unknown[]): TrimRange {
  return withEntry('masteringRepairDetectTrimRangeStereo', () => {
    const r = requestOnly<MasteringRepairDetectTrimRangeStereoRequest>(
      'masteringRepairDetectTrimRangeStereo',
      args,
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    const trim = nativeTrimOptions(r, r.sampleRate);
    return withRespelled(trim.respelled, () =>
      addon.masteringRepairDetectTrimRangeStereo(r.left, r.right, r.sampleRate, trim.options),
    );
  });
}
