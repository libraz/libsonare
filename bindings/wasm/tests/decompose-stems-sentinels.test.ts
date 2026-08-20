/**
 * Sentinel-value handling for `decomposeStems`.
 *
 * The C ABI documents 0 on `SonareDecomposeStemsConfig` as "use the built-in
 * default", which is what makes a zero-initialised config mean the documented
 * defaults. Every surface exposing the same option names has to land on the
 * same effective value for the same literal input, or reject it the same way;
 * silently computing a numerically different answer is the one outcome that
 * cannot be told apart from working.
 *
 * The comparisons are within this surface on purpose. Cross-surface float
 * equality does not hold for every beta -- the multiplicative update runs
 * exponents of `beta - 2` and `beta - 1`, so an ill-conditioned beta diverges
 * in the last digits between native and WebAssembly builds. What must match
 * across surfaces is which input maps to which effective value, and that is
 * what each case asserts.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { decomposeStems, init } from '../dist/index.js';

const SAMPLE_RATE = 22050;
const FRAMES = 8192;

describe('decomposeStems sentinel values', () => {
  beforeAll(async () => {
    await init();
  });

  const samples = new Float32Array(FRAMES);
  for (let i = 0; i < FRAMES; i++) {
    samples[i] =
      0.5 * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE) +
      0.3 * Math.sin((2 * Math.PI * 660 * i) / SAMPLE_RATE);
  }

  const run = (options: Record<string, number>) =>
    decomposeStems({ samples, sampleRate: SAMPLE_RATE, ...options });

  const fingerprint = (options: Record<string, number>): string => {
    const result = run(options);
    return JSON.stringify([
      result.components.length,
      result.components[0]?.length ?? 0,
      result.w.length,
      result.h.length,
      Array.from(result.components[0]?.slice(0, 8) ?? []),
    ]);
  };

  // Each field paired with the default the C ABI documents for it.
  const sentinelDefaults: Array<[string, number]> = [
    ['nComponents', 4],
    ['nFft', 2048],
    ['hopLength', 512],
    ['nIter', 100],
    ['beta', 2],
    ['maskPower', 1],
  ];

  it('treats an explicit zero as the documented default', () => {
    for (const [field, documentedDefault] of sentinelDefaults) {
      expect(fingerprint({ [field]: 0 })).toBe(fingerprint({ [field]: documentedDefault }));
    }
  });

  it('still applies a real value in every sentinel field', () => {
    // Without this the previous case would also pass on a surface that ignored
    // the field outright.
    expect(fingerprint({ nComponents: 3 })).not.toBe(fingerprint({ nComponents: 0 }));
    expect(fingerprint({ nFft: 1024 })).not.toBe(fingerprint({ nFft: 0 }));
    expect(fingerprint({ beta: 0.5 })).not.toBe(fingerprint({ beta: 0 }));
    expect(fingerprint({ maskPower: 2 })).not.toBe(fingerprint({ maskPower: 0 }));
  });

  it('rejects a negative value rather than folding it into the default', () => {
    for (const field of ['nComponents', 'nFft', 'hopLength', 'nIter', 'maskPower']) {
      expect(() => run({ [field]: -1 })).toThrow();
    }
  });

  it('rejects a sub-unit mask power and non-finite values', () => {
    expect(() => run({ maskPower: 0.5 })).toThrow();
    expect(() => run({ maskPower: Number.NaN })).toThrow();
    expect(() => run({ beta: Number.NaN })).toThrow();
  });
});
