/**
 * WASM half of the StreamingRetune cross-surface contract.
 *
 * `setConfig(partial)` is a MERGE on Node and Python: the keys the caller did
 * not name keep their current value. WASM rebuilt the config from defaults
 * instead, so `setConfig({ semitones: 7 })` followed by `setConfig({ mix: 0.5 })`
 * silently reset the pitch shift to 0 with no error and no warning — the same
 * code kept 7 on Node.
 *
 * `grainSize` is the one key that must NOT be seeded from `config()`: once
 * prepared, `config()` reports the EFFECTIVE grain, so seeding from it would
 * freeze the `0` "derive from the sample rate" sentinel into a literal and a
 * re-`prepare()` at another rate would keep the first rate's grain.
 *
 * These run against the built module, so they FAIL against a `dist/` older than
 * the fix in `src/wasm/bindings/streaming/retune.cpp` — the symptom is
 * `semitones` reading back as 0 and `mix` as 1. Rebuild the WASM module before
 * doubting the assertions.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, StreamingRetune } from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;

describe('StreamingRetune setConfig merges', () => {
  beforeAll(async () => {
    await init();
  });

  it('keeps unmentioned keys when setConfig carries only one', () => {
    const retune = new StreamingRetune({ semitones: 5, mix: 0.25 });
    try {
      retune.prepare(SR, BLOCK);
      retune.setConfig({ mix: 0.75 });
      const config = retune.config();
      expect(config.semitones).toBeCloseTo(5, 5);
      expect(config.mix).toBeCloseTo(0.75, 5);
    } finally {
      retune.delete();
    }
  });

  it('keeps a pitch shift set by an earlier setConfig', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      retune.setConfig({ semitones: 7 });
      retune.setConfig({ mix: 0.5 });
      expect(retune.config().semitones).toBeCloseTo(7, 5);
      expect(retune.config().mix).toBeCloseTo(0.5, 5);
    } finally {
      retune.delete();
    }
  });

  it('reads an explicit undefined the same as an absent key', () => {
    const retune = new StreamingRetune({ semitones: 5, mix: 0.25, grainSize: 512 });
    try {
      retune.prepare(SR, BLOCK);
      const baseline = retune.config();
      retune.setConfig({ semitones: undefined, mix: undefined, grainSize: undefined });
      expect(retune.config()).toEqual(baseline);
      retune.setConfig({});
      expect(retune.config()).toEqual(baseline);
    } finally {
      retune.delete();
    }
  });

  it('re-derives the grain at a new sample rate after an unrelated setConfig', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.grainSize()).toBe(2232);
      // Touching any other control must not freeze the derived grain: the 0
      // sentinel is the last value the caller REQUESTED, and it is what the
      // next prepare resolves from.
      retune.setConfig({ semitones: 5 });
      retune.prepare(22050, BLOCK);
      expect(retune.grainSize()).toBe(1024);
    } finally {
      retune.delete();
    }
  });

  it('applies a grain requested after prepare on the next prepare', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      const derived = retune.grainSize();
      retune.setConfig({ grainSize: 512 });
      // Structural: the request takes effect at the next prepare, and until then
      // config() keeps reporting the grain actually in use.
      expect(retune.grainSize()).toBe(derived);
      expect(retune.config().grainSize).toBe(derived);
      retune.prepare(SR, BLOCK);
      expect(retune.grainSize()).toBe(512);
    } finally {
      retune.delete();
    }
  });
});
