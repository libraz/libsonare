import { OMIT_FOR_POSITIVE_DEFAULT } from './internal/default_refusal.js';
import {
  assertDeclaredKeys,
  type Respelled,
  refuse,
  refuseType,
  refuseUnknownKey,
} from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { requireOneOf } from './internal/one_of.js';
import { isAbsent, isPlainRecord } from './internal/value_guards.js';
import { NESTED_REQUEST_KEYS, REQUEST_KEYS, type RequestFn } from './request_keys.js';
import type { ProjectMidiEvent } from './types.js';

/** The options of request `R` in the positional form `(samples, sampleRate, options)`. */
export type OptionsOf<R> = Omit<R, 'samples' | 'sampleRate'>;

function assertNonEmptySamples(samples: ArrayLike<number>, argName = 'samples'): void {
  if (samples.length === 0) {
    throw refuse(`${argName} must be a non-empty buffer`);
  }
}

export function assertFiniteSamples(
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
      throw refuse(`${argName} contains a non-finite sample at index ${i}`);
    }
  }
}

/** A non-audio array (a feature matrix's data): each element finite, the first offender named by index. */
export function assertFiniteValues(values: ArrayLike<number>, argName: string): void {
  for (let i = 0; i < values.length; i++) {
    if (!Number.isFinite(values[i] as number)) {
      throw refuse(`${argName} contains a non-finite sample at index ${i}`);
    }
  }
}

export function assertSamples(
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  assertNonEmptySamples(samples, argName);
  assertFiniteSamples(samples, validate, argName);
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
  samples: ArrayLike<number>,
  validate: boolean,
  windowStart: number,
  windowLength: number,
  argName = 'samples',
): void {
  assertNonEmptySamples(samples, argName);
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
      throw refuse(`${argName} contains a non-finite sample at index ${i}`);
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
  samples: ArrayLike<number>,
  channels: number,
  validate: boolean,
): void {
  assertAudioSamples(samples, { scanFinite: validate });
  assertPositiveInteger(channels, 'channels');
  if (samples.length % channels !== 0) {
    throw refuse(`samples length must be a multiple of channels`);
  }
}

