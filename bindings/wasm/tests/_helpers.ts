/**
 * Shared test-signal generators and assertions for the WASM binding suites.
 */

import { expect } from 'vitest';
import { isSonareError } from '../dist/index.js';

/** Options for {@link sine}. */
export interface SineOptions {
  /** Peak amplitude of the generated tone. Defaults to 0.5. */
  amp?: number;
  /** Sample rate in Hz. Defaults to 48000. */
  sampleRate?: number;
}

/**
 * Generates a mono sine tone as a Float32Array.
 *
 * @param freqHz - Tone frequency in Hz.
 * @param durationSec - Duration in seconds; the sample count is
 *   `floor(sampleRate * durationSec)`.
 * @param opts - Optional amplitude and sample-rate overrides.
 * @returns The generated tone.
 */
export function sine(freqHz: number, durationSec: number, opts: SineOptions = {}): Float32Array {
  const { amp = 0.5, sampleRate = 48000 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}

/**
 * Asserts that a `Mixer.stripById()` lookup resolved, narrowing away the `null`
 * it returns for an unknown id so the index can be passed to the strip-indexed
 * setters and meter readers.
 *
 * @param index - The `stripById()` return value.
 * @param id - The scene strip id that was looked up, used in the failure message.
 * @throws Error when the lookup missed.
 */
export function assertStripIndex(index: number | null, id: string): asserts index is number {
  if (index === null) {
    throw new Error(`Mixer.stripById(${JSON.stringify(id)}) did not resolve to a strip index`);
  }
}

/** The native class a refused argument surfaces as. */
export type ArgumentRefusalClass = typeof RangeError | typeof TypeError;

/**
 * Asserts that a caught value is an argument the binding refused itself: a
 * native `RangeError` (out of domain, the default) or `TypeError` (wrong type),
 * and not a coded `SonareError`, which is what a library or object-state
 * failure stays.
 *
 * @param caught - What the call threw, or `undefined` when it did not throw.
 * @param kind - The class the refusal must be.
 * @param context - Names the case in each failure message.
 * @returns The refusal, narrowed to `Error`.
 */
export function expectArgumentRefusal(
  caught: unknown,
  kind: ArgumentRefusalClass = RangeError,
  context = 'refusal',
): Error {
  expect(caught, `${context}: expected a refusal, got no throw`).toBeDefined();
  expect(isSonareError(caught), `${context}: an argument refusal is not a coded SonareError`).toBe(
    false,
  );
  expect(caught, `${context}: expected ${kind.name}`).toBeInstanceOf(kind);
  return caught as Error;
}

/**
 * Runs `call` and asserts it throws an argument refusal of class `kind`.
 *
 * @param call - The call under test.
 * @param kind - The class the refusal must be; `RangeError` by default.
 * @returns The refusal, narrowed to `Error`.
 */
export function expectRefusalOf(
  call: () => unknown,
  kind: ArgumentRefusalClass = RangeError,
): Error {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  return expectArgumentRefusal(caught, kind);
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Pattern for an unknown-key refusal whose message starts with `head`
 * (`"<entry>: unknown key <path>"`); `also` is the nested path a misplaced key belongs under.
 */
export function unknownKeyText(head: string, also?: string): RegExp {
  const tail = also === undefined ? '' : `, or ${escapeRegExp(also)}`;
  return new RegExp(`${escapeRegExp(head)} \\(expected [^)]*${tail}\\)`);
}

/** Matcher for the TypeError carrying exactly that refusal, for `toThrow` / `rejects.toThrow`. */
export function unknownKeyError(head: string, also?: string) {
  const pattern = new RegExp(`^${unknownKeyText(head, also).source}$`);
  return {
    asymmetricMatch: (actual: unknown) =>
      actual instanceof TypeError && pattern.test(actual.message),
    toString: () => `TypeError matching ${pattern}`,
    toAsymmetricMatcher: () => `TypeError matching ${pattern}`,
  };
}

/** A literal message compares equal; a pattern from {@link unknownKeyText} compares by match. */
export function textMatcher(message: string | RegExp): unknown {
  return typeof message === 'string' ? message : expect.stringMatching(message);
}

/** Returns `value`, failing the test when it is absent; for a level the call must have measured. */
export function present<T>(value: T | undefined, what = 'value'): T {
  expect(value, `${what} should be present`).toBeDefined();
  return value as T;
}
