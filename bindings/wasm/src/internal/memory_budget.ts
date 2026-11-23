/**
 * The heap a one-shot call certainly needs, checked before the call.
 *
 * Each entry counts, from its arguments alone, what it cannot avoid holding at
 * once: the heap copy of its input and the output it will return. That count is
 * a lower bound, so a call it refuses would have failed anyway, and a call it
 * admits may still run out of memory; that failure stays the coded
 * `OutOfMemory`, whose text then names the input size the call was given.
 *
 * Stores a handle keeps across calls (project clips and sources, sample-bank
 * samples, engine clips, takes) are not checked here, since what they hold
 * accumulates; past the heap they fail with the same coded `OutOfMemory`.
 */

import { ErrorCode, isSonareError } from '../errors.js';
import { getSonareModule } from '../module_state.js';
import { codedError, refuse } from './entry.js';

/** What one call certainly holds, as a function of the argument that scales it. */
export interface HeapNeed {
  /** The argument whose size scales the need, as the caller spelled it. */
  readonly subject: string;
  /** Its size: a sample count for an audio input, the value itself for an option. */
  readonly size: number;
  /** Set for an audio input, whose size is stated in samples and seconds. */
  readonly sampleRate?: number;
  /** True when `size` counts samples (with or without a sample rate). */
  readonly samples?: boolean;
  /** Bytes the call certainly holds at once for a subject of size `n`. */
  readonly bytes: (n: number) => number;
  /**
   * True when `bytes` never decreases as `n` grows, so the largest accepted
   * size can be stated exactly; leave unset otherwise.
   */
  readonly monotone?: boolean;
}

const MIB = 1024 * 1024;

const ceilMib = (bytes: number): string => (Math.ceil((bytes / MIB) * 10) / 10).toFixed(1);
const floorMib = (bytes: number): string => (Math.floor((bytes / MIB) * 10) / 10).toFixed(1);

function sizeText(need: HeapNeed, n: number): string {
  if (!need.samples) {
    return `${n}`;
  }
  if (need.sampleRate === undefined) {
    return `${n} samples`;
  }
  return `${n} samples (${(n / need.sampleRate).toFixed(2)} s at ${need.sampleRate} Hz)`;
}

function longestText(need: HeapNeed, m: number): string {
  if (!need.samples) {
    return `${m}`;
  }
  if (need.sampleRate === undefined) {
    return `${m} samples`;
  }
  return `${m} samples (${(Math.floor((m / need.sampleRate) * 100) / 100).toFixed(2)} s)`;
}

