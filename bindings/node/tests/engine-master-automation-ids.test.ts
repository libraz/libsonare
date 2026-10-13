/**
 * Master fader, pan and width automation ids on the Node RealtimeEngine, and the
 * ABI constants a host compares the loaded addon against.
 */

import { describe, expect, it } from 'vitest';
import { EXPECTED_ABI_VERSION, EXPECTED_ENGINE_ABI_VERSION, RealtimeEngine } from '../src/index.js';
import { addon } from '../src/native.js';

describe('master automation ids (Node)', () => {
  it('resolves the three master controls to engine parameters and refuses other names', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      const ids = (['faderDb', 'pan', 'width'] as const).map((name) =>
        engine.resolveMasterAutomationId(name),
      );
      expect(new Set(ids).size).toBe(3);
      ids.forEach((id, index) => {
        expect(engine.parameterInfo(id).name).toBe(['faderDb', 'pan', 'width'][index]);
        expect(() => engine.setParameterSmoothed(id, index === 0 ? -6 : 0.5)).not.toThrow();
      });
      expect(engine.resolveMasterAutomationId('gain' as 'faderDb')).toBe(-1);
    } finally {
      engine.destroy();
    }
  });
});

describe('exported ABI constants (Node)', () => {
  it('match the loaded addon', () => {
    expect(EXPECTED_ABI_VERSION).toBe(addon.abiVersion());
    expect(EXPECTED_ENGINE_ABI_VERSION).toBe(addon.engineAbiVersion());
  });
});
