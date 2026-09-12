import { ErrorCode, SonareError } from './errors';

/**
 * Per-call validation options accepted by guarded wrappers. Empty-buffer
 * checks are always performed; pass `{ validate: false }` to opt out of the
 * O(n) NaN/Inf scan on hot paths.
 *
 * `{ validate: false }` only skips this JS-side pre-scan (which raises a
 * `RangeError` naming the exact offending index). It is NOT a way to push
 * non-finite samples into the core: the native layer always re-validates the
 * buffer (see `validate_offline_audio_input` in the C++ core), matching the C
 * ABI / Node / Python surfaces, so an NaN/Inf buffer still throws — just with a
 * generic native message instead of the indexed JS one.
 */
export interface ValidateOptions {
  validate?: boolean;
}

/**
 * Offline-analysis sample-rate bounds, mirroring the C++ core limits
 * (`sonare::kMinAudioSampleRate` / `kMaxAudioSampleRate` in `core/audio.h`).
 * Every guarded WASM entry point rejects the same out-of-range rates the C ABI,
 * Node, and Python surfaces do.
 */
export const MIN_AUDIO_SAMPLE_RATE = 8000;
export const MAX_AUDIO_SAMPLE_RATE = 384000;

function assertNonEmptySamples(
  fnName: string,
  samples: ArrayLike<number>,
  argName = 'samples',
): void {
  if (samples.length === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be empty`);
  }
}

function assertFiniteSamples(
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

export function assertFiniteScalar(fnName: string, value: number, argName: string): void {
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a finite number`);
  }
}

export function assertSampleRate(fnName: string, sampleRate: number): void {
  if (
    !Number.isInteger(sampleRate) ||
    sampleRate < MIN_AUDIO_SAMPLE_RATE ||
    sampleRate > MAX_AUDIO_SAMPLE_RATE
  ) {
    throw new RangeError(
      `${fnName}: sampleRate out of supported range [${MIN_AUDIO_SAMPLE_RATE}, ${MAX_AUDIO_SAMPLE_RATE}]`,
    );
  }
}

/** Validate and retain the public Audio.fromBuffer construction contract. */
export function validateAudioBuffer(samples: Float32Array, sampleRate: number): void {
  assertSamples('Audio.fromBuffer', samples, true);
  assertSampleRate('Audio.fromBuffer', sampleRate);
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
 * Reported as the branded `SonareError` carrying `InvalidParameter` rather than
 * a `RangeError`, because the class follows what the rejection stands in for: a
 * `RangeError` is this surface refusing an argument on its own authority, while
 * this one pre-empts a native refusal the caller would have received under that
 * code had the narrowing not wrapped the value into the accepted domain first.
 * The agreement with the Node and Python surfaces is asserted by the Node
 * package's `tests/narrowing-code-parity.test.ts`, which drives one value
 * through all three — weakening this check turns that red.
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
 * Check both HPSS kernels before embind narrows them.
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

function assertIntegralField(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value)) {
    throw new SonareError(
      ErrorCode.InvalidParameter,
      'InvalidParameter',
      `${fnName}: ${argName} must be an integer`,
    );
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
 * All four fields reach the module through one options-bag reader, which
 * refuses a non-finite or out-of-range value by name — but it narrows with a
 * cast, so a fractional value truncates instead. 31.5 separates on 31, and
 * anything in `(-1, 0)` truncates to the 0 every one of these fields reads as
 * "keep the default", so the call succeeds on a framing the caller never asked
 * for. Range stays the reader's, which names the field it rejected;
 * integrality is what cannot be deferred to it.
 *
 * The fields are iterated rather than named at each call site so a new one is
 * visible here. Absence means the default, so an omitted field is not resolved.
 */
export function assertPercussiveSeparation(
  fnName: string,
  options: PercussiveSeparationFields,
): void {
  const fields = ['nFft', 'hopLength', 'hpssKernelHarmonic', 'hpssKernelPercussive'] as const;
  for (const field of fields) {
    const value = options[field];
    if (value !== undefined) {
      assertIntegralField(fnName, value, field);
    }
  }
}

export function assertNonNegativeInteger(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0) {
    throw new RangeError(`${fnName}: ${argName} must be a non-negative integer`);
  }
}

export function assertPositiveInteger(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value <= 0) {
    throw new RangeError(`${fnName}: ${argName} must be a positive integer`);
  }
}

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
