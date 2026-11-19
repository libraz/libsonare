import { refuse, refuseType } from './internal/entry.js';
import { requireOneOf } from './internal/one_of.js';
import type { Matrix2D } from './public_types.js';
import { REQUEST_SHAPES, type RequestFn, type RequestShape } from './request_keys.js';

/** The options of request `R` in the positional form `(samples, sampleRate, options)`. */
export type OptionsOf<R> = Omit<R, 'samples' | 'sampleRate'>;

/**
 * Offline-analysis sample-rate bounds, mirroring the C++ core limits
 * (`sonare::kMinAudioSampleRate` / `kMaxAudioSampleRate` in `core/audio.h`).
 * Every guarded WASM entry point rejects the same out-of-range rates the C ABI,
 * Node, and Python surfaces do.
 */
export const MIN_AUDIO_SAMPLE_RATE = 8000;
export const MAX_AUDIO_SAMPLE_RATE = 384000;

function assertNonEmptySamples(samples: ArrayLike<number>, argName = 'samples'): void {
  if (samples.length === 0) {
    throw refuse(`${argName} must not be empty`);
  }
}

function assertFiniteSamples(
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
      throw refuse(`${argName} contains a non-finite value at index ${i}`);
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

export function assertFiniteScalar(value: number, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${argName} must be a finite number`);
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

/** A scalar the native layer reads as a float; only the type is checked, so NaN and infinities pass through. */
export function assertNumber(value: unknown, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
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

/** A frequency band: each given bound is a positive float, and a given pair is strictly ordered. Absent (`undefined` / `null`) is skipped. */
export function assertFrequencyBounds(
  min: unknown,
  max: unknown,
  minName: string,
  maxName: string,
): void {
  const hasMin = min !== undefined && min !== null;
  const hasMax = max !== undefined && max !== null;
  if (hasMin) {
    assertPositiveFloatValue(min, minName);
  }
  if (hasMax) {
    assertPositiveFloatValue(max, maxName);
  }
  if (hasMin && hasMax && !((max as number) > (min as number))) {
    throw refuse(`${maxName} must be greater than ${minName}`);
  }
}

/**
 * A sample rate: `TypeError` unless a number, `RangeError` unless an integer
 * inside the supported range (NaN, infinities, fractions, zero and negatives
 * included).
 */
export function assertSampleRate(sampleRate: unknown, argName = 'sampleRate'): void {
  if (typeof sampleRate !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  // Two refusals, not one: 22050.7 sits inside the range, so reporting it as
  // out of range names an argument that is not the one at fault. `argName` is
  // the same point for a rate the caller spelled something else, such as a
  // resampler's source and target.
  if (!Number.isInteger(sampleRate)) {
    throw refuse(`${argName} must be an integer`);
  }
  if (sampleRate < MIN_AUDIO_SAMPLE_RATE || sampleRate > MAX_AUDIO_SAMPLE_RATE) {
    throw refuse(
      `${argName} out of supported range [${MIN_AUDIO_SAMPLE_RATE}, ${MAX_AUDIO_SAMPLE_RATE}]`,
    );
  }
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
): void {
  assertFloat32Array(samples, argName);
  assertSamples(samples, (options as { scanFinite?: boolean }).scanFinite !== false, argName);
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
  if (channels.length === 0) {
    throw refuse(`${argName} must not be empty`);
  }
  channels.forEach((channel, i) => {
    assertAudioInput(channel, sampleRate, options, `${argName}[${i}]`);
  });
}

/** A boolean argument the native layer reads positionally; anything else is a `TypeError`, never a truthy coercion. */
export function requireBoolean(value: unknown, argName: string): boolean {
  if (typeof value !== 'boolean') {
    throw refuseType(`${argName} must be a boolean`);
  }
  return value;
}

/** An optional boolean: `undefined` and `null` take the fallback, any other non-boolean is a `TypeError`. */
export function optionalBoolean(value: unknown, argName: string, fallback: boolean): boolean {
  return value === undefined || value === null ? fallback : requireBoolean(value, argName);
}

/** A string argument the native layer reads through embind; anything else is a `TypeError`, never a raw `BindingError`. */
export function assertString(value: unknown, argName: string): void {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
}

/** An optional string: `undefined` and `null` take the fallback, any other non-string is a `TypeError`. */
export function optionalString(value: unknown, argName: string, fallback: string): string {
  if (value === undefined || value === null) {
    return fallback;
  }
  assertString(value, argName);
  return value as string;
}

/** Validate and retain the public Audio.fromBuffer construction contract. */
export function validateAudioBuffer(samples: Float32Array, sampleRate: number): void {
  assertSamples(samples, true);
  assertSampleRate(sampleRate);
}

/** Bounds of the native `int` every embind argument below is narrowed into. */
export const C_INT_MIN = -2147483648;
export const C_INT_MAX = 2147483647;

/**
 * Reject an argument embind's declared-`int` narrowing would wrap.
 *
 * A positional embind parameter declared `int` WRAPS rather than saturates, so
 * a kernel of `2 ** 32` arrives as 0 and `2 ** 32 + 1` as 1 — both values the
 * native guards accept, so the call succeeds having separated on a setting the
 * caller never asked for. (The options-object path narrows through
 * `checkedIntFromVal`, which refuses the same inputs in the module; this is the
 * positional path's equivalent.)
 *
 * Reported as a `RangeError`, like every refused argument: the check pre-empts a
 * native refusal the caller would have received had the narrowing not wrapped the
 * value into the accepted domain first.
 */
export function assertInt32(value: number, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isInteger(value) || value < C_INT_MIN || value > C_INT_MAX) {
    throw refuse(`${argName} must be an integer in [${C_INT_MIN}, ${C_INT_MAX}]`);
  }
}

/**
 * Check both HPSS kernels before embind narrows them.
 *
 * Parity, positivity and the ceiling stay the core's to enforce, and it names
 * the median filter that rejected the value. What cannot be deferred is the
 * narrowing itself: a wrapped kernel arrives as a legal one and separates on it.
 *
 * These two arrive POSITIONALLY, so they never pass through the options-bag
 * reader and inherit none of its checks. That is what separates this from
 * {@link assertPercussiveSeparation}, which duplicates a reader check to improve
 * a message; here there is no reader to duplicate.
 */
export function assertHpssKernels(kernelHarmonic: number, kernelPercussive: number): void {
  assertInt32(kernelHarmonic, 'kernelHarmonic');
  assertInt32(kernelPercussive, 'kernelPercussive');
}

function assertIntegralField(value: number, argName: string): void {
  if (!Number.isInteger(value)) {
    throw refuse(`${argName} must be an integer`);
  }
}

/**
 * Percussive-event separation fields, as the request objects carry them.
 */
export interface PercussiveSeparationFields {
  nFft?: number;
  hopLength?: number;
  hpssKernelHarmonic?: number;
  hpssKernelPercussive?: number;
}

/**
 * Check the percussive-event separation's framing and kernels for integrality.
 *
 * All four fields reach the module through one options-bag reader, which now
 * refuses a fractional value itself, so this is the diagnostic rather than the
 * guarantee: it fires first and names the function, where the reader can only
 * name the field. Both reject the same set, so they cannot disagree about an
 * input — only about how the message reads. Do not narrow this to fields the
 * reader misses; there are none, and a check scoped to a gap that no longer
 * exists is how a stale justification outlives its divergence.
 *
 * The fields are iterated rather than named at each call site so a new one is
 * visible here. Absence means the default, so an omitted field is not resolved.
 */
export function assertPercussiveSeparation(options: PercussiveSeparationFields): void {
  const fields = ['nFft', 'hopLength', 'hpssKernelHarmonic', 'hpssKernelPercussive'] as const;
  for (const field of fields) {
    const value = options[field];
    if (value !== undefined) {
      assertIntegralField(value, field);
      // 0 is the native default, so an explicit 0 would separate on it while reporting success.
      if (value === 0) {
        throw refuse(`${field} must not be 0; omit it for the library value`);
      }
    }
  }
}

/** Integer strictly greater than zero, up to the native `int` ceiling. */
export function assertPositiveInteger(value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 1 || value > C_INT_MAX) {
    throw refuse(`${argName} must be an integer in [1, ${C_INT_MAX}]`);
  }
}

/**
 * Split "not an integer" into the two mistakes it can be: `TypeError` when the
 * value is not a `number` at all, `RangeError` when it is a number but not
 * integral -- the right-type, wrong-domain half of the same message.
 *
 * `Number.isInteger` alone answers both, which is why a single check reads as
 * sufficient; what it cannot do is say which one happened, and a caller can act
 * only on the mistake they made. The classes are the contract on this surface,
 * so a sibling argument in the same call must not report a fraction differently
 * from this one.
 */
export function assertIntegerValue(value: unknown, argName: string): asserts value is number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (!Number.isInteger(value)) {
    throw refuse(`${argName} must be an integer`);
  }
}

/** Even integer in `[min, max]`, for an FFT-style size where only parity matters. */
export function assertEvenIntegerAtLeast(
  value: number,
  argName: string,
  min: number,
  max: number,
): void {
  // Range and parity are separate refusals: 2 ** 31 is already even, so telling
  // its caller the value must be even names a property it has.
  if (!Number.isInteger(value) || value < min || value > max) {
    throw refuse(`${argName} must be an integer in [${min}, ${max}]`);
  }
  if (value % 2 !== 0) {
    throw refuse(`${argName} must be an even integer in [${min}, ${max}]`);
  }
}

/** General integer-in-`[min, max]` check, for a bound {@link assertU7}/{@link assertNibble} don't cover. */
export function assertBoundedInteger(
  value: number,
  argName: string,
  min: number,
  max: number,
): void {
  if (!Number.isInteger(value) || value < min || value > max) {
    throw refuse(`${argName} must be an integer in [${min}, ${max}]`);
  }
}

/** The core's largest retune grain, in samples. */
const MAX_RETUNE_GRAIN_SAMPLES = 8192;

/**
 * A given retune grain: `TypeError` unless a number; `RangeError` for 0, since absence is the
 * automatic grain, and for anything but a multiple of 4 in `[4, 8192]` (the hop is a quarter).
 */
export function assertRetuneGrain(value: unknown, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
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

export function assertU7(value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    throw refuse(`${argName} must be an integer in [0, 127]`);
  }
  return value;
}

export function assertNibble(value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw refuse(`${argName} must be an integer in [0, 15]`);
  }
  return value;
}

/** A count that may be zero: `TypeError` unless a number, `RangeError` unless a non-negative safe integer. */
export function assertNonNegativeSafeInteger(value: number, argName: string): void {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (!Number.isSafeInteger(value) || value < 0) {
    throw refuse(`${argName} must be an integer in [0, ${Number.MAX_SAFE_INTEGER}]`);
  }
}

export function assertU32(value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw refuse(`${argName} must be an integer in [0, 4294967295]`);
  }
}

/**
 * Convert a caller-supplied index array to `Int32Array`, refusing any element
 * the conversion would change rather than folding it.
 *
 * `Int32Array.from(values, Math.trunc)` turns the sample index `1000.7` into
 * `1000` and `2 ** 31` into `-2 ** 31`. The C ABI takes `const int*`, so both
 * arrive as values nothing downstream can separate from ones the caller chose.
 * An `Int32Array` is returned as-is: its elements are already exact.
 */
export function toInt32Array(values: Int32Array | ArrayLike<number>, argName: string): Int32Array {
  if (values instanceof Int32Array) {
    return values;
  }
  if (
    values === null ||
    typeof values !== 'object' ||
    !Number.isInteger((values as ArrayLike<number>).length) ||
    (values as ArrayLike<number>).length < 0
  ) {
    throw refuseType(`${argName} must be an Int32Array or an array of numbers`);
  }
  const out = new Int32Array(values.length);
  for (let i = 0; i < values.length; i++) {
    const element = values[i];
    // The element shape of `assertIntegerValue`, and it splits the same way: the
    // class says whether the entry was the wrong type or the wrong domain, and a
    // sibling element in the same array must not report a fraction differently.
    if (typeof element !== 'number') {
      throw refuseType(`${argName}[${i}] must be an integer`);
    }
    if (!Number.isInteger(element)) {
      throw refuse(`${argName}[${i}] must be an integer`);
    }
    if (element < C_INT_MIN || element > C_INT_MAX) {
      throw refuse(`${argName}[${i}] must be an integer in [${C_INT_MIN}, ${C_INT_MAX}]`);
    }
    out[i] = element;
  }
  return out;
}

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
    throw refuse(`${argName} is out of range at ${sampleRate} Hz`);
  }
  return samples;
}

/** How {@link resolveSampleBound} treats the sign of the value it resolves. */
export type SampleBoundSign = 'non-negative' | 'signed';

/** {@link resolveSampleBound} for a bound the call cannot do without: neither spelling is a `TypeError`. */
export function requireSampleBound(
  sampleValue: number | null | undefined,
  secValue: number | null | undefined,
  sampleRate: number,
  sampleName: string,
  secName: string,
  sign: SampleBoundSign = 'non-negative',
): number {
  requireOneOf(
    [sampleName, secName],
    [
      sampleValue !== undefined && sampleValue !== null,
      secValue !== undefined && secValue !== null,
    ],
  );
  return resolveSampleBound(sampleValue, secValue, sampleRate, sampleName, secName, sign) as number;
}

/**
 * Resolve one time bound that is spelled either in samples or in seconds.
 *
 * `undefined` / `null` on both spellings is absence, and the result is
 * `undefined` so the caller applies its own default (an end bound's default is
 * the end of the signal). Giving both spellings is refused by name. A position
 * is `'non-negative'`; a displacement (a time offset, which may move a note
 * earlier) is `'signed'`. A negative value is refused, never read as "absent".
 */
export function resolveSampleBound(
  sampleValue: number | null | undefined,
  secValue: number | null | undefined,
  sampleRate: number,
  sampleName: string,
  secName: string,
  sign: SampleBoundSign = 'non-negative',
): number | undefined {
  const hasSample = sampleValue !== undefined && sampleValue !== null;
  const hasSec = secValue !== undefined && secValue !== null;
  if (!hasSample && !hasSec) {
    return undefined;
  }
  requireOneOf([sampleName, secName], [hasSample, hasSec]);
  const value = hasSec
    ? secondsToSampleCount(secValue as number, sampleRate, secName)
    : (sampleValue as number);
  const name = hasSec ? secName : sampleName;
  // A non-number or non-finite sample value is left for the native layer to refuse by name.
  if (typeof value !== 'number' || (!hasSec && !Number.isFinite(value))) {
    return value;
  }
  if (sign === 'non-negative' && value < 0) {
    throw refuse(`${name} must be at least 0`);
  }
  return value === 0 ? 0 : value;
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
  if (value === undefined || value === null) {
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

/**
 * Map an optional engine `renderFrame` onto the C value: absence is "immediate"
 * (-1), and a negative frame is refused rather than read as the same thing.
 */
export function resolveRenderFrame(renderFrame: number | null | undefined): number {
  if (renderFrame === undefined || renderFrame === null) {
    return C_ABSENT;
  }
  if (typeof renderFrame !== 'number') {
    throw refuseType(`renderFrame must be a number`);
  }
  if (!(Number.isFinite(renderFrame) && renderFrame >= 0)) {
    throw refuse(`renderFrame must be finite and at least 0; omit it to apply immediately`);
  }
  return renderFrame;
}

/**
 * Refuses a request key the entry point does not declare, and a key of a nested
 * options object it forwards to native code that the options do not declare. A
 * declared key whose value is `undefined` is absent, as everywhere else. A key
 * that belongs to a nested object is pointed at its field.
 */
export function assertKnownRequestKeys(requestName: RequestFn, request: object): void {
  const shape: RequestShape = REQUEST_SHAPES[requestName];
  for (const key of Object.keys(request)) {
    if (shape.keys.includes(key)) {
      continue;
    }
    const owner = Object.entries({ ...shape.nested, ...shape.hints }).find(([, keys]) =>
      keys.includes(key),
    );
    const hint = owner === undefined ? '' : ` ('${key}' belongs under '${owner[0]}')`;
    throw refuseType(`unknown request key '${key}'${hint}`);
  }
  for (const [field, keys] of Object.entries(shape.nested ?? {})) {
    const value = (request as Record<string, unknown>)[field];
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      continue;
    }
    for (const key of Object.keys(value)) {
      if (!keys.includes(key)) {
        throw refuseType(`unknown option '${key}' in '${field}'`);
      }
    }
  }
}

/**
 * The options bag of a positional overload, which becomes part of the request
 * the entry point reads: it declares the same keys as the request form, and a
 * key outside them is refused by name rather than dropped.
 */
export function optionsBag<T extends object>(requestName: RequestFn, options: T): T {
  const shape: RequestShape = REQUEST_SHAPES[requestName];
  for (const key of Object.keys(options ?? {})) {
    if (!shape.keys.includes(key)) {
      throw refuseType(`unknown option '${key}'`);
    }
  }
  return options;
}

/**
 * The request-object form of a positional entry point: its first argument is
 * either the audio buffer or a request, so anything else (null, a number, an
 * array, another typed array) is a wrong-typed buffer and is reported as one,
 * naming the field, instead of failing on a property read. An entry point that
 * takes only a request (`requestOnly`) reports the request itself instead. A
 * request carrying a key the entry point does not declare is refused by name.
 */
export function requestObject<T>(
  requestName: RequestFn,
  value: T,
  argName = 'samples',
  requestOnly = false,
): T {
  if (
    typeof value !== 'object' ||
    value === null ||
    Array.isArray(value) ||
    ArrayBuffer.isView(value)
  ) {
    if (requestOnly) {
      throw refuseType(`request must be an object`);
    }
    assertAudioSamples(value, {}, argName);
    return value;
  }
  assertKnownRequestKeys(requestName, value);
  return value;
}

/** Builds a {@link Matrix2D} from a native flat buffer and its dimensions. */
export function toMatrix2D(data: Float32Array, rows: number, cols: number): Matrix2D {
  return { data, rows, cols };
}

/** Builds a {@link Matrix2D} whose cols are read back from the buffer native returned, given its row count. */
export function toMatrix2DByRows(data: Float32Array, rows: number): Matrix2D {
  return { data, rows, cols: rows > 0 ? data.length / rows : 0 };
}

/**
 * Checks a caller-supplied {@link Matrix2D}: `TypeError` unless an object with a
 * `Float32Array` `data` and integer `rows` / `cols`, `RangeError` for a
 * non-positive dimension, a `data` length other than `rows * cols`, or (unless
 * `validate` is `false`) a NaN or infinity.
 */
export function assertMatrix2D(value: unknown, argName: string, validate = true): Matrix2D {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw refuseType(`${argName} must be a Matrix2D`);
  }
  const m = value as Partial<Matrix2D>;
  if (!(m.data instanceof Float32Array)) {
    throw refuseType(`${argName}.data must be a Float32Array`);
  }
  assertIntegerValue(m.rows as number, `${argName}.rows`);
  assertIntegerValue(m.cols as number, `${argName}.cols`);
  assertPositiveInteger(m.rows as number, `${argName}.rows`);
  assertPositiveInteger(m.cols as number, `${argName}.cols`);
  const expected = (m.rows as number) * (m.cols as number);
  if (!Number.isSafeInteger(expected) || m.data.length !== expected) {
    throw refuse(`${argName}.data length must equal rows * cols`);
  }
  if (validate) {
    assertFiniteValues(m.data, `${argName}.data`);
  }
  return m as Matrix2D;
}

/**
 * A closed string set: `TypeError` unless a string, `RangeError` for a string
 * outside `tokens`. Absent (`undefined` / `null`) takes `fallback`.
 */
export function requireToken<T extends string>(
  value: unknown,
  argName: string,
  tokens: readonly T[],
  fallback: T,
): T {
  return optionalToken(value, argName, tokens) ?? fallback;
}

/** {@link requireToken} for a field whose default the library owns: absent stays `undefined`. */
export function optionalToken<T extends string>(
  value: unknown,
  argName: string,
  tokens: readonly T[],
): T | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  assertString(value, argName);
  if (!(tokens as readonly string[]).includes(value as string)) {
    throw refuse(`${argName} must be one of ${tokens.join(', ')}`);
  }
  return value as T;
}

/** `estimateTuning` / `pitchTuning` report a semitone fraction (librosa mirrors); the JS surface speaks cents. */
export function tuningCentsFromNative(semitoneFraction: number): number {
  return semitoneFraction * 100;
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
): R {
  const first = args[0];
  if (first instanceof Float32Array) {
    if (args.length > 3) {
      throw refuseType(`expected (samples, sampleRate, options?)`);
    }
    const sampleRate = args[1];
    assertSampleRate(sampleRate);
    const options = args[2] ?? {};
    if (!plainObject(options)) {
      throw refuseType(`options must be an object`);
    }
    for (const key of ['samples', 'sampleRate']) {
      if (Object.keys(options).includes(key)) {
        throw refuseType(`unknown option '${key}'`);
      }
    }
    optionsBag(requestName, options);
    return { ...options, samples: first, sampleRate } as unknown as R;
  }
  if (plainObject(first)) {
    assertNoExtraArguments(args, 1);
    assertKnownRequestKeys(requestName, first);
    assertSampleRate(first.sampleRate);
    return first as unknown as R;
  }
  assertAudioSamples(first, {}, 'samples');
  throw refuseType(`expected a request object or a Float32Array`);
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

/** Refuse a surround pan field outside its range; the engine and mixer never clamp it. */
export function assertSurroundPan(pan: {
  azimuth?: number;
  divergence?: number;
  lfe?: number;
}): void {
  const check = (name: 'azimuth' | 'divergence' | 'lfe', min: number, max: number): void => {
    const value = pan[name];
    if (value === undefined || value === null) {
      return;
    }
    if (typeof value !== 'number') {
      throw refuseType(`${name} must be a number`);
    }
    if (!(value >= min && value <= max)) {
      throw refuse(`${name} must be finite and in [${min}, ${max}]`);
    }
  };
  check('azimuth', -180, 180);
  check('divergence', 0, 1);
  check('lfe', 0, 1);
}
