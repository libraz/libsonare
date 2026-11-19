/**
 * WASM half of the StreamingRetune cross-surface contract.
 *
 * `setConfig(partial)` is a MERGE on Node and Python: the keys the caller did
 * not name keep their current value. WASM rebuilt the config from defaults
 * instead, so `setConfig({ pitchSemitones: 7 })` followed by `setConfig({ mix: 0.5 })`
 * silently reset the pitch shift to 0 with no error and no warning — the same
 * code kept 7 on Node.
 *
 * `grainSizeSamples` must NOT be seeded from the grain in use: once prepared that
 * is a literal, so seeding from it would freeze an automatic grain and a
 * re-`prepare()` at another rate would keep the first rate's grain.
 *
 * These run against the built module, so they FAIL against a `dist/` older than
 * the fix in `src/wasm/bindings/streaming/retune.cpp` — the symptom is
 * `pitchSemitones` reading back as 0 and `mix` as 1. Rebuild the WASM module before
 * doubting the assertions.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, StreamingRetune, type StreamingRetuneConfig } from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;

describe('StreamingRetune setConfig merges', () => {
  beforeAll(async () => {
    await init();
  });

  it('keeps unmentioned keys when setConfig carries only one', () => {
    const retune = new StreamingRetune({ pitchSemitones: 5, mix: 0.25 });
    try {
      retune.prepare(SR, BLOCK);
      retune.setConfig({ mix: 0.75 });
      const config = retune.config();
      expect(config.pitchSemitones).toBeCloseTo(5, 5);
      expect(config.mix).toBeCloseTo(0.75, 5);
    } finally {
      retune.destroy();
    }
  });

  it('keeps a pitch shift set by an earlier setConfig', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      retune.setConfig({ pitchSemitones: 7 });
      retune.setConfig({ mix: 0.5 });
      expect(retune.config().pitchSemitones).toBeCloseTo(7, 5);
      expect(retune.config().mix).toBeCloseTo(0.5, 5);
    } finally {
      retune.destroy();
    }
  });

  it('reads an explicit undefined the same as an absent key', () => {
    const retune = new StreamingRetune({ pitchSemitones: 5, mix: 0.25, grainSizeSamples: 512 });
    try {
      retune.prepare(SR, BLOCK);
      const baseline = retune.config();
      retune.setConfig({ pitchSemitones: undefined, mix: undefined, grainSizeSamples: undefined });
      expect(retune.config()).toEqual(baseline);
      retune.setConfig({});
      expect(retune.config()).toEqual(baseline);
    } finally {
      retune.destroy();
    }
  });

  it('re-derives the grain at a new sample rate after an unrelated setConfig', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.grainSizeSamples).toBe(2232);
      // Touching any other control must not freeze the derived grain: the 0
      // sentinel is the last value the caller REQUESTED, and it is what the
      // next prepare resolves from.
      retune.setConfig({ pitchSemitones: 5 });
      retune.prepare(22050, BLOCK);
      expect(retune.grainSizeSamples).toBe(1024);
    } finally {
      retune.destroy();
    }
  });

  it('applies a grain requested after prepare on the next prepare', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      const derived = retune.grainSizeSamples;
      retune.setConfig({ grainSizeSamples: 512 });
      // Structural: the grain in use moves at the next prepare, while config()
      // reports what is configured from the moment it is set.
      expect(retune.grainSizeSamples).toBe(derived);
      expect(retune.config().grainSizeSamples).toBe(512);
      retune.prepare(SR, BLOCK);
      expect(retune.grainSizeSamples).toBe(512);
    } finally {
      retune.destroy();
    }
  });
});

describe('StreamingRetune grain', () => {
  beforeAll(async () => {
    await init();
  });

  const withRetune = (config: StreamingRetuneConfig, body: (retune: StreamingRetune) => void) => {
    const retune = new StreamingRetune(config);
    try {
      body(retune);
    } finally {
      retune.destroy();
    }
  };

  it('reads an automatic grain back as absent before and after prepare', () => {
    withRetune({}, (retune) => {
      expect('grainSizeSamples' in retune.config()).toBe(false);
      retune.prepare(SR, BLOCK);
      expect('grainSizeSamples' in retune.config()).toBe(false);
      expect(retune.grainSizeSamples).toBe(2232);
    });
  });

  it('keeps a configured grain across a sample-rate change', () => {
    withRetune({ grainSizeSamples: 2232 }, (retune) => {
      retune.prepare(SR, BLOCK);
      retune.prepare(22050, BLOCK);
      expect(retune.config().grainSizeSamples).toBe(2232);
      expect(retune.grainSizeSamples).toBe(2232);
    });
  });

  it('refuses a grain of 0 with the automatic-grain hint', () => {
    expect(() => new StreamingRetune({ grainSizeSamples: 0 })).toThrow(
      new RangeError(
        'StreamingRetune: grainSizeSamples must not be 0; omit it for the automatic grain',
      ),
    );
    withRetune({}, (retune) => {
      expect(() => retune.setConfig({ grainSizeSamples: 0 })).toThrow(
        new RangeError(
          'StreamingRetune.setConfig: grainSizeSamples must not be 0; omit it for the automatic grain',
        ),
      );
    });
  });

  it('refuses a grain that is not a multiple of 4 in [4, 8192]', () => {
    for (const grain of [1, 3, 6, 1.5, -4, 8196]) {
      expect(() => new StreamingRetune({ grainSizeSamples: grain }), String(grain)).toThrow(
        new RangeError('StreamingRetune: grainSizeSamples must be a multiple of 4 in [4, 8192]'),
      );
    }
    withRetune({ grainSizeSamples: 512 }, (retune) => {
      expect(() => retune.setConfig({ grainSizeSamples: 8196 })).toThrow(
        new RangeError(
          'StreamingRetune.setConfig: grainSizeSamples must be a multiple of 4 in [4, 8192]',
        ),
      );
      expect(() => retune.setConfig({ grainSizeSamples: '512' as never })).toThrow(
        new TypeError('StreamingRetune.setConfig: grainSizeSamples must be a number'),
      );
      // A refused grain leaves the configured one in place.
      expect(retune.config().grainSizeSamples).toBe(512);
    });
    for (const grain of [4, 8192]) {
      withRetune({ grainSizeSamples: grain }, (retune) => {
        retune.prepare(SR, BLOCK);
        expect(retune.grainSizeSamples).toBe(grain);
      });
    }
  });

  it('round-trips config() through setConfig without moving the grain', () => {
    withRetune({}, (retune) => {
      retune.prepare(SR, BLOCK);
      retune.setConfig(retune.config());
      retune.prepare(22050, BLOCK);
      expect(retune.grainSizeSamples).toBe(1024);
      expect('grainSizeSamples' in retune.config()).toBe(false);
    });
    withRetune({ grainSizeSamples: 2232 }, (retune) => {
      retune.prepare(SR, BLOCK);
      retune.setConfig(retune.config());
      retune.prepare(22050, BLOCK);
      expect(retune.grainSizeSamples).toBe(2232);
      expect(retune.config().grainSizeSamples).toBe(2232);
    });
  });
});
