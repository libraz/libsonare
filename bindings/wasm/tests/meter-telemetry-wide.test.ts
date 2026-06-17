/**
 * WASM coverage for per-plane (wide) meter telemetry: a lane routed into a 5.1
 * group bus and panned hard to Ls yields a wide bus meter whose surround-left
 * plane carries the energy, while the stereo fast-path drain is unaffected.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SR = 48000;
const BLOCK = 256;

describe('wide meter telemetry (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('drains per-plane meters for a surround group bus', () => {
    const engine = new RealtimeEngine(SR, BLOCK);
    try {
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(BLOCK).fill(0.5)],
          startPpq: 0,
          lengthSamples: BLOCK,
        },
      ]);
      // 5.1 group bus (channelLayout 2); the lane routes into it, panned to Ls.
      engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      engine.setTrackStripJson(
        10,
        '{"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"s","surroundPan":{"azimuth":-110}}]}',
      );
      engine.play();
      engine.process([
        new Float32Array(BLOCK),
        new Float32Array(BLOCK),
        new Float32Array(BLOCK),
        new Float32Array(BLOCK),
        new Float32Array(BLOCK),
        new Float32Array(BLOCK),
      ]);

      const wide = engine.drainMeterTelemetryWide();
      const busMeter = wide.find((record) => record.targetId === 33);
      expect(busMeter).toBeDefined();
      if (!busMeter) {
        throw new Error('expected a 5.1 bus meter');
      }
      expect(busMeter.channelCount).toBe(6);
      expect(busMeter.peakDb).toHaveLength(6);
      // Ls (plane 4) carries the panned lane, above the silent front-left plane.
      expect(busMeter.peakDb[4]).toBeGreaterThan(busMeter.peakDb[0] + 10);
    } finally {
      engine.destroy();
    }
  });
});
