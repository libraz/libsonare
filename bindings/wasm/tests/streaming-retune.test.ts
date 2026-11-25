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
import {
  ErrorCode,
  init,
  isSonareError,
  StreamingRetune,
  type StreamingRetuneConfig,
} from '../dist/index.js';
import { expectRefusalOf, unknownKeyError } from './_helpers';

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
    // The pitch and mix domains are refused bounds, named by the entry that took them.
    expect(() => new StreamingRetune({ pitchSemitones: 25 })).toThrow(
      new RangeError('StreamingRetune: pitchSemitones must be finite and in [-24, 24]'),
    );
    withRetune({}, (retune) => {
      expect(() => retune.setConfig({ mix: 1.5 })).toThrow(
        new RangeError('StreamingRetune.setConfig: mix must be finite and in [0, 1]'),
      );
    });
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

  it('refuses an unknown config key by name on the constructor and setConfig', () => {
    expect(() => new StreamingRetune({ pitch: 3 } as never)).toThrow(
      unknownKeyError('StreamingRetune: unknown key pitch'),
    );
    const retune = new StreamingRetune({ pitchSemitones: 3 });
    try {
      expect(() => retune.setConfig({ pitchSemitones: 5, bogus: 1 } as never)).toThrow(
        unknownKeyError('StreamingRetune.setConfig: unknown key bogus'),
      );
      // A refused config leaves the current one in place.
      expect(retune.config().pitchSemitones).toBeCloseTo(3, 5);
    } finally {
      retune.destroy();
    }
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
        new TypeError('StreamingRetune.setConfig: grainSizeSamples must be an integer'),
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

describe('StreamingRetune prepare and empty blocks', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports the coded unprepared state before reading a nonempty block', () => {
    const retune = new StreamingRetune();
    try {
      let caught: unknown;
      try {
        retune.processMono(Float32Array.of(Number.NaN));
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidState);
      expect((caught as Error).message).toBe(
        'StreamingRetune.processMono: the retune stage has not been prepared',
      );
    } finally {
      retune.destroy();
    }
  });

  it('treats an empty block as a no-op before and after prepare', () => {
    const retune = new StreamingRetune();
    try {
      expect(retune.grainSizeSamples).toBe(0);
      const unprepared = retune.processMono(new Float32Array(0));
      expect(unprepared).toBeInstanceOf(Float32Array);
      expect(unprepared).toHaveLength(0);
      expect(retune.grainSizeSamples).toBe(0);

      retune.prepare(SR, BLOCK);
      const grainSize = retune.grainSizeSamples;
      expect(retune.processMono(new Float32Array(0))).toHaveLength(0);
      expect(retune.grainSizeSamples).toBe(grainSize);
    } finally {
      retune.destroy();
    }
  });

  it('keeps the previous prepared state after a refused re-prepare', () => {
    const config = { pitchSemitones: 3, mix: 0.75, grainSizeSamples: 512 };
    const retune = new StreamingRetune(config);
    const control = new StreamingRetune(config);
    try {
      retune.prepare(SR, BLOCK);
      control.prepare(SR, BLOCK);
      const first = Float32Array.from({ length: BLOCK }, (_, i) => Math.sin(i * 0.03) * 0.2);
      retune.processMono(first);
      control.processMono(first);

      const grainBefore = retune.grainSizeSamples;
      const latencyBefore = retune.latencySamples;
      expect(expectRefusalOf(() => retune.prepare(7999, BLOCK), RangeError).message).toBe(
        'StreamingRetune.prepare: sampleRate must be an integer in [8000, 384000]',
      );
      expect(expectRefusalOf(() => retune.prepare(SR + 0.5, BLOCK)).message).toBe(
        'StreamingRetune.prepare: sampleRate must be an integer in [8000, 384000]',
      );
      expect(expectRefusalOf(() => retune.prepare(1e100, BLOCK)).message).toBe(
        'StreamingRetune.prepare: sampleRate must be an integer in [8000, 384000]',
      );
      const wrongSecondShape = expectRefusalOf(
        () => retune.prepare(7999, 'invalid' as unknown as number),
        TypeError,
      );
      expect(expectRefusalOf(() => retune.prepare(7999, 0.5)).message).toBe(
        'StreamingRetune.prepare: sampleRate must be an integer in [8000, 384000]',
      );
      expect(wrongSecondShape.message).toBe(
        'StreamingRetune.prepare: maxBlockSize must be a number',
      );
      expect(expectRefusalOf(() => retune.prepare(SR, 0)).message).toBe(
        'StreamingRetune.prepare: maxBlockSize must be an integer in [1, 1048576]',
      );
      expect(expectRefusalOf(() => retune.prepare(SR, 0.5)).message).toBe(
        'StreamingRetune.prepare: maxBlockSize must be an integer in [1, 1048576]',
      );
      expect(expectRefusalOf(() => retune.prepare(SR, 1e100)).message).toBe(
        'StreamingRetune.prepare: maxBlockSize must be an integer in [1, 1048576]',
      );
      expect(retune.grainSizeSamples).toBe(grainBefore);
      expect(retune.latencySamples).toBe(latencyBefore);

      const next = Float32Array.from({ length: BLOCK }, (_, i) => Math.cos(i * 0.02) * 0.15);
      const expected = control.processMono(next);
      const actual = retune.processMono(next);
      const scale = Math.max(...expected.map(Math.abs));
      expect(scale).toBeGreaterThan(0);
      expect(actual).toHaveLength(expected.length);
      const maxError = Math.max(...expected.map((sample, i) => Math.abs(sample - actual[i])));
      expect(maxError).toBeLessThanOrEqual(scale * 8 * 2 ** -23);
    } finally {
      retune.destroy();
      control.destroy();
    }
  });
});
