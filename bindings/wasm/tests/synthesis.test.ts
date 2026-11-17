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
});
