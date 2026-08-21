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

/**
 * Resolves and validates the `nFft` / `hopLength` pair every STFT-backed entry
 * point takes.
 *
 * The core FFT is mixed-radix, so any even size transforms exactly; only the
 * real one-sided spectrum's `n_fft / 2 + 1` bin layout needs the evenness. A
 * power-of-two restriction rejects sizes the C ABI and the native CLI accept,
 * which is what a second copy of this rule used to do: `hpss({ nFft: 1536 })`
 * worked while `hpssWithResidual({ nFft: 1536 })` threw, with the message
 * asserting a constraint this project had explicitly written down as untrue.
 * One implementation, so the two cannot disagree again.
 */
export function resolveFftOptions(
  fnName: string,
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  const resolvedNFft = nFft === undefined ? 2048 : nFft;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  if (typeof resolvedNFft !== 'number' || !Number.isInteger(resolvedNFft)) {
    throw new TypeError(`${fnName}: nFft must be an integer`);
  }
  if (resolvedNFft < 2 || resolvedNFft > 2 ** 30 || resolvedNFft % 2 !== 0) {
    throw new RangeError(`${fnName}: nFft must be an even integer >= 2`);
  }
  if (typeof resolvedHopLength !== 'number' || !Number.isInteger(resolvedHopLength)) {
    throw new TypeError(`${fnName}: hopLength must be an integer`);
  }
  if (resolvedHopLength <= 0 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: hopLength must be a positive integer`);
  }
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}
