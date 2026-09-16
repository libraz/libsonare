import { ErrorCode, SonareError } from './errors.js';
import type { ProjectMidiEvent } from './types.js';

/**
 * Per-call validation options accepted by guarded wrappers. Empty-buffer
 * checks are always performed; pass `{ validate: false }` to opt out of the
 * O(n) NaN/Inf scan on hot paths where the caller already controls the data.
 */
export interface ValidateOptions {
  validate?: boolean;
}

export function assertNonEmptySamples(
  fnName: string,
  samples: ArrayLike<number>,
  argName = 'samples',
): void {
  if (samples.length === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be empty`);
  }
}

export function assertFiniteSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  if (!validate) {
    return;
  }
  for (let i = 0; i < samples.length; i++) {
    const v = samples[i] as number;
    if (!Number.isFinite(v)) {
      throw new RangeError(`${fnName}: ${argName} contains NaN or Inf at index ${i}`);
    }
  }
}

export function assertSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  assertNonEmptySamples(fnName, samples, argName);
  assertFiniteSamples(fnName, samples, validate, argName);
}

/**
 * `assertSamples` restricted to the span a windowed entry point actually reads.
 *
 * The emptiness check still covers the whole buffer, and the reported index is
 * the absolute one, so the message keeps naming the sample the caller passed.
 * What narrows is the scan: a windowed call refuses a non-finite sample inside
 * its frame and is indifferent to one outside it, which is the contract the C
 * ABI states and the cost model it promises -- per call the scan is bounded by
 * the frame, not by the length of the buffer being polled.
 */
export function assertSamplesInWindow(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  windowStart: number,
  windowLength: number,
  argName = 'samples',
): void {
  assertNonEmptySamples(fnName, samples, argName);
  if (!validate) {
    return;
  }
  // Floored and ceiled before use: a fractional offset is refused by the layer
  // that owns it, but an index of 100.5 reads `undefined` out of the buffer and
  // would be reported here as a non-finite sample that is not there.
  const start = Math.min(Math.max(Math.floor(windowStart), 0), samples.length);
  const stop = Math.min(start + Math.max(Math.ceil(windowLength), 0), samples.length);
  for (let i = start; i < stop; i++) {
    const v = samples[i] as number;
    if (!Number.isFinite(v)) {
      throw new RangeError(`${fnName}: ${argName} contains NaN or Inf at index ${i}`);
    }
  }
}

/**
 * Check an interleaved buffer against the channel count that divides it.
 *
 * The channel count is narrowed into a C `int` by the addon, which truncates
 * rather than refuses, so a fractional count has to be refused here or not at
 * all: `2.5` divides a 100-sample buffer exactly, reaches the addon as `2`, and
 * comes back as a successful two-channel answer to a question nobody asked.
 * Mirrors the WASM validator of the same name.
 */
export function assertInterleavedSamples(
  fnName: string,
  samples: ArrayLike<number>,
  channels: number,
  validate: boolean,
): void {
  assertSamples(fnName, samples, validate);
  assertPositiveInteger(fnName, channels, 'channels');
  if (samples.length % channels !== 0) {
    throw new RangeError(`${fnName}: samples length must be a multiple of channels`);
  }
}

export function assertFiniteScalar(fnName: string, value: number, argName: string): void {
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a finite number`);
  }
}

/** {@link assertFiniteScalar} plus a floor of zero, for a quantity that is a magnitude. */
export function assertNonNegativeScalar(fnName: string, value: number, argName: string): void {
  assertFiniteScalar(fnName, value, argName);
  if (value < 0) {
    throw new RangeError(`${fnName}: ${argName} must be non-negative`);
  }
}

/**
 * Offline-analysis sample-rate bounds, mirroring the C++ core limits
 * (`sonare::kMinAudioSampleRate` / `kMaxAudioSampleRate` in `core/audio.h`) and
 * the WASM/Python surfaces. Used where a wrong `sampleRate` would corrupt the
 * result rather than merely pick a default, so it must be supplied explicitly.
 */
export const MIN_AUDIO_SAMPLE_RATE = 8000;
export const MAX_AUDIO_SAMPLE_RATE = 384000;

/**
 * The integrality half of {@link assertSampleRate}, for an entry point whose
 * core carries no range at all.
 *
 * A fraction is still refused, because the addon narrowing truncates it onto a
 * rate the core accepts: `44100.7` renders at `44100` with nothing anywhere
 * reporting the substitution. What is deliberately not refused is a rate
 * outside `[8000, 384000]`, which the signal generators do accept.
 */
export function assertIntegralSampleRate(
  fnName: string,
  sampleRate: number,
  argName = 'sampleRate',
): void {
  if (!Number.isInteger(sampleRate)) {
    throw new RangeError(`${fnName}: ${argName} must be an integer`);
  }
}

