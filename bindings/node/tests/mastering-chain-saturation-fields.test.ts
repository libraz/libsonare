/**
 * The tape oversampling factor and the exciter antialiasing mode are declared
 * on the chain config type, so these pin that the declarations describe fields
 * the core actually reads: a valid value runs, a mode given as a number or an
 * unknown name is refused by path, and a mode the exciter does not implement is
 * refused by the processor.
 */

import { describe, expect, it } from 'vitest';
import { masterAudio } from '../src/index.js';

const sampleRate = 22_050;
const samples = Float32Array.from({ length: 4096 }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

// Largest sample-wise difference between two renders of the same input.
function maxDifference(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

describe('mastering chain saturation fields', () => {
  it('reads the tape oversampling factor', () => {
    // A field that is declared but never read would render identically at every
    // factor, so this compares the audio rather than only the length. Measured
    // on this signal: 0.12 between factors 1 and 4, against a repeat-render
    // floor of exactly 0, so the threshold sits well clear of both ends.
    const render = (oversampleFactor: number) =>
      masterAudio(samples, sampleRate, {
        overrides: {
          saturation: {
            tape: { enabled: true, driveDb: 12, saturation: 0.9, oversampleFactor },
          },
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
      masterAudio(samples, sampleRate, {
        overrides: { saturation: { tape: { enabled: true, driveDb: 6, oversampleFactor: 3 } } },
      }),
    ).toThrow(
      expect.objectContaining({
        name: 'RangeError',
        message: 'masterAudio: overrides.saturation.tape.oversampleFactor must be one of 1, 2, 4',
      }),
    );
  });

  it('reads the exciter antialiasing mode', () => {
    // none and oversample4x: the two modes the exciter implements.
    // Measured difference between them on this signal: 0.0028, against a
    // repeat-render floor of exactly 0.
    const render = (aliasing: 'none' | 'oversample4x') =>
      masterAudio(samples, sampleRate, {
        overrides: {
          saturation: { exciter: { enabled: true, amount: 1.0, driveDb: 12, aliasing } },
        },
      }).samples;
    const none = render('none');
    expect(none).toHaveLength(samples.length);
    expect(maxDifference(none, render('none'))).toBe(0);
    expect(maxDifference(none, render('oversample4x'))).toBeGreaterThan(1e-4);
  });

  it('refuses a mode given as a number or an unknown name, naming the path', () => {
    const tokens =
      'masterAudio: overrides.saturation.exciter.aliasing must be one of none, adaa1, adaa2, oversample4x';
    const cases: [unknown, ErrorConstructor][] = [
      [3, TypeError],
      ['bogus', RangeError],
    ];
    for (const [aliasing, cls] of cases) {
      expect(() =>
        masterAudio(samples, sampleRate, {
          overrides: { saturation: { exciter: { enabled: true, amount: 0.5, aliasing } } } as never,
        }),
      ).toThrow(
        expect.objectContaining({ name: cls.name, message: expect.stringContaining(tokens) }),
      );
    }
  });

  it('rejects the antialiasing modes the exciter does not implement', () => {
    for (const aliasing of ['adaa1', 'adaa2'] as never[]) {
      expect(() =>
        masterAudio(samples, sampleRate, {
          overrides: { saturation: { exciter: { enabled: true, amount: 0.5, aliasing } } },
        }),
      ).toThrow(/ADAA/);
    }
  });
});