export function assertFiniteScalar(value: number, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${argName} must be finite`);
  }
}

/** {@link assertFiniteScalar} plus a floor of zero, for a quantity that is a magnitude. */
export function assertNonNegativeScalar(value: number, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!(Number.isFinite(value) && value >= 0)) {
    throw refuse(`${argName} must be finite and at least 0`);
  }
}

/** The mel core uses 0 as Nyquist, so callers must omit the option instead. */
export function assertMelFmaxOmitted(value: unknown): void {
  if (typeof value === 'number' && Math.fround(value) === 0) {
    throw refuse(`fmaxHz ${OMIT_FOR_POSITIVE_DEFAULT}`);
  }
}

const FLOAT32_MAX = 3.4028234663852886e38;

/** A scalar native reads as a float: `TypeError` unless a number, `RangeError` unless finite and within the 32-bit float range. */
export function assertFloatValue(value: unknown, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value) || Math.abs(value) > FLOAT32_MAX) {
    throw refuse(`${argName} must be finite and in [-3.4028235e38, 3.4028235e38]`);
  }
}

/** {@link assertFloatValue} plus a floor: strictly positive. */
export function assertPositiveFloatValue(value: unknown, argName: string): void {
  if (typeof value === 'number' && !(Number.isFinite(value) && value > 0)) {
    throw refuse(`${argName} must be finite and greater than 0`);
  }
  assertFloatValue(value, argName);
}

/** {@link assertFloatValue} plus a floor of zero. */
export function assertNonNegativeFloatValue(value: unknown, argName: string): void {
  if (typeof value === 'number' && !(Number.isFinite(value) && value >= 0)) {
    throw refuse(`${argName} must be finite and at least 0`);
  }
  assertFloatValue(value, argName);
}

/** {@link assertFloatValue} over every entry of `fields`, each named by its key. */
export function assertFloatFields(fields: Record<string, unknown>): void {
  for (const [name, value] of Object.entries(fields)) {
    assertFloatValue(value, name);
  }
}

/** A frequency band: each given bound is a positive float; their order is the core's to refuse. Absent (`undefined` / `null`) is skipped. */
export function assertFrequencyBounds(
  min: unknown,
  max: unknown,
  minName: string,
  maxName: string,
): void {
  const hasMin = !isAbsent(min);
  const hasMax = !isAbsent(max);
  if (hasMin) {
    assertPositiveFloatValue(min, minName);
  }
  if (hasMax) {
    assertPositiveFloatValue(max, maxName);
  }
}

/**
 * Offline-analysis sample-rate bounds, mirroring the C++ core limits
 * (`sonare::kMinAudioSampleRate` / `kMaxAudioSampleRate` in `core/audio.h`) and
 * the WASM/Python surfaces. Used where a wrong `sampleRate` would corrupt the
 * result rather than merely pick a default, so it must be supplied explicitly.
 */
const MIN_AUDIO_SAMPLE_RATE = 8000;
const MAX_AUDIO_SAMPLE_RATE = 384000;

/**
 * A sample rate: `TypeError` unless a number, `RangeError` unless an integer
 * inside the supported range (NaN, infinities, fractions, zero and negatives
 * included), stated as the interval either way.
 */
export function assertSampleRate(sampleRate: unknown, argName = 'sampleRate'): void {
  assertIntegerType(sampleRate, argName);
  assertSampleRateRange(sampleRate, argName);
}

function assertSampleRateRange(sampleRate: number, argName = 'sampleRate'): void {
  assertIntegerRange(sampleRate, argName, MIN_AUDIO_SAMPLE_RATE, MAX_AUDIO_SAMPLE_RATE);
}

/**
 * The preflight every one-shot taking samples and a sample rate runs before it
 * touches the native layer: a wrong-typed argument is a `TypeError` naming it,
 * and empty or non-finite audio and an out-of-range rate are a `RangeError`.
 *
 * A stereo or
 * paired entry point calls this once per buffer, naming each through `argName`.
 */
export function assertAudioInput(
  samples: unknown,
  sampleRate: unknown,
  options: object = {},
  argName = 'samples',
): void {
  assertFloat32Array(samples, argName);
  assertSampleRate(sampleRate as number);
  assertSamples(samples, (options as { scanFinite?: boolean }).scanFinite !== false, argName);
}

/** {@link assertAudioInput} for an entry point that takes no sample rate. */
export function assertAudioSamples(
  samples: unknown,
  options: object = {},
  argName = 'samples',
  allowEmpty = false,
): void {
  assertFloat32Array(samples, argName);
  if (!allowEmpty) {
    assertNonEmptySamples(samples, argName);
  }
  assertFiniteSamples(samples, (options as { scanFinite?: boolean }).scanFinite !== false, argName);
}

/** A `Float32Array` argument; anything else is a `TypeError` naming the field. */
export function assertFloat32Array(value: unknown, argName: string): asserts value is Float32Array {
  if (!(value instanceof Float32Array)) {
    throw refuseType(`${argName} must be a Float32Array`);
  }
}

/** {@link assertAudioInput} over a list of equal-rate channel planes. */
export function assertAudioChannels(
  channels: unknown,
  sampleRate: unknown,
  options: object = {},
  argName = 'channels',
): void {
  if (!Array.isArray(channels)) {
    throw refuseType(`${argName} must be an array of Float32Array`);
  }
  requireListBudget(channels, argName);
  if (channels.length === 0) {
    throw refuse(`${argName} must not be empty`);
  }
  channels.forEach((channel, i) => {
    assertAudioInput(channel, sampleRate, options, `${argName}[${i}]`);
  });
}

export function assertU7(value: number, argName: string): number {
  assertBoundedInteger(value, argName, 0, 127);
  return value;
}

export function assertNibble(value: number, argName: string): number {
  assertBoundedInteger(value, argName, 0, 15);
  return value;
}

/**
 * Split "not an integer" into the two mistakes it can be: `TypeError` when the
 * value is not a `number` at all, `RangeError` when it is a number but not an
 * integer in `[min, max]` (the native `int` by default) -- the right-type,
 * wrong-domain half, which always states the domain.
 *
 * `Number.isInteger` alone answers both, which is why a single check reads as
 * sufficient; what it cannot do is say which one happened, and a caller can act
 * only on the mistake they made. The classes are the contract on this surface,
 * so a sibling argument in the same call must not report a fraction differently
 * from this one.
 */
export function assertIntegerValue(
  value: unknown,
  argName: string,
  min = C_INT_MIN,
  max = C_INT_MAX,
): asserts value is number {
  assertBoundedInteger(value, argName, min, max);
}

/**
 * An integer in `[min, max]`: `TypeError` "must be an integer" unless a number, by type alone;
 * `RangeError` stating the interval for any other number outside it (fractions and NaN included).
 */
export function assertBoundedInteger(
  value: unknown,
  argName: string,
  min: number,
  max: number,
): asserts value is number {
  assertIntegerType(value, argName);
  assertIntegerRange(value, argName, min, max);
}

function assertIntegerType(value: unknown, argName: string): asserts value is number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
}

function assertIntegerRange(value: number, argName: string, min: number, max: number): void {
  if (!Number.isInteger(value) || value < min || value > max) {
    throw refuse(`${argName} must be an integer in [${min}, ${max}]`);
  }
}

/** The core's largest retune grain, in samples. */
const MAX_RETUNE_GRAIN_SAMPLES = 8192;

/**
 * A given retune grain: `TypeError` "must be an integer" unless a number; `RangeError` for 0, since absence is the
 * automatic grain, and for anything but a multiple of 4 in `[4, 8192]` (the hop is a quarter).
 */
export function assertRetuneGrain(value: unknown, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (value === 0) {
    throw refuse(`${argName} must not be 0; omit it for the automatic grain`);
  }
  if (
    !Number.isInteger(value) ||
    value % 4 !== 0 ||
    value < 4 ||
    value > MAX_RETUNE_GRAIN_SAMPLES
  ) {
    throw refuse(`${argName} must be a multiple of 4 in [4, ${MAX_RETUNE_GRAIN_SAMPLES}]`);
  }
}

/** A pitch class: `TypeError` "must be an integer" unless a number, `RangeError` unless an integer in `[0, 11]`. Names are refused. */
export function assertPitchClass(value: unknown, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (!Number.isInteger(value) || value < 0 || value > 11) {
    throw refuse(`${argName} must be an integer in [0, 11]`);
  }
}

/** A member of a closed string set: `TypeError` unless a string (numbers included), `RangeError` for an unknown token. */
export function assertClosedToken<T extends string>(
  value: unknown,
  allowed: readonly T[],
  argName: string,
): asserts value is T {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be one of ${allowed.join(', ')}`);
  }
  if (!allowed.includes(value as T)) {
    throw refuse(`${argName} must be one of ${allowed.join(', ')}`);
  }
}

