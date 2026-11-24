import { beforeAll, describe, expect, it } from 'vitest';
import { chirp, clicks, init, tone } from '../src/index';

describe('synthetic audio generation', () => {
  beforeAll(async () => {
    await init();
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
});