export function assertSampleRate(fnName: string, sampleRate: number, argName = 'sampleRate'): void {
  // Two refusals, not one: 22050.7 sits inside the range, so reporting it as
  // out of range names an argument that is not the one at fault. `argName` is
  // the same point for a rate the caller spelled something else, such as a
  // resampler's source and target.
  assertIntegralSampleRate(fnName, sampleRate, argName);
  if (sampleRate < MIN_AUDIO_SAMPLE_RATE || sampleRate > MAX_AUDIO_SAMPLE_RATE) {
    throw new RangeError(
      `${fnName}: ${argName} out of supported range [${MIN_AUDIO_SAMPLE_RATE}, ${MAX_AUDIO_SAMPLE_RATE}]`,
    );
  }
}

export function assertU7(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 127]`);
  }
  return value;
}

export function assertNibble(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 15]`);
  }
  return value;
}

/**
 * Reject anything `Number.isInteger` refuses, wrong type included.
 *
 * `Number.isInteger` already returns `false` for a value that is not a
 * `number` at all, so a separate `typeof` guard ahead of it would be dead
 * code: the two collapse to one class (`TypeError`) and one message here.
 */
export function assertIntegerType(
  fnName: string,
  value: unknown,
  argName: string,
): asserts value is number {
  if (!Number.isInteger(value)) {
    throw new TypeError(`${fnName}: ${argName} must be an integer`);
  }
}

/**
 * Split "not an integer" the way some option resolvers report it: `TypeError`
 * when the value is not a `number` at all, `RangeError` when it is a number
 * but not integral -- the right-type, wrong-domain half of the same message.
 * Unlike {@link assertIntegerType}, the two halves are distinguishable, so
 * both checks are needed.
 */
export function assertIntegerValue(
  fnName: string,
  value: unknown,
  argName: string,
): asserts value is number {
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be an integer`);
  }
  if (!Number.isInteger(value)) {
    throw new RangeError(`${fnName}: ${argName} must be an integer`);
  }
}

/** General integer-in-`[min, max]` check, for a bound {@link assertU7}/{@link assertNibble} don't cover. */
export function assertBoundedInteger(
  fnName: string,
  value: number,
  argName: string,
  min: number,
  max: number,
): void {
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [${min}, ${max}]`);
  }
}

/** Integer strictly greater than zero, up to the native `int` ceiling. */
export function assertPositiveInteger(fnName: string, value: number, argName: string): void {
  // Two refusals, not one: 512.7 is positive, so reporting it as non-positive
  // names a property it has. The ceiling travels with the sign rather than with
  // integrality, because both describe a value the native `int` can carry.
  if (!Number.isInteger(value)) {
    throw new RangeError(`${fnName}: ${argName} must be an integer`);
  }
  if (value <= 0 || value > C_INT_MAX) {
    throw new RangeError(`${fnName}: ${argName} must be a positive integer`);
  }
}

