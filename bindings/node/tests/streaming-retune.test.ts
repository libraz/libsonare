/**
 * StreamingRetune reached only WASM: no C ABI, no Node, no Python, while its
 * siblings in the same module family (StreamingMasteringChain,
 * StreamingEqualizer) were mirrored to all three. These pin the Node half of
 * that mirror, and the rejection policy the four surfaces now share: a
 * non-finite control or sample is refused rather than substituted or zeroed,
 * because either would enter the grain history and persist into every later
 * block with nothing to tell the caller its input had been altered.
 */

import { describe, expect, it } from 'vitest';
import { StreamingRetune, type StreamingRetuneConfig } from '../src/index.js';

const SR = 48000;
const BLOCK = 512;

const sine = (n: number, hz: number, offset = 0): Float32Array =>
  new Float32Array(n).map((_, i) => Math.sin((2 * Math.PI * hz * (i + offset)) / SR));

const energy = (data: Float32Array): number => data.reduce((sum, v) => sum + v * v, 0);

describe('StreamingRetune', () => {
  it('exposes the same lifecycle as the WASM class', () => {
    const retune = new StreamingRetune({ pitchSemitones: 12, mix: 1, grainSizeSamples: 512 });
    try {
      expect(retune.grainSizeSamples).toBe(0);
      expect(retune.latencySamples).toBe(0);
      retune.prepare(SR, BLOCK);
      expect(retune.grainSizeSamples).toBe(512);
      expect(retune.latencySamples).toBe(512);
      expect(retune.config()).toEqual({ pitchSemitones: 12, mix: 1, grainSizeSamples: 512 });
      retune.reset();
      expect(retune.grainSizeSamples).toBe(512);
    } finally {
      retune.destroy();
    }
  });

  it('derives a grain from the sample rate when none is requested', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.grainSizeSamples).toBe(2232);
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

  it('re-derives the grain at a new sample rate after an unrelated setConfig', () => {
    // setConfig seeded every key from config(), and config().grainSizeSamples is the
    // EFFECTIVE grain once prepared -- so touching any other control froze the 0
    // "derive from the sample rate" sentinel into a literal, and a host
    // re-preparing at 22.05 kHz kept the 48 kHz grain: double the latency and
    // double the transient smear, with no diagnostic saying why.
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.grainSizeSamples).toBe(2232);
      retune.setConfig({ pitchSemitones: 5 });
      retune.prepare(22050, BLOCK);
      expect(retune.grainSizeSamples).toBe(1024);
      // And an explicitly requested grain still survives an unrelated update.
      retune.setConfig({ grainSizeSamples: 512 });
      retune.setConfig({ mix: 0.5 });
      retune.prepare(22050, BLOCK);
      expect(retune.grainSizeSamples).toBe(512);
    } finally {
      retune.destroy();
    }
  });

  it('reads an explicit undefined the same as an absent key', () => {
    // The shared-reader contract the addon convention scanner enforces: a key
    // set to undefined must fall back to the current value, never to a typed
    // accessor's dummy zero.
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

  it('shifts pitch and leaves the caller buffer alone', () => {
    const retune = new StreamingRetune({ pitchSemitones: 12, mix: 1, grainSizeSamples: 512 });
    try {
      retune.prepare(SR, BLOCK);
      let out: Float32Array<ArrayBufferLike> = new Float32Array(BLOCK);
      for (let pass = 0; pass < 8; pass++) {
        const input = sine(BLOCK, 220, pass * BLOCK);
        const before = Array.from(input);
        out = retune.processMono(input);
        // The addon copies before processing; the argument is never rewritten.
        expect(Array.from(input)).toEqual(before);
      }
      // Past the one-grain latency the output carries energy: a silent result
      // would mean the block never reached the core.
      expect(energy(out)).toBeGreaterThan(1);
    } finally {
      retune.destroy();
    }
  });

  it('reports the C ABI rejections rather than silently continuing', () => {
    expect(() => new StreamingRetune({ pitchSemitones: Number.NaN })).toThrow();
    expect(() => new StreamingRetune({ mix: Number.POSITIVE_INFINITY })).toThrow();

    const retune = new StreamingRetune();
    try {
      // Processing before prepare: the core answers a violated precondition
      // with a silent no-op to stay audio-thread callable, so without the C
      // ABI guard this would look like a successful render of silence.
      expect(() => retune.processMono(new Float32Array(64))).toThrow();

      retune.prepare(SR, 64);
      expect(() => retune.processMono(new Float32Array(128))).toThrow();
      expect(() => retune.setConfig({ pitchSemitones: Number.NaN })).toThrow();

      const withNan = new Float32Array(64);
      withNan[7] = Number.NaN;
      expect(() => retune.processMono(withNan)).toThrow();
      // Refused means untouched, not partially rewritten.
      expect(Number.isNaN(withNan[7])).toBe(true);
    } finally {
      retune.destroy();
    }
  });

  it('is safe to destroy twice', () => {
    const retune = new StreamingRetune();
    retune.destroy();
    expect(() => retune.destroy()).not.toThrow();
  });

  it('refuses an unknown config key by name on the constructor and setConfig', () => {
    expect(() => new StreamingRetune({ pitch: 3 } as never)).toThrow(
      new TypeError("StreamingRetune: unknown request key 'pitch'"),
    );
    const retune = new StreamingRetune({ pitchSemitones: 3 });
    try {
      expect(() => retune.setConfig({ pitchSemitones: 5, bogus: 1 } as never)).toThrow(
        new TypeError("StreamingRetune.setConfig: unknown request key 'bogus'"),
      );
      // A refused config leaves the current one in place.
      expect(retune.config().pitchSemitones).toBeCloseTo(3, 5);
    } finally {
      retune.destroy();
    }
  });
});

describe('StreamingRetune grain', () => {
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
