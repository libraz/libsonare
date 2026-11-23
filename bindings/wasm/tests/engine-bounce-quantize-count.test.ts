import { beforeAll, describe, expect, it } from 'vitest';
import type { EngineBounceOptions } from '../dist/index.js';
import { init, RealtimeEngine } from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = 256;

describe('bounceOffline quantizeClippedSamples', () => {
  beforeAll(async () => {
    await init();
  });

  /** A 2.0-amplitude tone sits past full scale on about two thirds of its samples. */
  const render = (overrides: Partial<EngineBounceOptions>) => {
    const channel = new Float32Array(FRAMES);
    for (let i = 0; i < FRAMES; i++) {
      channel[i] = 2.0 * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE);
    }
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      engine.setClips([
        { id: 1, channels: [channel, channel.slice()], startPpq: 0, lengthSamples: FRAMES },
      ]);
      engine.play();
      return engine.bounceOffline({
        totalFrames: FRAMES,
        blockSize: BLOCK_SIZE,
        numChannels: 2,
        ...overrides,
      });
    } finally {
      engine.destroy();
    }
  };

  it('is a non-negative integer count when the bounce dithered', () => {
    const result = render({ dither: 'tpdf', ditherBits: 16 });
    expect(Number.isInteger(result.quantizeClippedSamples)).toBe(true);
    expect(result.quantizeClippedSamples as number).toBeGreaterThan(0);
  });

  it('is absent when the bounce did not dither', () => {
    expect('quantizeClippedSamples' in render({})).toBe(false);
  });
});