/** Even integer in `[min, max]`, for an FFT-style size where only parity matters. */
export function assertEvenIntegerAtLeast(
  fnName: string,
  value: number,
  argName: string,
  min: number,
  max: number,
): void {
  // Range and parity are separate refusals: 2 ** 31 is already even, so telling
  // its caller the value must be even names a property it has.
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [${min}, ${max}]`);
  }
  if (value % 2 !== 0) {
    throw new RangeError(`${fnName}: ${argName} must be an even integer >= ${min}`);
  }
}

/** Reject anything but one specific accepted integer, for a field that presently has a single legal value. */
export function assertExactInteger(
  fnName: string,
  value: number,
  argName: string,
  expected: number,
  note: string,
): void {
  if (!Number.isInteger(value) || value !== expected) {
    throw new TypeError(`${fnName}: ${argName} must be the integer ${expected} (${note})`);
  }
}

export function midi1Event(
  fnName: string,
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  assertFiniteScalar(fnName, ppq, 'ppq');
  if (ppq < 0) {
    throw new RangeError(`${fnName}: ppq must be non-negative`);
  }
  const g = assertNibble(fnName, group, 'group');
  const ch = assertNibble(fnName, channel, 'channel');
  const d1 = assertU7(fnName, data1, 'data1');
  const d2 = assertU7(fnName, data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the WASM suite) so it cannot silently drift.
  const word = ((0x2 << 28) | (g << 24) | (status << 20) | (ch << 16) | (d1 << 8) | d2) >>> 0;
  return { ppq, data0: word, data1: 0 };
}

/** Bounds of the native `int` every addon argument below is narrowed into. */
export const C_INT_MIN = -2147483648;
export const C_INT_MAX = 2147483647;

/**
 * Reject an argument the addon's `Int32Value()` narrowing would wrap.
 *
 * `Int32Value()` is ECMAScript ToInt32, so it wraps rather than saturates: a
 * kernel of `2 ** 32` arrives as 0 and `2 ** 32 + 1` as 1, both of which are
 * values the native guards accept. The call then succeeds having separated on a
 * setting the caller never asked for, which no refusal-shaped check downstream
 * can see. Anything narrowed into a C `int` whose wrapped value would still be
 * in domain has to be checked here instead.
 *
 * Unlike the `RangeError` its neighbours raise, this reports the branded
 * `SonareError` carrying `InvalidParameter`. The class follows what the
 * rejection stands in for, not JS idiom: a `RangeError` is this surface
 * refusing an argument on its own authority, while this one pre-empts a native
 * refusal the caller would have received under that code had the narrowing not
 * wrapped the value into the accepted domain first.
 *
 * That the WASM and Python surfaces answer the same input with the same code is
 * asserted by `tests/narrowing-code-parity.test.ts`, which drives one value
 * through all three, rather than left here as a claim — editing one surface's
 * validator says nothing about the other two on its own.
 */
export function assertInt32(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < C_INT_MIN || value > C_INT_MAX) {
    throw new SonareError(
      ErrorCode.InvalidParameter,
      'InvalidParameter',
      `${fnName}: ${argName} must be an integer within the signed 32-bit range`,
    );
  }
}

/**
 * Check both HPSS kernels before the addon narrows them.
 *
 * Parity, positivity and the ceiling stay the core's to enforce, and it names
 * the median filter that rejected the value. What cannot be deferred is the
 * narrowing itself: a wrapped kernel arrives as a legal one and separates on it.
 */
export function assertHpssKernels(
  fnName: string,
  kernelHarmonic: number,
  kernelPercussive: number,
): void {
  assertInt32(fnName, kernelHarmonic, 'kernelHarmonic');
  assertInt32(fnName, kernelPercussive, 'kernelPercussive');
}

export function assertU32(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 4294967295]`);
  }
}

/**
 * Reject an argument that is not a count: a non-negative integer a JS number
 * still denotes exactly.
 *
 * Neither neighbour says this. {@link assertU32} caps at `0xffffffff`, which is
 * a different domain — the width of a C `uint32_t`, not the range over which a
 * JS number is an exact integer — and {@link assertInt64} ignores sign, so a
 * negative count passes it and arrives at a `size_t` parameter as an enormous
 * positive one. A field that is a length, a capacity or an index needs both
 * halves, which is what this is for; the field's own upper bound stays the
 * callee's to enforce.
 */
export function assertNonNegativeSafeInteger(fnName: string, value: number, argName: string): void {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(
      `${fnName}: ${argName} must be a non-negative integer no greater than Number.MAX_SAFE_INTEGER`,
    );
  }
}

/**
 * {@link assertInt32}'s 64-bit sibling, for a field the addon reads as `int64`.
 *
 * The bound is the safe-integer range rather than the C type's, because a JS
 * number past it no longer denotes one specific `int64` and the value the callee
 * receives is not the one the caller wrote. Everything inside it is the callee's
 * to accept or refuse.
 */
export function assertInt64(fnName: string, value: number, argName: string): void {
  if (!Number.isSafeInteger(value)) {
    throw new SonareError(
      ErrorCode.InvalidParameter,
      'InvalidParameter',
      `${fnName}: ${argName} must be an integer within the safe-integer range`,
    );
  }
}

export function assertProjectMidiEvents(
  fnName: string,
  events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
): void {
  if (!Array.isArray(events)) {
    throw new TypeError(`${fnName}: events must be an array`);
  }
  events.forEach((event, index) => {
    const prefix = `events[${index}]`;
    if (Array.isArray(event)) {
      if (event.length < 3) {
        throw new TypeError(`${fnName}: ${prefix} must contain [ppq, data0, data1]`);
      }
      assertFiniteScalar(fnName, event[0], `${prefix}.ppq`);
      if (event[0] < 0) {
        throw new RangeError(`${fnName}: ${prefix}.ppq must be non-negative`);
      }
      assertU32(fnName, event[1], `${prefix}.data0`);
      assertU32(fnName, event[2], `${prefix}.data1`);
      return;
    }
    if (event === null || typeof event !== 'object') {
      throw new TypeError(`${fnName}: ${prefix} must be a MIDI event object or tuple`);
    }
    assertFiniteScalar(fnName, event.ppq, `${prefix}.ppq`);
    if (event.ppq < 0) {
      throw new RangeError(`${fnName}: ${prefix}.ppq must be non-negative`);
    }
    assertU32(fnName, event.data0, `${prefix}.data0`);
    if (event.data1 !== undefined) {
      assertU32(fnName, event.data1, `${prefix}.data1`);
    }
  });
}