/** A boolean argument; anything else is a `TypeError`, never a truthy coercion. */
export function requireBoolean(value: unknown, argName: string): boolean {
  if (typeof value !== 'boolean') {
    throw refuseType(`${argName} must be a boolean`);
  }
  return value;
}

/** A string argument the addon reads; anything else is a `TypeError`, never the addon's usage text. */
export function assertString(value: unknown, argName: string): void {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
}

/** Integer strictly greater than zero, up to the native `int` ceiling. */
export function assertPositiveInteger(value: unknown, argName: string): asserts value is number {
  assertBoundedInteger(value, argName, 1, C_INT_MAX);
}

/** The constant-Q family's largest hop, in samples; the core states the same bound. */
const CQT_MAX_HOP_LENGTH = 65536;

/** A constant-Q hop: an integer in `[1, CQT_MAX_HOP_LENGTH]`. */
export function assertCqtHopLength(value: unknown): asserts value is number {
  assertBoundedInteger(value, 'hopLength', 1, CQT_MAX_HOP_LENGTH);
}

/** Even integer in `[min, max]`, for an FFT-style size where only parity matters. */
export function assertEvenIntegerAtLeast(
  value: unknown,
  argName: string,
  min: number,
  max: number,
): asserts value is number {
  // Range and parity are separate refusals: 2 ** 31 is already even, so telling
  // its caller the value must be even names a property it has.
  assertBoundedInteger(value, argName, min, max);
  if (value % 2 !== 0) {
    throw refuse(`${argName} must be an even integer in [${min}, ${max}]`);
  }
}

/** The largest timeline position the engine and project accept, in quarter notes. */
const MAX_PUBLIC_PPQ = 1e12;

