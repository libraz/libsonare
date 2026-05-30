/**
 * Tests for the offline stereo / phase-scope / spectrum Node wrappers.
 *
 * These functions are thin pass-throughs over sonare_c_editing.cpp entries;
 * the goal is to verify the marshaling: heap arrays come back as
 * Float32Arrays, summary scalars line up with the C++ side, and the documented
 * defaults match.
 */

import { describe, expect, it } from 'vitest';
import {
  meteringPhaseScope,
  meteringSpectrum,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringVectorscope,
} from '../src/index';

const SR = 22050;

function sine(freq: number, durationSec: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function inverted(samples: Float32Array): Float32Array {
  const out = new Float32Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    out[i] = -(samples[i] ?? 0);
  }
  return out;
}

describe('Stereo meter wrappers (Node)', () => {
  it('correlation is +1 for in-phase and -1 for inverted', () => {
    const left = sine(440, 0.5);
    const right = left.slice();
    expect(meteringStereoCorrelation(left, right, SR)).toBeCloseTo(1.0, 3);
    expect(meteringStereoCorrelation(left, inverted(left), SR)).toBeCloseTo(-1.0, 3);
  });

  it('stereo_width is ~0 for mono and > 0 when polarity flips', () => {
    const left = sine(440, 0.5);
    const width_mono = meteringStereoWidth(left, left, SR);
    const width_inv = meteringStereoWidth(left, inverted(left), SR);
    expect(Math.abs(width_mono)).toBeLessThan(1e-3);
    expect(width_inv).toBeGreaterThan(width_mono);
  });

  it('vectorscope returns one point per sample as Float32Array pair', () => {
    const left = sine(440, 0.1);
    const result = meteringVectorscope(left, left, SR);
    expect(result.mid).toBeInstanceOf(Float32Array);
    expect(result.side).toBeInstanceOf(Float32Array);
    expect(result.mid.length).toBe(left.length);
    expect(result.side.length).toBe(left.length);
    // In-phase: side is ~0 everywhere.
    let maxSide = 0;
    for (let i = 0; i < result.side.length; i++) {
      maxSide = Math.max(maxSide, Math.abs(result.side[i] ?? 0));
    }
    expect(maxSide).toBeLessThan(1e-3);
  });

  it('phase_scope populates summary stats', () => {
    const left = sine(440, 0.1);
    const result = meteringPhaseScope(left, left, SR);
    expect(result.mid.length).toBe(left.length);
    expect(result.side.length).toBe(left.length);
    expect(result.radius.length).toBe(left.length);
    expect(result.angleRad.length).toBe(left.length);
    expect(result.correlation).toBeCloseTo(1.0, 3);
    expect(result.maxRadius).toBeGreaterThan(0);
  });

  it('rejects mismatched left/right lengths', () => {
    const left = sine(440, 0.1);
    const right = sine(440, 0.05);
    expect(() => meteringStereoCorrelation(left, right, SR)).toThrow();
    expect(() => meteringVectorscope(left, right, SR)).toThrow();
  });

  it('rejects invalid sample rates', () => {
    const left = sine(440, 0.1);
    expect(() => meteringStereoCorrelation(left, left, 0)).toThrow();
    expect(() => meteringPhaseScope(left, left, -1)).toThrow();
  });
});

describe('Spectrum meter wrapper (Node)', () => {
  it('returns nFft/2+1 bins and a peak near the tone frequency', () => {
    const samples = sine(1000, 0.5);
    const result = meteringSpectrum(samples, SR, { nFft: 2048 });
    const expectedBins = 2048 / 2 + 1;
    expect(result.frequencies.length).toBe(expectedBins);
    expect(result.magnitude.length).toBe(expectedBins);
    expect(result.power.length).toBe(expectedBins);
    expect(result.db.length).toBe(expectedBins);
    expect(result.nFft).toBe(2048);
    expect(result.sampleRate).toBe(SR);
    let peakBin = 0;
    let peakMag = -1;
    for (let i = 0; i < result.magnitude.length; i++) {
      const mag = result.magnitude[i] ?? 0;
      if (mag > peakMag) {
        peakMag = mag;
        peakBin = i;
      }
    }
    expect(result.frequencies[peakBin]).toBeCloseTo(1000, -1);
    // power[i] ≈ magnitude[i]^2 (large magnitude bins drift in the last sig figs).
    const expectedPower = (result.magnitude[peakBin] ?? 0) ** 2;
    expect(Math.abs((result.power[peakBin] ?? 0) - expectedPower) / expectedPower).toBeLessThan(
      1e-5,
    );
  });

  it('uses defaults when options are omitted', () => {
    const samples = sine(440, 0.5);
    const result = meteringSpectrum(samples, SR);
    expect(result.nFft).toBe(2048);
    expect(result.frequencies.length).toBe(2048 / 2 + 1);
  });

  it('rejects non-power-of-two nFft', () => {
    const samples = sine(440, 0.1);
    expect(() => meteringSpectrum(samples, SR, { nFft: 1500 })).toThrow();
  });
});
