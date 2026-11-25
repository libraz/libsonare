/**
 * WASM coverage for realtime spectrum + vectorscope (scope) telemetry: a steady
 * tone fed through the engine yields a master snapshot whose FFT peak sits in a
 * low band and whose goniometer carries scatter points.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { capabilities, ErrorCode, init, RealtimeEngine } from '../dist/index.js';

const SR = 48000;
const BLOCK = 256;
const TONE_HZ = 1000;

describe('scope telemetry (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports NotSupported when scope telemetry is not compiled in', (context) => {
    if (capabilities().features.mixing) {
      context.skip();
    }
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() => engine.configureScopeTelemetry(128, 32)).toThrow(
        expect.objectContaining({
          code: ErrorCode.NotSupported,
          reason: 'NotCompiledIn',
          message: expect.stringContaining('mixing support is not compiled in'),
        }),
      );
    } finally {
      engine.destroy();
    }
  });

  it('refuses out-of-domain configuration without changing the active tap', () => {
    const checkUnchanged = (invalid: (engine: RealtimeEngine) => void, message: string) => {
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      try {
        expect(engine.configureScopeTelemetry(BLOCK, 32)).toBe(32);
        expect(() => invalid(engine)).toThrow(new RangeError(message));
        engine.play();
        let phase = 0;
        for (let block = 0; block < 12; block++) {
          const left = new Float32Array(BLOCK);
          const right = new Float32Array(BLOCK);
          for (let i = 0; i < BLOCK; i++) {
            const sample = 0.5 * Math.sin((2 * Math.PI * TONE_HZ * phase) / SR);
            left[i] = sample;
            right[i] = sample;
            phase++;
          }
          engine.process([left, right]);
        }
        const master = engine.drainScopeTelemetry().find((record) => record.targetId === 0);
        expect(master?.bands.length).toBe(32);
      } finally {
        engine.destroy();
      }
    };

    checkUnchanged(
      (engine) => engine.configureScopeTelemetry(-1, 32),
      'RealtimeEngine.configureScopeTelemetry: intervalFrames must be an integer in [0, 2147483647]',
    );
    checkUnchanged(
      (engine) => engine.configureScopeTelemetry(BLOCK, 65),
      'RealtimeEngine.configureScopeTelemetry: bandCount must be an integer in [1, 64]',
    );
    const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
    try {
      expect(() => engine.configureScopeTelemetry(-1, 65)).toThrow(
        new RangeError(
          'RealtimeEngine.configureScopeTelemetry: intervalFrames must be an integer in [0, 2147483647]',
        ),
      );
      expect(() => engine.configureScopeTelemetry(0, -1)).toThrow(
        new RangeError(
          'RealtimeEngine.configureScopeTelemetry: bandCount must be an integer in [1, 64]',
        ),
      );
      expect(() => engine.configureScopeTelemetry(-1, 'bad' as unknown as number)).toThrow(
        new TypeError('RealtimeEngine.configureScopeTelemetry: bandCount must be an integer'),
      );
    } finally {
      engine.destroy();
    }
  });

  it('reports a tone spectrum and goniometer per target', () => {
    const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
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
