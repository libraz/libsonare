/**
 * Tests for the offline metering + scale-quantizer Node wrappers.
 *
 * These functions are thin pass-throughs over the C API entry points added
 * in sonare_c_editing.cpp. The goal is to verify the marshaling: result
 * fields are correctly typed, heap arrays are returned as TypedArrays, and
 * the documented defaults match the C side.
 */

import { describe, expect, it } from 'vitest';
import {
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringRmsDb,
  meteringTruePeakDb,
  scaleCorrectionSemitones,
  scalePitchClassEnabled,
  scaleQuantizeMidi,
} from '../src/index';

const SR = 22050;
const C_MAJOR_MASK = 0b101010110101;

function sine(freq: number, durationSec: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('Offline metering wrappers', () => {
  it('peak / rms / crest / dc agree on a 440 Hz sine', () => {
    const samples = sine(440, 1);
    const peak = meteringPeakDb(samples, SR);
    const rms = meteringRmsDb(samples, SR);
    const crest = meteringCrestFactorDb(samples, SR);
    const dc = meteringDcOffset(samples, SR);
    expect(Number.isFinite(peak)).toBe(true);
    expect(Number.isFinite(rms)).toBe(true);
    expect(peak).toBeGreaterThanOrEqual(rms);
    expect(crest).toBeCloseTo(peak - rms, 2);
    expect(Math.abs(dc)).toBeLessThan(1e-2);
  });

  it('true peak >= sample peak with default oversample factor', () => {
    const samples = sine(440, 1);
    const tp = meteringTruePeakDb(samples, SR);
    expect(tp).toBeGreaterThanOrEqual(meteringPeakDb(samples, SR) - 0.1);
  });

  it('true peak rejects non-power-of-two oversample factor', () => {
    const samples = sine(440, 0.1);
    expect(() => meteringTruePeakDb(samples, SR, 3)).toThrow();
  });

  it('detect clipping reports runs and returns plain JS arrays', () => {
    const samples = new Float32Array(8000).fill(0.1);
    for (let i = 1000; i < 1064; i++) {
      samples[i] = 1.0;
    }
    const report = meteringDetectClipping(samples, SR, 0.999);
    expect(report.clippedSamples).toBeGreaterThanOrEqual(1);
    expect(report.regions.length).toBeGreaterThanOrEqual(1);
    expect(report.maxClippedPeak).toBeGreaterThanOrEqual(1.0);
    const region = report.regions[0];
    if (!region) {
      throw new Error('expected a clipping region');
    }
    expect(region.startSample).toBeLessThanOrEqual(region.endSample);
    expect(region.length).toBeGreaterThanOrEqual(1);
    expect(region.peak).toBeCloseTo(1.0, 5);
  });

  it('detect clipping returns empty regions for clean signal', () => {
    const samples = sine(440, 0.5);
    for (let i = 0; i < samples.length; i++) {
      samples[i] *= 0.3;
    }
    const report = meteringDetectClipping(samples, SR);
    expect(report.clippedSamples).toBe(0);
    expect(report.regions).toEqual([]);
  });

  it('dynamic range returns a positive DR for varying-level signal', () => {
    const loud = sine(440, 3.5);
    const quiet = sine(440, 3.5);
    for (let i = 0; i < quiet.length; i++) {
      quiet[i] *= 0.05;
    }
    const combined = new Float32Array(loud.length + quiet.length + loud.length);
    combined.set(loud, 0);
    combined.set(quiet, loud.length);
    combined.set(loud, loud.length + quiet.length);
    const report = meteringDynamicRange(combined, SR);
    expect(report.windowRmsDb).toBeInstanceOf(Float32Array);
    expect(report.windowRmsDb.length).toBeGreaterThanOrEqual(2);
    expect(report.dynamicRangeDb).toBeGreaterThan(0);
  });

  it('dynamic range rejects inverted percentiles', () => {
    const samples = sine(440, 1);
    expect(() => meteringDynamicRange(samples, SR, 0, 0, 0.9, 0.1)).toThrow();
  });
});

describe('Scale quantizer wrappers', () => {
  it('snaps off-scale notes to the nearest enabled pitch class', () => {
    const q = scaleQuantizeMidi(0, C_MAJOR_MASK, 61.0);
    expect([60, 62]).toContain(Math.round(q));
  });

  it('in-scale notes pass through', () => {
    expect(scaleQuantizeMidi(0, C_MAJOR_MASK, 60.0)).toBeCloseTo(60.0, 2);
  });

  it('correction_semitones agrees with quantize_midi', () => {
    const q = scaleQuantizeMidi(0, C_MAJOR_MASK, 61.4);
    const c = scaleCorrectionSemitones(0, C_MAJOR_MASK, 61.4);
    expect(c).toBeCloseTo(q - 61.4, 2);
  });

  it('pitch_class_enabled reflects the mode mask', () => {
    expect(scalePitchClassEnabled(0, C_MAJOR_MASK, 0)).toBe(true);
    expect(scalePitchClassEnabled(0, C_MAJOR_MASK, 1)).toBe(false);
  });

  it('rejects bad arguments', () => {
    expect(() => scaleQuantizeMidi(-1, C_MAJOR_MASK, 60.0)).toThrow();
    expect(() => scaleQuantizeMidi(0, 0, 60.0)).toThrow();
    expect(() => scalePitchClassEnabled(0, C_MAJOR_MASK, 12)).toThrow();
  });
});
