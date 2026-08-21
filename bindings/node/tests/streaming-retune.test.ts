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
import { StreamingRetune } from '../src/index.js';

const SR = 48000;
const BLOCK = 512;

const sine = (n: number, hz: number, offset = 0): Float32Array =>
  new Float32Array(n).map((_, i) => Math.sin((2 * Math.PI * hz * (i + offset)) / SR));

const energy = (data: Float32Array): number => data.reduce((sum, v) => sum + v * v, 0);

describe('StreamingRetune', () => {
  it('exposes the same lifecycle as the WASM class', () => {
    const retune = new StreamingRetune({ semitones: 12, mix: 1, grainSize: 512 });
    try {
      expect(retune.grainSize()).toBe(0);
      expect(retune.latencySamples()).toBe(0);
      retune.prepare(SR, BLOCK);
      expect(retune.grainSize()).toBe(512);
      expect(retune.latencySamples()).toBe(512);
      expect(retune.config()).toEqual({ semitones: 12, mix: 1, grainSize: 512 });
      retune.reset();
      expect(retune.grainSize()).toBe(512);
    } finally {
      retune.destroy();
    }
  });

  it('derives a grain from the sample rate when none is requested', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.grainSize()).toBe(2232);
    } finally {
      retune.destroy();
    }
  });

  it('applies a grain requested after prepare on the next prepare', () => {
    const retune = new StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      const derived = retune.grainSize();
      retune.setConfig({ grainSize: 512 });
      // Still the effective grain: the request is structural and takes effect
      // at the next prepare, which is what the config doc promises.
      expect(retune.grainSize()).toBe(derived);
      expect(retune.config().grainSize).toBe(derived);
      retune.prepare(SR, BLOCK);
      expect(retune.grainSize()).toBe(512);
    } finally {
      retune.destroy();
    }
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
      retune.destroy();
    }
  });

  it('reads an explicit undefined the same as an absent key', () => {
    // The shared-reader contract the addon convention scanner enforces: a key
    // set to undefined must fall back to the current value, never to a typed
    // accessor's dummy zero.
    const retune = new StreamingRetune({ semitones: 5, mix: 0.25, grainSize: 512 });
    try {
      retune.prepare(SR, BLOCK);
      const baseline = retune.config();
      retune.setConfig({ semitones: undefined, mix: undefined, grainSize: undefined });
      expect(retune.config()).toEqual(baseline);
      retune.setConfig({});
      expect(retune.config()).toEqual(baseline);
    } finally {
      retune.destroy();
    }
  });

  it('shifts pitch and leaves the caller buffer alone', () => {
    const retune = new StreamingRetune({ semitones: 12, mix: 1, grainSize: 512 });
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
    expect(() => new StreamingRetune({ semitones: Number.NaN })).toThrow();
    expect(() => new StreamingRetune({ mix: Number.POSITIVE_INFINITY })).toThrow();

    const retune = new StreamingRetune();
    try {
      // Processing before prepare: the core answers a violated precondition
      // with a silent no-op to stay audio-thread callable, so without the C
      // ABI guard this would look like a successful render of silence.
      expect(() => retune.processMono(new Float32Array(64))).toThrow();

      retune.prepare(SR, 64);
      expect(() => retune.processMono(new Float32Array(128))).toThrow();
      expect(() => retune.setConfig({ semitones: Number.NaN })).toThrow();

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
});
