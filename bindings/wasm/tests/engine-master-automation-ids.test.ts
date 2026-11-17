/**
 * Master fader, pan and width automation ids on the WASM RealtimeEngine, and the
 * constants the worklet entry exports for the facade's master target.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import { SONARE_METER_RING_PROTOCOL_VERSION } from '../dist/worklet.js';
import {
  ENGINE_MIXER_PARAM_FADER_DB,
  ENGINE_MIXER_PARAM_PAN,
  engineMixerMasterTarget,
} from '../src/worklet/protocol.js';

describe('master automation ids (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('resolves the three master controls to engine parameters and refuses other names', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      const names = ['faderDb', 'pan', 'width'] as const;
      const ids = names.map((name) => engine.resolveStripAutomationId('master', name));
      expect(new Set(ids).size).toBe(3);
      ids.forEach((id, index) => {
        expect(engine.parameterInfo(id).name).toBe(names[index]);
      });
      expect(() => engine.resolveStripAutomationId('master', 'gain' as 'faderDb')).toThrow(
        RangeError,
      );
    } finally {
      engine.destroy();
    }
  });

  it('agrees with the id the worklet facade builds for the master fader and pan', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      expect(engine.resolveStripAutomationId('master', 'faderDb')).toBe(
        engineMixerMasterTarget(ENGINE_MIXER_PARAM_FADER_DB),
      );
      expect(engine.resolveStripAutomationId('master', 'pan')).toBe(
        engineMixerMasterTarget(ENGINE_MIXER_PARAM_PAN),
      );
      expect(SONARE_METER_RING_PROTOCOL_VERSION).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });
});