/** The largest integer size below `n` whose need fits `headroom`; -1 when none does. */
function largestFitting(need: HeapNeed, n: number, headroom: number): number {
  let lo = -1;
  let hi = Math.floor(n);
  while (hi - lo > 1) {
    const mid = lo + Math.floor((hi - lo) / 2);
    if (need.bytes(mid) <= headroom) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/** Refuses a call whose certain need exceeds what the heap can still hand out. */
function requireHeadroom(need: HeapNeed): void {
  const bytes = need.bytes(need.size);
  const headroom = getSonareModule().sonareHeapHeadroom(bytes);
  if (bytes <= headroom) {
    return;
  }
  let text = `${need.subject} is ${sizeText(need, need.size)} and needs ${ceilMib(bytes)} MiB, more than the ${floorMib(headroom)} MiB available`;
  if (need.monotone) {
    const m = largestFitting(need, need.size, headroom);
    if (m >= 0) {
      text += `; the ${need.samples ? 'longest' : 'largest'} accepted is ${longestText(need, m)}`;
    }
  }
  throw refuse(text);
}

function nameOutOfMemory(error: unknown, need: HeapNeed): unknown {
  if (isSonareError(error) && error.code === ErrorCode.OutOfMemory) {
    return codedError(
      ErrorCode.OutOfMemory,
      `Out of memory; ${need.subject} is ${sizeText(need, need.size)}`,
      { cause: error },
    );
  }
  return error;
}

/**
 * Runs `call` once the heap can hold what it certainly needs, and states the
 * input size on an `OutOfMemory` it raises past that bound.
 */
export function withHeapNeed<T>(need: HeapNeed, call: () => T): T {
  requireHeadroom(need);
  let result: T;
  try {
    result = call();
  } catch (error) {
    throw nameOutOfMemory(error, need);
  }
  if (result instanceof Promise) {
    return result.catch((error: unknown) => {
      throw nameOutOfMemory(error, need);
    }) as T;
  }
  return result;
}

/** The need of a call scaled by a sample-count input, whose `bytes` never decreases with its length. */
export function sampleNeed(
  subject: string,
  length: number,
  sampleRate: number | undefined,
  bytes: (n: number) => number,
): HeapNeed {
  return { subject, size: length, sampleRate, samples: true, bytes, monotone: true };
}

/** Runs `call` under `need` when `applies`; an out-of-domain argument is the native reader's refusal. */
export function withHeapNeedWhen<T>(applies: boolean, need: HeapNeed, call: () => T): T {
  return applies ? withHeapNeed(need, call) : call();
}

/** Runs a mono call once the heap holds `buffers` float buffers of the input's length. */
export function withMonoHeap<T>(
  samples: ArrayLike<number>,
  sampleRate: number | undefined,
  buffers: number,
  call: () => T,
  subject = 'samples',
): T {
  return withHeapNeed(
    sampleNeed(subject, samples.length, sampleRate, (n) => 4 * buffers * n),
    call,
  );
}

/**
 * Runs a stereo call once the heap holds `buffers` float buffers of each channel's length.
 * Channels of different lengths are the native reader's refusal, not a memory one.
 */
export function withPairHeap<T>(
  left: ArrayLike<number>,
  right: ArrayLike<number>,
  sampleRate: number | undefined,
  buffers: number,
  call: () => T,
  subject = 'left',
): T {
  return withHeapNeedWhen(
    left.length === right.length,
    sampleNeed(subject, left.length, sampleRate, (n) => 8 * buffers * n),
    call,
  );
}

/**
 * Runs a channel-set call once the heap holds `buffers` float buffers of every channel's length.
 * Channels of different lengths are the native reader's refusal, not a memory one.
 */
export function withChannelsHeap<T>(
  channels: readonly ArrayLike<number>[],
  sampleRate: number | undefined,
  buffers: number,
  call: () => T,
  subject = 'channels',
): T {
  const length = channels.length === 0 ? 0 : channels[0].length;
  return withHeapNeedWhen(
    channels.every((channel) => channel.length === length),
    sampleNeed(subject, length, sampleRate, (n) => 4 * buffers * channels.length * n),
    call,
  );
}

/** Runs a call over a plain value array once the heap holds `buffers` float buffers of its length. */
export function withValuesHeap<T>(
  values: ArrayLike<number>,
  buffers: number,
  call: () => T,
  subject = 'values',
): T {
  return withHeapNeed(
    { subject, size: values.length, bytes: (n) => 4 * buffers * n, monotone: true },
    call,
  );
}

/** `kMaxOfflineAudioSamples`: the longest signal an offline native call produces or accepts. */
export const MAX_OFFLINE_AUDIO_SAMPLES = 500_000_000;

/** `kMaxStftNFft` in src/core/spectrum.h: the largest STFT size the native readers accept. */
export const MAX_STFT_N_FFT = 524288;

/** Bins of a centred STFT whose even size and hop the native reader accepts; undefined otherwise. */
export function stftBins(nFft: unknown, hopLength: unknown): number | undefined {
  if (typeof nFft !== 'number' || !Number.isInteger(nFft) || nFft < 2 || nFft > MAX_STFT_N_FFT) {
    return undefined;
  }
  if (nFft % 2 !== 0) {
    return undefined;
  }
  if (
    typeof hopLength !== 'number' ||
    !Number.isInteger(hopLength) ||
    hopLength < 1 ||
    hopLength > 0x7fffffff
  ) {
    return undefined;
  }
  return nFft / 2 + 1;
}

/** Frames a centred STFT of `n` samples holds at an even size. */
export const stftFrames = (n: number, hopLength: number): number => 1 + Math.floor(n / hopLength);
