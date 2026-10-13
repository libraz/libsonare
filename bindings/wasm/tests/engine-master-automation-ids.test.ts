/**
 * Master fader, pan and width automation ids on the WASM RealtimeEngine, and the
 * constants the worklet entry exports for the facade's master target.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import {
  ENGINE_MIXER_PARAM_FADER_DB,
  ENGINE_MIXER_PARAM_PAN,
  engineMixerMasterTarget,
  SONARE_METER_RING_PROTOCOL_VERSION,
} from '../dist/worklet.js';

describe('master automation ids (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('resolves the three master controls to engine parameters and refuses other names', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      const names = ['faderDb', 'pan', 'width'] as const;
      const ids = names.map((name) => engine.resolveMasterAutomationId(name));
      expect(new Set(ids).size).toBe(3);
      ids.forEach((id, index) => {
        expect(engine.parameterInfo(id).name).toBe(names[index]);
      });
      expect(engine.resolveMasterAutomationId('gain' as 'faderDb')).toBe(-1);
    } finally {
      engine.destroy();
    }
  });

  it('agrees with the id the worklet facade builds for the master fader and pan', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(engine.resolveMasterAutomationId('faderDb')).toBe(
        engineMixerMasterTarget(ENGINE_MIXER_PARAM_FADER_DB),
      );
      expect(engine.resolveMasterAutomationId('pan')).toBe(
        engineMixerMasterTarget(ENGINE_MIXER_PARAM_PAN),
      );
      expect(SONARE_METER_RING_PROTOCOL_VERSION).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });
});
