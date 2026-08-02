/**
 * Shared boundary-check fixtures for the WASM boundary suite. Both the Vitest
 * regression test (`boundary-regressions.test.ts`) and the headless-browser
 * smoke (`scripts/boundary-smoke.mjs`) import this single module — the
 * browser harness serves it verbatim over HTTP — so the invalid-input vectors and
 * signal helpers cannot drift between the two runners.
 *
 * Plain ESM (no TypeScript) so it loads unchanged in the browser, under `node`
 * for the smoke script, and through the Vitest TypeScript pipeline.
 */

export const SR = 48000;
export const BLOCK = 128;
export const WASM_FLOAT_BUDGET = 64 * 1024 * 1024;

/**
 * Frame counts every `drainTailStereo` boundary must reject: zero, above the
 * block size, negative, fractional, non-finite, and past the 32-bit range.
 * @type {number[]}
 */
export const INVALID_DRAIN_COUNTS = [
  0,
  BLOCK + 1,
  -1,
  0.5,
  Number.NaN,
  Number.POSITIVE_INFINITY,
  2 ** 32 + 1,
];

/**
 * Inverse (mel -> STFT) matrix shapes that must be rejected as an overflow
 * before any allocation, expressed as `[rows, frames]` pairs.
 * @type {Array<[number, number]>}
 */
export const INVERSE_OVERFLOW_SHAPES = [
  [65536, 65536],
  [65537, 65537],
  [2147483647, 2],
];

/**
 * A 1 kHz sine of `BLOCK` frames at amplitude `amplitude`.
 * @param {number} amplitude
 * @returns {Float32Array}
 */
export function sine(amplitude) {
  const out = new Float32Array(BLOCK);
  for (let i = 0; i < out.length; i++) {
    out[i] = amplitude * Math.sin((2 * Math.PI * 1000 * i) / SR);
  }
  return out;
}

/**
 * Root-mean-square level of a sample buffer.
 * @param {ArrayLike<number>} samples
 * @returns {number}
 */
export function rms(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / samples.length);
}

/**
 * Configure band 0 of an equalizer as a dynamic, externally-side-chained peak,
 * matching the shape both boundary runners exercise.
 * @param {{ setBand: (index: number, band: object) => void }} eq
 */
export function configureDynamicEq(eq) {
  eq.setBand(0, {
    type: 'Peak',
    frequencyHz: 1000,
    gainDb: 0,
    q: 2,
    enabled: true,
    dynamic: true,
    externalSidechain: true,
    thresholdDb: -32,
    ratio: 4,
    rangeDb: -12,
    attackMs: 0,
    releaseMs: 20,
  });
}
