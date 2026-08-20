/**
 * The object-shaped mastering-chain reader describes the tape and exciter
 * stages field by field, so a field it does not read is silently dropped. These
 * cover the two that were missing, and — because this reader builds the config
 * directly rather than through the C ABI — that an antialiasing ordinal naming
 * no mode is rejected here instead of being cast into the config.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringChain } from '../src/index';

const sampleRate = 22_050;
const samples = Float32Array.from({ length: 4096 }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

// Largest sample-wise difference between two renders of the same input.
function maxDifference(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

describe('WASM mastering chain saturation fields', () => {
  it('reads the tape oversampling factor', () => {
    // A field that is declared but never read would render identically at every
    // factor, so this compares the audio rather than only the length. Measured
    // on this signal: 0.12 between factors 1 and 4, against a repeat-render
    // floor of exactly 0, so the threshold sits well clear of both ends.
    const render = (oversampleFactor: number) =>
      masteringChain(samples, sampleRate, {
        saturation: {
          tape: { enabled: true, driveDb: 12, saturation: 0.9, oversampleFactor },
        },
      }).samples;
    const plain = render(1);
    expect(plain).toHaveLength(samples.length);
    expect(maxDifference(plain, render(1))).toBe(0);
    for (const oversampleFactor of [2, 4]) {
      expect(maxDifference(plain, render(oversampleFactor))).toBeGreaterThan(1e-3);
    }
  });

  it('rejects a tape oversampling factor the processor does not implement', () => {
    expect(() =>
      masteringChain(samples, sampleRate, {
        saturation: { tape: { enabled: true, driveDb: 6, oversampleFactor: 3 } },
      }),
    ).toThrow();
  });

  it('reads the exciter antialiasing mode', () => {
    // 0 = none, 3 = 4x oversampling: the two modes the exciter implements.
    // Measured difference between them on this signal: 0.0028, against a
    // repeat-render floor of exactly 0.
    const render = (aliasing: number) =>
      masteringChain(samples, sampleRate, {
        saturation: { exciter: { enabled: true, amount: 1.0, driveDb: 12, aliasing } },
      }).samples;
    const none = render(0);
    expect(none).toHaveLength(samples.length);
    expect(maxDifference(none, render(0))).toBe(0);
    expect(maxDifference(none, render(3))).toBeGreaterThan(1e-4);
  });

  it('rejects an antialiasing ordinal that names no mode', () => {
    // The reader bypasses the C-ABI enum guard, so an ordinal outside the
    // declared set must be rejected here rather than cast into the config.
    for (const aliasing of [-1, 4, 99]) {
      expect(() =>
        masteringChain(samples, sampleRate, {
          saturation: { exciter: { enabled: true, amount: 0.5, aliasing } },
        }),
      ).toThrow();
    }
  });

  it('rejects the antialiasing modes the exciter does not implement', () => {
    // 1 and 2 name real modes (ADAA), so they pass the ordinal range check and
    // are refused by the processor's own validation instead.
    for (const aliasing of [1, 2]) {
      expect(() =>
        masteringChain(samples, sampleRate, {
          saturation: { exciter: { enabled: true, amount: 0.5, aliasing } },
        }),
      ).toThrow();
    }
  });
});
