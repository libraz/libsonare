import { describe, expect, it } from 'vitest';
import { chirp, clicks, tone } from '../src/index.js';

describe('synthetic audio generation', () => {
  it('generates tone, chirp, and clicks', () => {
    expect(tone(440, 22050, 0.01)).toHaveLength(220);
    expect(chirp(200, 800, 22050, 0.01)).toHaveLength(220);
    expect(clicks(new Float32Array([0]), 22050, 32, 1000, 0.01)).toHaveLength(32);
  });
});
