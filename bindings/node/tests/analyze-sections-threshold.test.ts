import { describe, expect, it } from 'vitest';
import { analyzeSections } from '../src/index.js';

const SR = 22050;
const SPAN_SEC = 4;

/** Four 4 s blocks alternating a pure 220 Hz tone and a bright 311.13 Hz tone. */
function abab(): Float32Array {
  const n = SR * SPAN_SEC;
  const out = new Float32Array(n * 4);
  for (let i = 0; i < out.length; i++) {
    const bright = Math.floor(i / n) % 2 === 1;
    const freq = bright ? 311.13 : 220;
    const count = bright ? 6 : 1;
    let v = 0;
    for (let k = 1; k <= count; k++) {
      v += Math.sin((2 * Math.PI * freq * k * i) / SR) / k;
    }
    out[i] = (0.4 * v) / count;
  }
  return out;
}

const SIGNAL = abab();
const OPTIONS = { minSectionSec: 2 };

describe('analyzeSections threshold', () => {
  it('gives the same sections through the positional and request forms', () => {
    const positional = analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: 0.1 });
    const request = analyzeSections({
      samples: SIGNAL,
      sampleRate: SR,
      ...OPTIONS,
      threshold: 0.1,
    });
    expect(request).toEqual(positional);
  });

  it('reads an omitted threshold as the library default 0.22', () => {
    expect(analyzeSections(SIGNAL, SR, OPTIONS)).toEqual(
      analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: 0.22 }),
    );
  });

  it('changes the sections it reports', () => {
    const permissive = analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: 0 });
    const strict = analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: 5 });
    expect(strict).not.toEqual(permissive);
    expect(strict.sections.length).toBeLessThan(permissive.sections.length);
  });

  it('refuses a negative threshold as a RangeError from the library', () => {
    expect(() => analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: -0.1 })).toThrow(
      new RangeError('analyzeSections: options.threshold must be finite and at least 0'),
    );
    expect(() => analyzeSections(SIGNAL, SR, { ...OPTIONS, threshold: -0.1 })).toThrow(RangeError);
  });
});
