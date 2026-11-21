/**
 * Master fader, pan and width automation ids on the Node RealtimeEngine, and the
 * internal ABI mirrors the package checks the loaded addon against.
 */

import { describe, expect, it } from 'vitest';
import { EXPECTED_ABI_VERSION } from '../src/abi_version.js';
import { RealtimeEngine } from '../src/index.js';
import { addon } from '../src/native.js';
import { EXPECTED_ENGINE_ABI_VERSION } from '../src/realtime_engine.js';

describe('master automation ids (Node)', () => {
  it('resolves the three master controls to engine parameters and refuses other names', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      const ids = (['faderDb', 'pan', 'width'] as const).map((name) =>
        engine.resolveStripAutomationId('master', name),
      );
      expect(new Set(ids).size).toBe(3);
      ids.forEach((id, index) => {
        expect(engine.parameterInfo(id).name).toBe(['faderDb', 'pan', 'width'][index]);
        expect(() =>
          engine.setParameter(id, index === 0 ? -6 : 0.5, undefined, { smoothed: true }),
        ).not.toThrow();
      });
      expect(() => engine.resolveStripAutomationId('master', 'gain' as 'faderDb')).toThrow(
        RangeError,
      );
    } finally {
      engine.destroy();
    }
  });
});

describe('internal ABI mirrors (Node)', () => {
  it('match the loaded addon', () => {
    expect(EXPECTED_ABI_VERSION).toBe(addon.abiVersion());
    expect(EXPECTED_ENGINE_ABI_VERSION).toBe(addon.engineAbiVersion());
  });
});
