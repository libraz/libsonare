/**
 * `detectBoundaries` refuses a wrong-typed option instead of coercing it.
 *
 * The readers these ten fields sit on are the type-checked family, so a value of
 * the wrong type is a caller error rather than something to convert. Driving the
 * embind module directly is the only route that reaches them: the TypeScript
 * facade answers a wrong type first, and a reader nothing can reach is a reader
 * nothing has measured.
 */

import { beforeAll, describe, expect, it } from 'vitest';

const SAMPLE_RATE = 22050;

type EmbindModule = {
  detectBoundaries: (
    samples: Float32Array,
    sampleRate: number,
    options: Record<string, unknown>,
  ) => { sampleRate: number; nFrames: number };
  pcen: (
    values: Float32Array,
    nBins: number,
    nFrames: number,
    options: Record<string, unknown>,
  ) => Float32Array;
};

let module: EmbindModule;

function tone(durationSec: number, freqHz = 220, amp = 0.5): Float32Array {
  const out = new Float32Array(Math.floor(SAMPLE_RATE * durationSec));
  for (let i = 0; i < out.length; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / SAMPLE_RATE);
  }
  return out;
}

/** Every field well-typed, so each case below moves exactly one of them. */
const VALID = {
  nFft: 2048,
  hopLength: 512,
  kernelSize: 64,
  threshold: 0.3,
  absoluteThreshold: 0.005,
  nMfcc: 13,
  nChroma: 12,
  peakDistance: 2.0,
  useMfcc: true,
  useChroma: true,
};

describe('detectBoundaries wrong-typed options', () => {
  beforeAll(async () => {
    const createModule = (await import('../dist/sonare.js')).default;
    module = (await createModule()) as unknown as EmbindModule;
  });

  it('accepts the same call when every option is well-typed', () => {
    // Non-vacuity: the refusals below are the type test firing, not the route
    // failing for some reason of its own.
    expect(module.detectBoundaries(tone(1), SAMPLE_RATE, VALID).sampleRate).toBe(SAMPLE_RATE);
  });

  it('refuses a numeric string where a number is required', () => {
    // Each of these is in range and spells a value the field would accept, so a
    // coercing reader answers with a plausible result rather than an error --
    // the caller's mistake becomes indistinguishable from a value they chose.
    const samples = tone(1);
    for (const key of [
      'nFft',
      'hopLength',
      'kernelSize',
      'nMfcc',
      'nChroma',
      'threshold',
      'absoluteThreshold',
      'peakDistance',
    ] as const) {
      expect(() =>
        module.detectBoundaries(samples, SAMPLE_RATE, { ...VALID, [key]: String(VALID[key]) }),
      ).toThrow();
    }
  });

  it('refuses a non-boolean for the two feature-stream flags', () => {
    const samples = tone(1);
    // `'false'` is the sharp one: JS truthiness reads the string as true, so a
    // coercing reader turns a request to disable the stream into enabling it.
    for (const bad of [{ useMfcc: 'true' }, { useMfcc: 'false' }, { useChroma: 'false' }]) {
      expect(() => module.detectBoundaries(samples, SAMPLE_RATE, { ...VALID, ...bad })).toThrow();
    }
    // A number is not a flag either, and 1/0 is the spelling that would survive
    // every downstream check.
    for (const bad of [{ useMfcc: 1 }, { useChroma: 0 }]) {
      expect(() => module.detectBoundaries(samples, SAMPLE_RATE, { ...VALID, ...bad })).toThrow();
    }
  });

  it('refuses the other shapes that coerce to a number', () => {
    const samples = tone(1);
    // A one-element array stringifies to the number it holds, and a boolean
    // reads as 0 or 1; both arrive in the field's own domain.
    for (const bad of [{ nFft: [1024] }, { threshold: true }, { nMfcc: [13] }]) {
      expect(() => module.detectBoundaries(samples, SAMPLE_RATE, { ...VALID, ...bad })).toThrow();
    }
  });

  it('leaves an untouched facade on the coercing readers', () => {
    // The control for the claim above: this change moved ten fields, not the
    // reader family's behaviour. `pcen` still reads its options through the
    // presence-checked readers, so the same numeric string is still accepted
    // there -- which is what `detectBoundaries` did before.
    const bins = 4;
    const frames = 8;
    const matrix = new Float32Array(bins * frames).fill(0.25);
    expect(module.pcen(matrix, bins, frames, { sampleRate: 22050 }).length).toBe(bins * frames);
    expect(module.pcen(matrix, bins, frames, { sampleRate: '22050' }).length).toBe(bins * frames);
    expect(module.pcen(matrix, bins, frames, { hopLength: '512' }).length).toBe(bins * frames);
  });
});
