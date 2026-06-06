/**
 * Tests for the offline metering + scale-quantizer WASM wrappers.
 *
 * These functions are thin embind shims over the metering/* modules and the
 * 12-TET ScaleQuantizer. The goal is to verify the marshaling: result fields
 * are correctly typed, region/window arrays are returned as plain JS arrays
 * or TypedArrays, and the documented defaults match the C++ side.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
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
  waveformPeakPyramid,
  waveformPeaks,
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

describe('Offline metering wrappers (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

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

  it('true peak >= sample peak', () => {
    const samples = sine(440, 1);
    const tp = meteringTruePeakDb(samples, SR);
    expect(tp).toBeGreaterThanOrEqual(meteringPeakDb(samples, SR) - 0.1);
  });

  it('true peak validates oversampling factors like the C API', () => {
    const samples = sine(440, 1);
    expect(Number.isFinite(meteringTruePeakDb(samples, SR, 0))).toBe(true);
    expect(() => meteringTruePeakDb(samples, SR, -1)).toThrow();
    expect(() => meteringTruePeakDb(samples, SR, 3)).toThrow();
    expect(() => meteringTruePeakDb(samples, SR, 32)).toThrow();
  });

  it('detect clipping reports runs', () => {
    const samples = new Float32Array(8000).fill(0.1);
    for (let i = 1000; i < 1064; i++) {
      samples[i] = 1.0;
    }
    const report = meteringDetectClipping(samples, SR, { threshold: 0.999 });
    expect(report.clippedSamples).toBeGreaterThanOrEqual(1);
    expect(report.regions.length).toBeGreaterThanOrEqual(1);
    expect(report.maxClippedPeak).toBeGreaterThanOrEqual(1.0);
    const region = report.regions[0];
    if (!region) {
      throw new Error('expected a clipping region');
    }
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

  it('dynamic range returns a positive DR for varying signal', () => {
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
    expect(() =>
      meteringDynamicRange(samples, SR, { lowPercentile: 0.9, highPercentile: 0.1 }),
    ).toThrow();
  });

  it('waveform peaks bucket interleaved stereo audio', () => {
    const samples = new Float32Array([-1.0, 0.5, 0.25, -0.25, 0.75, 0.1, -0.5, -0.75, 0.0, 0.9]);
    const report = waveformPeaks(samples, 2, { samplesPerBucket: 2 });
    expect(report.channels).toBe(2);
    expect(report.bucketCount).toBe(3);
    expect(report.samplesPerBucket).toBe(2);
    expect(Array.from(report.min)).toEqual([
      expect.closeTo(-1.0, 6),
      expect.closeTo(-0.5, 6),
      expect.closeTo(0.0, 6),
      expect.closeTo(-0.25, 6),
      expect.closeTo(-0.75, 6),
      expect.closeTo(0.9, 6),
    ]);
    expect(Array.from(report.max)).toEqual([
      expect.closeTo(0.25, 6),
      expect.closeTo(0.75, 6),
      expect.closeTo(0.0, 6),
      expect.closeTo(0.5, 6),
      expect.closeTo(0.1, 6),
      expect.closeTo(0.9, 6),
    ]);
  });

  it('waveform peak pyramid returns each requested level', () => {
    const samples = new Float32Array([-1.0, 0.5, 0.25, -0.25, 0.75, 0.1, -0.5, -0.75, 0.0, 0.9]);
    const pyramid = waveformPeakPyramid(samples, 2, { samplesPerBucketLevels: [2, 4] });
    expect(pyramid).toHaveLength(2);
    expect(pyramid[0]?.bucketCount).toBe(3);
    expect(pyramid[1]?.bucketCount).toBe(2);
    expect(pyramid[1]?.min[0]).toBeCloseTo(-1.0, 6);
    expect(pyramid[1]?.max[0]).toBeCloseTo(0.75, 6);
  });

  it('waveform peaks default to 512-frame buckets and the standard zoom pyramid', () => {
    const samples = new Float32Array([-1.0, 0.5, 0.25, -0.25, 0.75, 0.1, -0.5, -0.75, 0.0, 0.9]);
    const report = waveformPeaks(samples, 2);
    expect(report.samplesPerBucket).toBe(512);
    expect(report.bucketCount).toBe(1);
    const pyramid = waveformPeakPyramid(samples, 2);
    expect(pyramid.map((level) => level.samplesPerBucket)).toEqual([512, 1024, 2048, 4096]);
  });
});

describe('Scale quantizer wrappers (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('snaps off-scale notes to nearest enabled pitch class', () => {
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
