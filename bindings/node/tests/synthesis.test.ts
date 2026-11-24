import { describe, expect, it } from 'vitest';
import { chirp, clicks, tone } from '../src/index.js';

describe('synthetic audio generation', () => {
  it('uses the documented defaults when tone and chirp requests are omitted', () => {
    expect(tone()).toHaveLength(22050);
    expect(tone(undefined)).toEqual(tone({}));
    expect(chirp()).toHaveLength(22050);
    expect(chirp(undefined)).toEqual(chirp({ linear: false }));
  });

  it('generates tone, chirp, and clicks', () => {
    expect(tone({ frequencyHz: 440, sampleRate: 22050, durationSec: 0.01 })).toHaveLength(220);
    expect(chirp({ fminHz: 200, fmaxHz: 800, sampleRate: 22050, durationSec: 0.01 })).toHaveLength(
      220,
    );
    expect(
      clicks({
        times: new Float32Array([0]),
        sampleRate: 22050,
        length: 32,
        frequencyHz: 1000,
        clickDurationSec: 0.01,
      }),
    ).toHaveLength(32);
  });

  it.each([
    [{ fminHz: -1, linear: true }, 'fminHz must be finite and at least 0'],
    [{ fmaxHz: 0, linear: true }, 'fmaxHz must be finite and greater than 0'],
    [{ fminHz: 0 }, 'fminHz must be finite and greater than 0'],
    [{ fmaxHz: 0 }, 'fmaxHz must be finite and greater than 0'],
  ])('chirp refuses %o outside the sweep domain', (fields, text) => {
    expect(() => chirp({ sampleRate: 22050, durationSec: 0.01, ...fields })).toThrow(
      new RangeError(`chirp: ${text}`),
    );
  });

  /**
   * These three carry no sample-rate domain, because the core imposes none:
   * a generator renders correctly at 100 Hz and at 500 kHz alike. What it does
   * not survive is a fraction, which the addon narrowing truncates onto a rate
   * the core then accepts -- 44100.7 rendered at 44100 with nothing anywhere
   * reporting the substitution.
   */
  describe('a generator reads its rate in the audio sample-rate domain', () => {
    const calls: Array<[string, (sampleRate: number) => Float32Array]> = [
      ['tone', (sampleRate) => tone({ frequencyHz: 440, sampleRate, durationSec: 0.01 })],
      ['chirp', (sampleRate) => chirp({ fminHz: 200, fmaxHz: 800, sampleRate, durationSec: 0.01 })],
      // A longer click than the case above: 0.01 s rounds to nothing at 100 Hz,
      // and the core refuses that on its own geometry rather than on the rate.
      [
        'clicks',
        (sampleRate) =>
          clicks({
            times: new Float32Array([0]),
            sampleRate,
            length: 32,
            frequencyHz: 1000,
            clickDurationSec: 0.1,
          }),
      ],
    ];

    for (const [name, call] of calls) {
      it(`${name} refuses a fraction that truncates onto a legal rate`, () => {
        expect(() => call(44100.7)).toThrow(/sampleRate must be an integer/);
      });

      it(`${name} takes the audio sample-rate domain`, () => {
        expect(() => call(8000)).not.toThrow();
        expect(() => call(384000)).not.toThrow();
        expect(() => call(7999)).toThrow(/sampleRate must be an integer in \[8000, 384000\]$/);
        expect(() => call(384001)).toThrow(RangeError);
      });
    }
  });
});
