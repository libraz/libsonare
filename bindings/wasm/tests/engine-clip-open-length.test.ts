/**
 * A clip with no `lengthSamples` plays to the end of its source from
 * `clipOffsetSamples`, exactly as the same clip with that length spelled out.
 * The fade-out spans the clip's full length, so a clip read as longer than its
 * remaining source would fade over silence and leave the audible part flat.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SOURCE_FRAMES = 128;
const OFFSET = 48;

function bounceClip(lengthSamples?: number): Float32Array {
  const source = new Float32Array(SOURCE_FRAMES).fill(0.5);
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    engine.setClips([
      {
        id: 1,
        channels: [source, source.slice()],
        startPpq: 0,
        clipOffsetSamples: OFFSET,
        fadeOutSamples: SOURCE_FRAMES - OFFSET,
        ...(lengthSamples === undefined ? {} : { lengthSamples }),
      },
    ]);
    engine.play();
    const result = engine.bounceOffline({
      totalFrames: SOURCE_FRAMES,
      blockSize: 128,
      numChannels: 2,
      sourceSampleRate: 48000,
      targetSampleRate: 48000,
    });
    return result.channels[0];
  } finally {
    engine.destroy();
  }
}

describe('RealtimeEngine clip length', () => {
  beforeAll(async () => {
    await init();
  });

  it('reads an omitted lengthSamples as the source remaining after the offset', () => {
    const explicit = bounceClip(SOURCE_FRAMES - OFFSET);
    const omitted = bounceClip();
    // The explicit render fades: its first frame is louder than its last audible one.
    expect(explicit[0]).toBeGreaterThan(explicit[SOURCE_FRAMES - OFFSET - 2] + 0.1);
    expect(omitted.length).toBe(explicit.length);
    for (let i = 0; i < explicit.length; i++) {
      expect(omitted[i], `frame ${i}`).toBeCloseTo(explicit[i], 6);
    }
  });
});