/** Refuses an event ppq that is not finite and inside the public timeline. */
export function assertEventPpq(ppq: number): void {
  if (!(Number.isFinite(ppq) && ppq >= 0 && ppq <= MAX_PUBLIC_PPQ)) {
    throw refuse(`ppq must be finite and in [0, 1e12]`);
  }
}

export function midi1Event(
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  assertEventPpq(ppq);
  const g = assertNibble(group, 'group');
  const ch = assertNibble(channel, 'channel');
  const d1 = assertU7(data1, 'data1');
  const d2 = assertU7(data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the WASM suite) so it cannot silently drift.
  const word = ((0x2 << 28) | (g << 24) | (status << 20) | (ch << 16) | (d1 << 8) | d2) >>> 0;
  return { ppq, word0: word, word1: 0 };
}

/** Bounds of the native `int` every addon argument below is narrowed into. */
const C_INT_MIN = -2147483648;
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
 * An out-of-domain value is a `RangeError`, the class every refused argument takes
 * on this surface: the narrowing would otherwise wrap it into the accepted domain
 * and the native refusal the caller is owed would never happen.
 *
 * That the WASM and Python surfaces answer the same input with the same class is
 * asserted by `tests/narrowing-code-parity.test.ts`, which drives one value
 * through all three, rather than left here as a claim — editing one surface's
 * validator says nothing about the other two on its own.
 */
export function assertInt32(value: unknown, argName: string): asserts value is number {
  assertBoundedInteger(value, argName, C_INT_MIN, C_INT_MAX);
}

/**
 * Check both HPSS kernels before the addon narrows them.
 *
 * Parity, positivity and the ceiling stay the core's to enforce, and it names
 * the median filter that rejected the value. What cannot be deferred is the
 * narrowing itself: a wrapped kernel arrives as a legal one and separates on it.
 */
export function assertHpssKernels(kernelHarmonic: number, kernelPercussive: number): void {
  assertInt32(kernelHarmonic, 'kernelHarmonic');
  assertInt32(kernelPercussive, 'kernelPercussive');
}

