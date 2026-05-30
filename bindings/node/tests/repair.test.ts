/**
 * Tests for the offline mastering repair Node wrappers.
 *
 * These functions are thin wrappers over mastering::repair::declick and
 * mastering::repair::denoise_classical. The goal is to verify the marshaling:
 * length-preserving Float32Array out, options-object plumbing, and validation.
 */

import { describe, expect, it } from 'vitest';
import { masteringRepairDeclick, masteringRepairDenoiseClassical } from '../src/index';

const SR = 22050;

function sine(freq: number, durationSec: number, amp = 0.3): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function withNoise(samples: Float32Array, amp: number): Float32Array {
  // Deterministic LCG noise so the test is reproducible.
  let state = 1 >>> 0;
  const out = new Float32Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const u = (state >>> 8) / (1 << 24);
    out[i] = (samples[i] ?? 0) + (u - 0.5) * amp;
  }
  return out;
}

describe('masteringRepairDeclick (Node)', () => {
  it('returns a same-length Float32Array with default options', () => {
    const samples = sine(440, 0.3);
    // Inject a few clicks.
    for (let i = 0; i < 5; i++) {
      samples[1000 + i * 1200] = 1.0;
    }
    const out = masteringRepairDeclick(samples, SR);
    expect(out).toBeInstanceOf(Float32Array);
    expect(out.length).toBe(samples.length);
    expect(Number.isFinite(out[0])).toBe(true);
  });

  it('accepts explicit options', () => {
    const samples = sine(440, 0.2);
    const out = masteringRepairDeclick(samples, SR, {
      threshold: 0.7,
      neighborRatio: 4.0,
      maxClickSamples: 8,
      lpcOrder: 16,
      residualRatio: 6.0,
    });
    expect(out.length).toBe(samples.length);
  });
});

describe('masteringRepairDenoiseClassical (Node)', () => {
  const clean = sine(440, 0.5, 0.5);
  const noisy = withNoise(clean, 0.4);

  it('runs with default options (LogMMSE)', () => {
    const out = masteringRepairDenoiseClassical(noisy, SR);
    expect(out).toBeInstanceOf(Float32Array);
    expect(out.length).toBe(noisy.length);
  });

  it('accepts string-typed mode and noiseEstimator', () => {
    const out = masteringRepairDenoiseClassical(noisy, SR, {
      mode: 'spectralSubtraction',
      noiseEstimator: 'quantile',
      nFft: 1024,
      hopLength: 256,
      overSubtraction: 2.0,
      spectralFloor: 0.05,
      gainFloor: 0.05,
      speechPresenceGain: false,
      gainSmoothing: true,
    });
    expect(out.length).toBe(noisy.length);
  });

  it('accepts mmseStsa mode', () => {
    const out = masteringRepairDenoiseClassical(noisy, SR, {
      mode: 'mmseStsa',
      nFft: 512,
      hopLength: 128,
    });
    expect(out.length).toBe(noisy.length);
  });

  it('rejects non-power-of-two nFft', () => {
    expect(() =>
      masteringRepairDenoiseClassical(noisy, SR, { nFft: 1500, hopLength: 256 }),
    ).toThrow();
  });

  it('rejects non-positive hopLength', () => {
    expect(() =>
      masteringRepairDenoiseClassical(noisy, SR, { nFft: 1024, hopLength: 0 }),
    ).toThrow();
  });

  it('rejects unknown string enums', () => {
    expect(() =>
      masteringRepairDenoiseClassical(noisy, SR, { mode: 'not-a-mode' as never }),
    ).toThrow(/unknown denoise mode/i);
    expect(() =>
      masteringRepairDenoiseClassical(noisy, SR, { noiseEstimator: 'not-an-estimator' as never }),
    ).toThrow(/unknown denoise noise estimator/i);
  });
});
