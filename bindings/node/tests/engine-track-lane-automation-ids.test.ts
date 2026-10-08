/**
 * Track fader/pan automation id lifetimes on the Node RealtimeEngine. Mirrors
 * the C ABI case in tests/api/sonare_c_engine_strip_test.cpp.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

const kBlock = 256;
const kFrames = kBlock * 80;

function alive(engine: RealtimeEngine, id: number): boolean {
  try {
    engine.parameterInfo(id);
    return true;
  } catch {
    return false;
  }
}

function settled(engine: RealtimeEngine, blocks = 30): number {
  let last = 0;
  for (let block = 0; block < blocks; block += 1) {
    const [chunk] = engine.process([new Float32Array(kBlock)]);
    last = chunk[chunk.length - 1];
  }
  return last;
}

describe('track lane automation ids (Node)', () => {
  it('keeps a fader id on its track across a reorder and retires it with the track', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      // Only track 20 sounds, so the output reads track 20's fader alone.
      engine.setClips([
        {
          id: 1,
          trackId: 20,
          channels: [new Float32Array(kFrames).fill(1)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ]);
      engine.setTrackLanes([10, 20]);
      const fader10 = engine.resolveTrackLaneAutomationId(10, 'faderDb');
      const pan10 = engine.resolveTrackLaneAutomationId(10, 'pan');
      expect(fader10).toBeGreaterThan(0);
      expect(pan10).not.toBe(fader10);
      expect(engine.resolveTrackLaneAutomationId(30, 'faderDb')).toBe(-1);
      expect(engine.resolveTrackLaneAutomationId(10, 'width' as 'pan')).toBe(-1);

      engine.setTrackLanes([20, 10]);
      expect(engine.resolveTrackLaneAutomationId(10, 'faderDb')).toBe(fader10);
      engine.setParameter(fader10, -60);
      engine.play();
      expect(settled(engine)).toBeCloseTo(1, 3);

      engine.setTrackLanes([20]);
      expect(alive(engine, fader10)).toBe(false);
      engine.setParameter(fader10, -60);
      expect(settled(engine)).toBeCloseTo(1, 3);
      engine.setTrackLanes([20, 10]);
      expect(alive(engine, fader10)).toBe(false);
      expect(engine.resolveTrackLaneAutomationId(10, 'faderDb')).not.toBe(fader10);
    } finally {
      engine.destroy();
    }
  });
});
