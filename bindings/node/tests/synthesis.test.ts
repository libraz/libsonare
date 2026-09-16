import { describe, expect, it } from 'vitest';
import { chirp, clicks, tone } from '../src/index.js';

describe('synthetic audio generation', () => {
  it('generates tone, chirp, and clicks', () => {
    expect(tone(440, 22050, 0.01)).toHaveLength(220);
    expect(chirp(200, 800, 22050, 0.01)).toHaveLength(220);
    expect(clicks(new Float32Array([0]), 22050, 32, 1000, 0.01)).toHaveLength(32);
  });

  /**
   * These three carry no sample-rate domain, because the core imposes none:
   * a generator renders correctly at 100 Hz and at 500 kHz alike. What it does
   * not survive is a fraction, which the addon narrowing truncates onto a rate
   * the core then accepts -- 44100.7 rendered at 44100 with nothing anywhere
   * reporting the substitution.
   */
  describe('a generator refuses a fractional rate without gaining a range', () => {
    const calls: Array<[string, (sampleRate: number) => Float32Array]> = [
      ['tone', (sampleRate) => tone(440, sampleRate, 0.01)],
      ['chirp', (sampleRate) => chirp(200, 800, sampleRate, 0.01)],
      // A longer click than the case above: 0.01 s rounds to nothing at 100 Hz,
      // and the core refuses that on its own geometry rather than on the rate.
      ['clicks', (sampleRate) => clicks(new Float32Array([0]), sampleRate, 32, 1000, 0.1)],
    ];

    for (const [name, call] of calls) {
      it(`${name} refuses a fraction that truncates onto a legal rate`, () => {
        expect(() => call(44100.7)).toThrow(/sampleRate must be an integer/);
      });

      it(`${name} still accepts a rate outside the analysis range`, () => {
        // Both ends, so a range quietly added later fails here rather than in a
        // caller: 100 is below the analysis floor and 500000 above its ceiling.
        expect(() => call(100)).not.toThrow();
        expect(() => call(500000)).not.toThrow();
      });
    }
  });
});
