/**
 * The documented "0 selects the library default" scalars on the Node surface.
 *
 * These parameters document 0 as the sentinel requesting the library default.
 * The convention only holds when a value that is not the sentinel is either
 * applied or refused -- a value quietly swapped for the default produces a
 * plausible result for a call the caller never made, which is the one outcome
 * that cannot be told apart from working.
 *
 * Each case asserts the outcome: the refusal, or the sample content. The zero
 * cases are the positive control -- 0 has to match omitting the parameter *and*
 * a non-sentinel value has to differ, or "0 selects the default" is
 * indistinguishable from "0 is applied as 0".
 */

import { describe, expect, it } from 'vitest';
import { mastering, meteringSpectrum } from '../src/index.js';

const SR = 44100;

/** Loud bursts over a quiet bed, so the true-peak limiter engages and the
 *  release time is observable in the output. */
function bursts(n: number): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const envelope = i % 4096 < 256 ? 1.0 : 0.02;
    out[i] = envelope * 0.95 * Math.sin((2 * Math.PI * 110 * i) / SR);
  }
  return out;
}

function maxAbsDiff(a: Float32Array, b: Float32Array): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

const NON_SENTINEL_REFUSED: [string, number][] = [
  ['a negative value', -5],
  ['NaN', Number.NaN],
  ['positive infinity', Number.POSITIVE_INFINITY],
  ['negative infinity', Number.NEGATIVE_INFINITY],
];

describe('mastering releaseMs', () => {
  const samples = bursts(SR);
  const master = (releaseMs?: number) =>
    mastering(
      releaseMs === undefined
        ? { samples, sampleRate: SR, targetLufs: -6 }
        : { samples, sampleRate: SR, targetLufs: -6, releaseMs },
    ).samples;

  it.each(NON_SENTINEL_REFUSED)('refuses %s instead of using the default', (_label, value) => {
    expect(() => master(value)).toThrow(/release_ms must be 0 .* or a finite positive value/);
  });

  it('treats 0 as the library default rather than a release of zero', () => {
    const omitted = master();
    expect(maxAbsDiff(master(0), omitted)).toBe(0);
    // 50 ms is the documented default the sentinel resolves to.
    expect(maxAbsDiff(master(50), omitted)).toBe(0);
  });

  it('applies a non-sentinel release time', () => {
    // Without this the equalities above would also hold for a release time the
    // limiter ignored.
    const omitted = master();
    expect(maxAbsDiff(master(5), omitted)).toBeGreaterThan(0.1);
    expect(maxAbsDiff(master(200), omitted)).toBeGreaterThan(0.1);
  });
});

describe('meteringSpectrum optional scalars', () => {
  const samples = bursts(8192);
  const spectrum = (options: Record<string, number> = {}) =>
    meteringSpectrum(samples, SR, options as never).db;

  it.each([
    'nFft',
    'octaveFraction',
    'dbRef',
    'dbAmin',
  ])('refuses a negative %s instead of using the default', (field) => {
    expect(() => spectrum({ [field]: -5 })).toThrow();
  });

  it.each(['nFft', 'octaveFraction'])('refuses a non-finite %s', (field) => {
    expect(() => spectrum({ [field]: Number.NaN })).toThrow();
    expect(() => spectrum({ [field]: Number.POSITIVE_INFINITY })).toThrow();
  });

  it.each([
    'nFft',
    'octaveFraction',
    'dbRef',
    'dbAmin',
  ])('treats 0 for %s as the library default', (field) => {
    expect(Array.from(spectrum({ [field]: 0 }))).toEqual(Array.from(spectrum()));
  });

  it('applies a non-sentinel dbRef and nFft', () => {
    // The controls for the equalities above: both fields really reach the core.
    expect(Array.from(spectrum({ dbRef: 2 }))).not.toEqual(Array.from(spectrum()));
    expect(spectrum({ nFft: 1024 }).length).not.toBe(spectrum().length);
  });
});
