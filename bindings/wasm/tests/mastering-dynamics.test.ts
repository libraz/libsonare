/**
 * Tests for the offline mastering dynamics WASM wrappers.
 *
 * These functions are thin embind shims over mastering::dynamics::Compressor,
 * Gate, and TransientShaper. The goal is to verify the marshaling:
 * length-preserving Float32Array out wrapped in { samples, latencySamples },
 * detector string/number plumbing, and that the processors actually act on
 * the audio (compressor reduces peak above threshold, gate attenuates below
 * threshold, transient shaper preserves length and remains finite).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
} from '../src/index';

const SR = 44100;

function sine(freq: number, durationSec: number, amp = 0.8): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function peakAbs(arr: Float32Array, start = 0, end = arr.length): number {
  let p = 0;
  for (let i = start; i < end; i++) {
    const v = Math.abs(arr[i] ?? 0);
    if (v > p) {
      p = v;
    }
  }
  return p;
}

function allFinite(arr: Float32Array): boolean {
  for (let i = 0; i < arr.length; i++) {
    if (!Number.isFinite(arr[i] ?? Number.NaN)) {
      return false;
    }
  }
  return true;
}

describe('masteringDynamicsCompressor (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns a same-length Float32Array with default options', () => {
    const x = sine(440, 0.3);
    const r = masteringDynamicsCompressor(x, SR);
    expect(r.samples).toBeInstanceOf(Float32Array);
    expect(r.samples.length).toBe(x.length);
    expect(typeof r.latencySamples).toBe('number');
    expect(allFinite(r.samples)).toBe(true);
  });

  it('reduces peak when threshold is below input level', () => {
    const x = sine(440, 0.5, 0.9);
    const r = masteringDynamicsCompressor(x, SR, {
      thresholdDb: -30,
      ratio: 10,
      attackMs: 0.1,
      releaseMs: 10,
      makeupGainDb: 0,
    });
    const half = Math.floor(x.length / 2);
    expect(peakAbs(r.samples, half)).toBeLessThan(peakAbs(x, half) * 0.95);
  });

  it('detector accepts both string and number forms', () => {
    const x = sine(440, 0.2, 0.5);
    const a = masteringDynamicsCompressor(x, SR, { detector: 'peak' }).samples;
    const b = masteringDynamicsCompressor(x, SR, { detector: 0 }).samples;
    expect(a.length).toBe(b.length);
    for (let i = 0; i < a.length; i++) {
      expect(a[i]).toBeCloseTo(b[i] ?? 0, 6);
    }
  });

  it('rejects empty input with a clear RangeError', () => {
    expect(() => masteringDynamicsCompressor(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsCompressor: samples must not be empty/,
    );
  });
});

describe('masteringDynamicsGate (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns a same-length Float32Array with default options', () => {
    const x = sine(440, 0.3, 0.5);
    const r = masteringDynamicsGate(x, SR);
    expect(r.samples).toBeInstanceOf(Float32Array);
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('attenuates signals well below the threshold', () => {
    // Quiet signal (-40 dBFS-ish) gated at -20 dB with -80 dB range.
    const x = sine(440, 0.5, 0.01);
    const r = masteringDynamicsGate(x, SR, {
      thresholdDb: -20,
      attackMs: 1,
      releaseMs: 5,
      rangeDb: -80,
      holdMs: 0,
      closeThresholdDb: -20,
    });
    const half = Math.floor(x.length / 2);
    expect(peakAbs(r.samples, half)).toBeLessThan(peakAbs(x, half) * 0.5);
  });

  it('accepts explicit options', () => {
    const x = sine(440, 0.2, 0.5);
    const r = masteringDynamicsGate(x, SR, {
      thresholdDb: -50,
      attackMs: 2,
      releaseMs: 80,
      rangeDb: -60,
      holdMs: 5,
      closeThresholdDb: -55,
      keyHpfHz: 100,
    });
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('rejects empty input with a clear RangeError', () => {
    expect(() => masteringDynamicsGate(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsGate: samples must not be empty/,
    );
  });
});

describe('masteringDynamicsTransientShaper (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns a same-length Float32Array with default options', () => {
    const x = sine(440, 0.3, 0.5);
    const r = masteringDynamicsTransientShaper(x, SR);
    expect(r.samples).toBeInstanceOf(Float32Array);
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('accepts explicit options', () => {
    const x = sine(440, 0.2, 0.5);
    const r = masteringDynamicsTransientShaper(x, SR, {
      attackGainDb: 6,
      sustainGainDb: -3,
      fastAttackMs: 0,
      fastReleaseMs: 20,
      slowAttackMs: 15,
      slowReleaseMs: 200,
      sensitivity: 1,
      maxGainDb: 12,
      gainSmoothingMs: 1,
      lookaheadMs: 0,
    });
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('latencySamples reflects lookahead', () => {
    const x = sine(440, 0.1, 0.5);
    const noLookahead = masteringDynamicsTransientShaper(x, SR, { lookaheadMs: 0 });
    const withLookahead = masteringDynamicsTransientShaper(x, SR, { lookaheadMs: 5 });
    expect(withLookahead.latencySamples).toBeGreaterThanOrEqual(noLookahead.latencySamples);
  });

  it('rejects empty input with a clear RangeError', () => {
    expect(() => masteringDynamicsTransientShaper(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsTransientShaper: samples must not be empty/,
    );
  });
});
