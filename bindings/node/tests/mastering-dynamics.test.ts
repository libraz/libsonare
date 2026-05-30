/**
 * Tests for the offline mastering dynamics Node wrappers.
 *
 * Thin wrappers over mastering::dynamics::{Compressor,Gate,TransientShaper}.
 * Verifies marshaling: length-preserving Float32Array out, latency in the
 * result, options-object plumbing, and string/number detector overloading.
 */

import { describe, expect, it } from 'vitest';
import {
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
} from '../src/index';

const SR = 44100;

function sine(freq: number, dur = 1.0, amp = 0.8): Float32Array {
  const n = Math.floor(SR * dur);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function peak(arr: Float32Array): number {
  let p = 0;
  for (const v of arr) {
    p = Math.max(p, Math.abs(v));
  }
  return p;
}

describe('masteringDynamicsCompressor (Node)', () => {
  it('default params runs', () => {
    const x = sine(440);
    const r = masteringDynamicsCompressor(x, SR);
    expect(r.samples.length).toBe(x.length);
    expect(Array.from(r.samples).every(Number.isFinite)).toBe(true);
    expect(r.latencySamples).toBeGreaterThanOrEqual(0);
  });

  it('reduces peak above threshold', () => {
    const x = sine(440, 1.0, 0.9);
    const r = masteringDynamicsCompressor(x, SR, {
      thresholdDb: -30,
      ratio: 10,
      attackMs: 0.1,
      releaseMs: 10,
    });
    const half = Math.floor(x.length / 2);
    const px = peak(x.slice(half));
    const py = peak(r.samples.slice(half));
    expect(py).toBeLessThan(px * 0.95);
  });

  it('detector accepts string and number', () => {
    const x = sine(440);
    const a = masteringDynamicsCompressor(x, SR, { detector: 'peak' }).samples;
    const b = masteringDynamicsCompressor(x, SR, { detector: 0 }).samples;
    expect(Array.from(a)).toEqual(Array.from(b));
  });
});

describe('masteringDynamicsGate (Node)', () => {
  it('default params runs', () => {
    const x = sine(440);
    const r = masteringDynamicsGate(x, SR);
    expect(r.samples.length).toBe(x.length);
    expect(Array.from(r.samples).every(Number.isFinite)).toBe(true);
  });

  it('attenuates quiet sections', () => {
    // Very quiet signal below the gate threshold should be attenuated by range_db.
    const x = sine(440, 1.0, 0.001);
    const r = masteringDynamicsGate(x, SR, { thresholdDb: -40, rangeDb: -60 });
    expect(peak(r.samples)).toBeLessThan(peak(x));
  });
});

describe('masteringDynamicsTransientShaper (Node)', () => {
  it('default params runs', () => {
    const x = sine(440);
    const r = masteringDynamicsTransientShaper(x, SR);
    expect(r.samples.length).toBe(x.length);
    expect(Array.from(r.samples).every(Number.isFinite)).toBe(true);
  });

  it('different attack/sustain gains change output', () => {
    const x = sine(440);
    const a = masteringDynamicsTransientShaper(x, SR, {
      attackGainDb: 6,
      sustainGainDb: 0,
    }).samples;
    const b = masteringDynamicsTransientShaper(x, SR, {
      attackGainDb: 0,
      sustainGainDb: 0,
    }).samples;
    let differ = 0;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) {
        differ++;
      }
    }
    expect(differ).toBeGreaterThan(0);
  });
});
