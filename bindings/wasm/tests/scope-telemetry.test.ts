/**
 * WASM coverage for realtime spectrum + vectorscope (scope) telemetry: a steady
 * tone fed through the engine yields a master snapshot whose FFT peak sits in a
 * low band and whose goniometer carries scatter points.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SR = 48000;
const BLOCK = 256;
const TONE_HZ = 1000;

describe('scope telemetry (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports a tone spectrum and goniometer per target', () => {
    const engine = new RealtimeEngine(SR, BLOCK);
    try {
      expect(engine.configureScopeTelemetry(BLOCK, 32)).toBe(32);
      engine.play();

      let phase = 0;
      for (let block = 0; block < 12; block++) {
        const left = new Float32Array(BLOCK);
        const right = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          const s = 0.5 * Math.sin((2 * Math.PI * TONE_HZ * phase) / SR);
          left[i] = s;
          right[i] = s;
          phase++;
        }
        engine.process([left, right]);
      }

      const records = engine.drainScopeTelemetry();
      expect(records.length).toBeGreaterThan(0);

      const master = records.find((r) => r.targetId === 0);
      expect(master).toBeDefined();
      if (master) {
        expect(master.bands.length).toBe(32);
        let peak = 0;
        for (let b = 1; b < master.bands.length; b++) {
          if (master.bands[b] > master.bands[peak]) {
            peak = b;
          }
        }
        // 1 kHz over a 32-band [0, 24 kHz] split -> band 0/1.
        expect(peak).toBeLessThanOrEqual(2);
        expect(master.bands[peak]).toBeGreaterThan(master.bands[24] + 20);
        expect(master.points.length).toBeGreaterThan(0);
      }

      // Disabling capture stops further snapshots once the queue drains.
      expect(engine.configureScopeTelemetry(0, 32)).toBe(32);
    } finally {
      engine.destroy();
    }
  });
});
