/**
 * Bus fader automation id lifetimes on the Node RealtimeEngine. Mirrors the C
 * ABI case in tests/api/sonare_c_engine_strip_test.cpp.
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

describe('bus automation ids (Node)', () => {
  it('keeps a fader id on its bus across a reorder and retires it with the bus', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      // Only track 20 sounds and it feeds bus 200, so the output reads bus 200's fader alone.
      engine.setClips([
        {
          id: 1,
          trackId: 20,
          channels: [new Float32Array(kFrames).fill(1)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ]);
      engine.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
      engine.setTrackLanes([{ trackId: 20, outputBusId: 200 }]);
      const fader100 = engine.resolveBusAutomationId(100, 'faderDb');
      const fader200 = engine.resolveBusAutomationId(200, 'faderDb');
      expect(fader100).toBeGreaterThan(0);
      expect(fader200).not.toBe(fader100);
      expect(engine.resolveBusAutomationId(300, 'faderDb')).toBe(-1);
      expect(engine.resolveBusAutomationId(100, 'pan' as 'faderDb')).toBe(-1);

      engine.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
      expect(engine.resolveBusAutomationId(200, 'faderDb')).toBe(fader200);
      engine.setParameter(fader200, -60);
      engine.play();
      expect(settled(engine)).toBeCloseTo(0.001, 4);

      engine.setParameter(fader200, 0);
      engine.setTrackBuses([{ busId: 200 }]);
      expect(alive(engine, fader100)).toBe(false);
      engine.setParameter(fader100, -60);
      expect(settled(engine)).toBeCloseTo(1, 3);
      engine.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
      expect(alive(engine, fader100)).toBe(false);
      expect(engine.resolveBusAutomationId(100, 'faderDb')).not.toBe(fader100);
    } finally {
      engine.destroy();
    }
  });
});
