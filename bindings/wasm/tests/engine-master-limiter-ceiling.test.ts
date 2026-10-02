/**
 * Master true-peak ceiling through the real WASM RealtimeEngine.
 *
 * The render assertion exercises the real WASM RealtimeEngine command queue;
 * the resolver assertion makes a construction-only lookahead parameter fail
 * at the name boundary instead of being silently queued and ignored.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const TRACK_ID = 10;

const masterLimiterScene = JSON.stringify({
  version: 1,
  strips: [
    {
      id: 'master',
      inserts: [
        {
          slot: 'pre',
          processor: 'maximizer.truePeakLimiter',
          // lookaheadMs is intentionally present in construction JSON.  It is
          // a valid construction field, but is not realtime-safe by name.
          params: JSON.stringify({ ceilingDb: -1, lookaheadMs: 0, releaseMs: 50 }),
        },
      ],
    },
  ],
  buses: [],
  connections: [],
});

function peak(channels: Float32Array[]): number {
  let value = 0;
  for (const channel of channels) {
    for (const sample of channel) {
      value = Math.max(value, Math.abs(sample));
    }
  }
  return value;
}

function processPeak(engine: RealtimeEngine, blocks: number, skip = 0): number {
  let result = 0;
  for (let block = 0; block < blocks; block += 1) {
    const current = peak(
      engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]),
    );
    if (block >= skip) {
      result = Math.max(result, current);
    }
  }
  return result;
}

describe('master limiter ceiling (real WASM engine)', () => {
  beforeAll(async () => {
    await init();
  });

  it('changes the rendered peak through a resolved master insert id', () => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      const frames = BLOCK_SIZE * 32;
      engine.setTrackLanes([{ trackId: TRACK_ID }]);
      engine.setClips([
        {
          id: 1,
          trackId: TRACK_ID,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setMasterStripJson(masterLimiterScene);
      engine.play();

      // Let the initial lookahead pipeline fill before measuring the -1 dB
      // ceiling.  A constant full-scale clip keeps the limiter engaged.
      const initialPeak = processPeak(engine, 12);
      expect(initialPeak).toBeGreaterThan(0.7);
      expect(initialPeak).toBeLessThan(1.05);

      const ceilingId = engine.resolveMasterInsertAutomationId(0, 'ceilingDb');
      expect(ceilingId).toBeGreaterThan(0);
      engine.setParameter(ceilingId, -12);
      // A queued ceiling change must pass through the limiter's lookahead and
      // release state before measuring the new steady peak.
      const loweredPeak = processPeak(engine, 16, 6);

      expect(loweredPeak).toBeGreaterThan(0.1);
      expect(loweredPeak).toBeLessThan(initialPeak * 0.5);
    } finally {
      engine.destroy();
    }
  });

  it('rejects construction-only master parameters before queueing them', () => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      engine.setTrackLanes([{ trackId: TRACK_ID }]);
      engine.setMasterStripJson(masterLimiterScene);
      expect(engine.resolveMasterInsertAutomationId(0, 'lookaheadMs')).toBe(-1);
      expect(() => engine.setMasterStripInsertParamByName(0, 'lookaheadMs', 1)).toThrow();
    } finally {
      engine.destroy();
    }
  });
});