export function assertU32(value: number, argName: string): void {
  assertBoundedInteger(value, argName, 0, 0xffffffff);
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
export function assertNonNegativeSafeInteger(value: number, argName: string): void {
  assertBoundedInteger(value, argName, 0, Number.MAX_SAFE_INTEGER);
}

/** A whole safe JS integer, with fractions refused as an argument shape. */
export function assertSafeInteger(value: unknown, argName: string): asserts value is number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value) || !Number.isSafeInteger(value)) {
    if (Number.isFinite(value) && !Number.isInteger(value)) {
      throw refuseType(`${argName} must be an integer`);
    }
    throw refuse(
      `${argName} must be an integer in [${-Number.MAX_SAFE_INTEGER}, ${Number.MAX_SAFE_INTEGER}]`,
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
export function assertInt64(value: number, argName: string): void {
  assertBoundedInteger(value, argName, -Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
}

export function assertProjectMidiEvents(events: ReadonlyArray<ProjectMidiEvent>): void {
  if (!Array.isArray(events)) {
    throw refuseType(`events must be an array`);
  }
  requireListBudget(events, 'events');
  events.forEach((event: unknown, index) => {
    const prefix = `events[${index}]`;
    if (event === null || typeof event !== 'object' || Array.isArray(event)) {
      throw refuseType(`${prefix} must be a { ppq, word0, word1?, sysex? } object`);
    }
    const record = event as ProjectMidiEvent;
    if (typeof record.ppq !== 'number') {
      throw refuseType(`${prefix}.ppq must be a number`);
    }
    if (!(Number.isFinite(record.ppq) && record.ppq >= 0 && record.ppq <= MAX_PUBLIC_PPQ)) {
      throw refuse(`${prefix}.ppq must be finite and in [0, 1e12]`);
    }
    assertU32(record.word0, `${prefix}.word0`);
    if (record.word1 !== undefined) {
      assertU32(record.word1, `${prefix}.word1`);
    }
    const sysex = (event as { sysex?: unknown }).sysex;
    if (sysex !== undefined && sysex !== null && !(sysex instanceof Uint8Array)) {
      throw refuseType(`${prefix}.sysex must be a Uint8Array`);
    }
  });
}

/**
 * Convert a time in seconds to the nearest whole sample at `sampleRate`.
 *
 * Refuses a non-finite time, and one too large for the result to stay an exact
 * JS integer.
 */
export function secondsToSampleCount(seconds: number, sampleRate: number, argName: string): number {
  if (typeof seconds !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  assertFiniteScalar(seconds, argName);
  const samples = Math.round(seconds * sampleRate);
  if (!Number.isSafeInteger(samples)) {
    throw refuse(
      `${argName} must be at most ${Number.MAX_SAFE_INTEGER} samples in magnitude at ${sampleRate} Hz`,
    );
  }
  return samples;
}

/** How {@link resolveSampleBound} treats the sign of the value it resolves. */
type SampleBoundSign = 'non-negative' | 'signed';

/** {@link resolveSampleBound} for a bound the call cannot do without: neither spelling is a `TypeError`. */
export function requireSampleBound(
  sampleValue: number | null | undefined,
  secValue: number | null | undefined,
  sampleRate: number,
  sampleName: string,
  secName: string,
  sign: SampleBoundSign = 'non-negative',
): SampleBound & { readonly value: number } {
  requireOneOf([sampleName, secName], [!isAbsent(sampleValue), !isAbsent(secValue)]);
  return resolveSampleBound(
    sampleValue,
    secValue,
    sampleRate,
    sampleName,
    secName,
    sign,
  ) as SampleBound & { readonly value: number };
}

/** A sample bound read from its samples or seconds spelling, with the respelling its refusal needs. */
export interface SampleBound {
  /** The bound in samples, or undefined when neither spelling was given. */
  readonly value: number | undefined;
  /** `{ [sampleName]: { name: secName, scale: sampleRate } }` when seconds were given, else empty. */
  readonly respelled: Respelled;
}

/**
 * Resolve one time bound that is spelled either in samples or in seconds.
 *
 * Returns the declaration both the conversion and the refusal's respelling
 * read: the value, and the respelling that names the seconds spelling when it
 * was the one given.
 *
 * `undefined` / `null` on both spellings is absence, and the value is
 * `undefined` so the caller applies its own default (an end bound's default is
 * the end of the signal). Giving both spellings is refused by name. A position
 * is `'non-negative'`; a displacement (a time offset, which may move a note
 * earlier) is `'signed'`. A negative value is refused, never read as "absent".
 * `subject` is the name the native layer refuses the bound under, when it is not
 * `sampleName` (a request path such as `punch.endSample` over a bare `endSample`).
 */
export function resolveSampleBound(
  sampleValue: number | null | undefined,
  secValue: number | null | undefined,
  sampleRate: number,
  sampleName: string,
  secName: string,
  sign: SampleBoundSign = 'non-negative',
  subject: string = sampleName,
): SampleBound {
  const hasSample = !isAbsent(sampleValue);
  const hasSec = !isAbsent(secValue);
  if (!hasSample && !hasSec) {
    return { value: undefined, respelled: {} };
  }
  requireOneOf([sampleName, secName], [hasSample, hasSec]);
  const respelled: Respelled = hasSec
    ? { [subject]: { name: secName, scale: sampleRate } }
    : subject !== sampleName
      ? { [subject]: { name: sampleName, scale: 1 } }
      : {};
  const value = hasSec
    ? secondsToSampleCount(secValue as number, sampleRate, secName)
    : (sampleValue as number);
  const name = hasSec ? secName : sampleName;
  // A non-number or non-finite sample value is left for the native layer to refuse by name.
  if (typeof value !== 'number' || (!hasSec && !Number.isFinite(value))) {
    return { value, respelled };
  }
  if (sign === 'non-negative' && value < 0) {
    throw refuse(`${name} must be at least 0`);
  }
  return { value: value === 0 ? 0 : value, respelled };
}

/** C ABI value for "apply at the next block head", spelled as absence on the facade. */
const RENDER_FRAME_IMMEDIATE = -1;

/**
 * Map an optional engine `renderFrame` onto the C value: absence is "immediate"
 * (-1), and a negative frame is refused rather than read as the same thing.
 */
export function resolveRenderFrame(renderFrame: number | null | undefined): number {
  if (isAbsent(renderFrame)) {
    return RENDER_FRAME_IMMEDIATE;
  }
  if (typeof renderFrame !== 'number') {
    throw refuseType(`renderFrame must be a number`);
  }
  if (!(Number.isFinite(renderFrame) && renderFrame >= 0)) {
    throw refuse(`renderFrame must be finite and at least 0; omit it to apply immediately`);
  }
  return renderFrame;
}

/** The C ABI spelling of "absent" for the optional numeric parameters that use `-1` for it. */
const C_ABSENT = -1;

/**
 * Map an optional numeric argument onto the C value: absence (`undefined` /
 * `null`) is the C ABI's `-1` "not given", and an explicit negative or
 * non-finite value is refused rather than read as the same thing.
 */
export function resolveOptionalNonNegative(
  value: number | null | undefined,
  argName: string,
): number {
  if (isAbsent(value)) {
    return C_ABSENT;
  }
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!(Number.isFinite(value) && value >= 0)) {
    throw refuse(`${argName} must be finite and at least 0; omit it for the default`);
  }
  return value;
}

/** The declared field names of `fnName`'s request. */
type RequestKeysOf<N extends RequestFn> = (typeof REQUEST_KEYS)[N];

type NestedKeysOf<N extends RequestFn> = N extends keyof typeof NESTED_REQUEST_KEYS
  ? (typeof NESTED_REQUEST_KEYS)[N]
  : Record<never, never>;

/** Field names present in exactly one of `A` and `B`. */
type KeyDrift<A, B> = Exclude<keyof A, keyof B> | Exclude<keyof B, keyof A>;

/** The object a nested field carries: the field itself, or each element of an array field. */
type NestedObject<V> = V extends readonly (infer E)[] ? E : V;

/** Drift of every nested object whose keys are refused, named by the field holding it. */
type NestedDrift<T, N extends RequestFn> = {
  [F in keyof NestedKeysOf<N>]: F extends keyof T
    ? KeyDrift<NestedObject<Exclude<NonNullable<T[F]>, string>>, NestedKeysOf<N>[F]>
    : F;
}[keyof NestedKeysOf<N>];

/**
 * `unknown` while the request type and its declared key set agree, otherwise an
 * object type no request satisfies, so a field added to a request type without
 * its key-set entry (or the reverse) fails to compile at the call.
 */
type RequestKeysAgree<T, N extends RequestFn> = [
  KeyDrift<T, RequestKeysOf<N>> | NestedDrift<T, N>,
] extends [never]
  ? unknown
  : { requestKeysDrift: KeyDrift<T, RequestKeysOf<N>> | NestedDrift<T, N> };

function ownKey(keys: object, key: string): boolean {
  return Object.hasOwn(keys, key);
}

/**
 * Refuse the first key of `value` outside `fnName`'s declared request fields,
 * and the first key of a nested options object outside its declared fields. A
 * key that belongs to a nested object is reported with the field it goes under.
 */
export function assertKnownRequestKeys(
  requestName: RequestFn,
  value: Record<string, unknown>,
): void {
  const keys: object = REQUEST_KEYS[requestName];
  const nested: Record<string, object> =
    NESTED_REQUEST_KEYS[requestName as keyof typeof NESTED_REQUEST_KEYS] ?? {};
  for (const key of Object.keys(value)) {
    if (ownKey(keys, key)) {
      continue;
    }
    const owner = Object.entries(nested).find(([, set]) => ownKey(set, key));
    throw refuseUnknownKey(key, Object.keys(keys), owner && `${owner[0]}.${key}`);
  }
  for (const [field, set] of Object.entries(nested)) {
    const options = value[field];
    // A nested field is one object, or an array whose elements each carry the declared keys.
    const list: readonly unknown[] = Array.isArray(options) ? options : [options];
    if (Array.isArray(options)) {
      requireListBudget(options, field);
    }
    for (let i = 0; i < list.length; i++) {
      const item = list[i];
      const path = Array.isArray(options) ? `${field}[${i}]` : field;
      if (!isPlainRecord(item)) {
        continue;
      }
      assertDeclaredKeys(item, Object.keys(set), path);
    }
  }
}

/** The request fields a positional `(samples, sampleRate, options?)` call passes by position. */
const POSITIONAL_KEYS = ['samples', 'sampleRate'];

/** The keys an options bag may carry: the request's, less the ones passed by position. */
function bagKeys(keys: object): string[] {
  return Object.keys(keys).filter((key) => !POSITIONAL_KEYS.includes(key));
}

/**
 * The options bag of a positional overload, held to the same declared fields
 * as the request form: an unknown key is refused (`TypeError`), naming it.
 */
export function optionsBag<O>(requestName: RequestFn, options: O): O {
  if (isPlainRecord(options)) {
    const keys: object = REQUEST_KEYS[requestName];
    for (const key of Object.keys(options)) {
      if (!ownKey(keys, key)) {
        throw refuseUnknownKey(key, bagKeys(keys));
      }
    }
  }
  return options;
}

/**
 * The request-object form of a positional entry point: its first argument is
 * either the audio buffer or a request, so anything else (null, a number, an
 * array, another typed array) is a wrong-typed buffer and is reported as one,
 * naming the field, instead of failing on a property read. An entry point that
 * takes only a request (`requestOnly`) reports the request itself instead.
 *
 * A request carrying a key its entry point does not declare is refused, naming
 * the key (`TypeError`), as is an unknown key inside a nested options object.
 */
export function requestObject<T, N extends RequestFn>(
  requestName: N,
  value: T & RequestKeysAgree<T, N>,
  argName = 'samples',
  requestOnly = false,
): T {
  if (!isPlainRecord(value)) {
    if (requestOnly) {
      throw refuseType(`request must be an object`);
    }
    assertAudioSamples(value, {}, argName);
  } else {
    assertKnownRequestKeys(requestName, value);
  }
  return value;
}

/** A request carrying audio plus the rate it was recorded at. */
interface SampleRequest {
  samples: Float32Array;
  sampleRate: number;
}

function plainObject(value: unknown): value is Record<string, unknown> {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !ArrayBuffer.isView(value)
  );
}

function assertNoExtraArguments(args: readonly unknown[], from: number): void {
  for (let i = from; i < args.length; i++) {
    if (args[i] !== undefined) {
      throw refuseType(`unexpected argument after the request object`);
    }
  }
}

/**
 * Normalises the two accepted call forms of a one-shot over audio into one
 * request: `(request)` and `(samples, sampleRate, options?)`.
 *
 * Throws `TypeError` for a wrong-typed argument, a missing `sampleRate`, an
 * extra argument after a request object, an unknown key, or `samples` /
 * `sampleRate` repeated inside `options`; `RangeError` for an out-of-domain
 * sample rate or audio. The caller states the request type it expects.
 */
export function sampleOneShot<R extends SampleRequest>(
  requestName: RequestFn,
  args: readonly unknown[],
  checkShape?: (request: R) => void,
): R {
  const first = args[0];
  let request: Record<string, unknown>;
  if (first instanceof Float32Array || !plainObject(first)) {
    if (args.length > 3) {
      throw refuseType(`expected (samples, sampleRate, options?)`);
    }
    const options = args[2] ?? {};
    if (!plainObject(options)) {
      throw refuseType(`options must be an object`);
    }
    for (const key of POSITIONAL_KEYS) {
      if (Object.keys(options).includes(key)) {
        throw refuseUnknownKey(key, bagKeys(REQUEST_KEYS[requestName]));
      }
    }
    optionsBag(requestName, options);
    request = { ...options, samples: first, sampleRate: args[1] };
  } else {
    assertNoExtraArguments(args, 1);
    assertKnownRequestKeys(requestName, first);
    request = first;
  }
  // Percussive entries also accept plain arrays and convert them after normalization.
  const samples = request.samples;
  const acceptsSampleArray =
    requestName === 'extractPercussiveEvents' || requestName === 'renderPercussiveEvents';
  if (!acceptsSampleArray || !Array.isArray(samples)) {
    assertFloat32Array(samples, 'samples');
  }
  const sampleRate = request.sampleRate;
  assertIntegerType(sampleRate, 'sampleRate');
  const normalized = request as unknown as R;
  checkShape?.(normalized);
  assertSampleRateRange(sampleRate);
  return normalized;
}

/**
 * Normalises a one-shot that takes no audio: exactly one request object. Throws
 * `TypeError` for any other first argument, an extra argument, or an unknown key.
 */
export function requestOnly<R extends object>(requestName: RequestFn, args: readonly unknown[]): R {
  const request = args[0];
  if (!plainObject(request)) {
    throw refuseType(`request must be an object`);
  }
  assertNoExtraArguments(args, 1);
  assertKnownRequestKeys(requestName, request);
  return request as R;
}
