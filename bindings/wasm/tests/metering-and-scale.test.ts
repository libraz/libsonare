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
  Audio,
  init,
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringRmsDb,
  meteringTruePeakDbtp,
  scaleCorrectionSemitones,
  scaleMaskForMode,
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

// Two channels of five frames each (the old interleaved fixture, split by channel).
const STEREO_PLANES = [
  new Float32Array([-1.0, 0.25, 0.75, -0.5, 0.0]),
  new Float32Array([0.5, -0.25, 0.1, -0.75, 0.9]),
];

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
    const tp = meteringTruePeakDbtp(samples, SR);
    expect(tp).toBeGreaterThanOrEqual(meteringPeakDb(samples, SR) - 0.1);
  });

  it('true peak validates oversampling factors like the C API', () => {
    const samples = sine(440, 1);
    expect(Number.isFinite(meteringTruePeakDbtp(samples, SR, { oversampleFactor: 2 }))).toBe(true);
    // An explicit 0 is refused rather than read as the default.
    expect(() => meteringTruePeakDbtp(samples, SR, { oversampleFactor: 0 })).toThrow(RangeError);
    expect(() => meteringTruePeakDbtp(samples, SR, { oversampleFactor: -1 })).toThrow();
    expect(() => meteringTruePeakDbtp(samples, SR, { oversampleFactor: 3 })).toThrow();
    expect(() => meteringTruePeakDbtp(samples, SR, { oversampleFactor: 32 })).toThrow();
  });

  it('detect clipping reports runs', () => {
    const samples = new Float32Array(8000).fill(0.1);
    for (let i = 1000; i < 1064; i++) {
      samples[i] = 1.0;
    }
    const report = meteringDetectClipping(samples, SR, { thresholdDb: 20 * Math.log10(0.999) });
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

  it('dynamic range keeps a silent low end infinite and leaves an unmeasured range absent', () => {
    const tone = sine(440, 2);
    const gapped = new Float32Array(tone.length + SR * 2);
    gapped.set(tone, 0);
    const report = meteringDynamicRange(gapped, SR, { windowSec: 0.4, hopSec: 0.2 });
    expect(report.lowPercentileDb).toBe(Number.NEGATIVE_INFINITY);
    expect(Number.isFinite(report.highPercentileDb)).toBe(true);
    expect(report.dynamicRangeDb).toBe(Number.POSITIVE_INFINITY);

    const silent = meteringDynamicRange(new Float32Array(SR * 2), SR);
    expect('dynamicRangeDb' in silent).toBe(false);
    expect(silent.lowPercentileDb).toBe(Number.NEGATIVE_INFINITY);
    expect(silent.highPercentileDb).toBe(Number.NEGATIVE_INFINITY);
    expect(Audio.fromBuffer(new Float32Array(SR * 2), SR).dynamicRange()).not.toHaveProperty(
      'dynamicRangeDb',
    );
  });

  it('dynamic range rejects inverted percentiles', () => {
    const samples = sine(440, 1);
    expect(() =>
      meteringDynamicRange(samples, SR, { lowPercentile: 0.9, highPercentile: 0.1 }),
    ).toThrow();
  });

  it('dynamic range rejects non-finite and out-of-range options', () => {
    const samples = sine(440, 1);
    expect(() => meteringDynamicRange(samples, SR, { windowSec: Number.NaN })).toThrow();
    expect(() => meteringDynamicRange(samples, SR, { hopSec: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => meteringDynamicRange(samples, SR, { lowPercentile: Number.NaN })).toThrow();
    expect(() => meteringDynamicRange(samples, SR, { highPercentile: 2 })).toThrow();
  });

  it('waveform peaks bucket planar stereo audio', () => {
    const report = waveformPeaks({ channels: STEREO_PLANES, samplesPerBucket: 2 });
    expect(report.min).toHaveLength(2);
    expect(report.bucketCount).toBe(3);
    expect(report.samplesPerBucket).toBe(2);
    expect(Array.from(report.min[0] as Float32Array)).toEqual([
      expect.closeTo(-1.0, 6),
      expect.closeTo(-0.5, 6),
      expect.closeTo(0.0, 6),
    ]);
    expect(Array.from(report.min[1] as Float32Array)).toEqual([
      expect.closeTo(-0.25, 6),
      expect.closeTo(-0.75, 6),
      expect.closeTo(0.9, 6),
    ]);
    expect(Array.from(report.max[0] as Float32Array)).toEqual([
      expect.closeTo(0.25, 6),
      expect.closeTo(0.75, 6),
      expect.closeTo(0.0, 6),
    ]);
    expect(Array.from(report.max[1] as Float32Array)).toEqual([
      expect.closeTo(0.5, 6),
      expect.closeTo(0.1, 6),
      expect.closeTo(0.9, 6),
    ]);
  });

  it('waveform peak pyramid returns each requested level', () => {
    const pyramid = waveformPeakPyramid({
      channels: STEREO_PLANES,
      samplesPerBucketLevels: [2, 4],
    });
    expect(pyramid).toHaveLength(2);
    expect(pyramid[0]?.bucketCount).toBe(3);
    expect(pyramid[1]?.bucketCount).toBe(2);
    expect(pyramid[1]?.min[0]?.[0]).toBeCloseTo(-1.0, 6);
    expect(pyramid[1]?.max[0]?.[0]).toBeCloseTo(0.75, 6);
  });

  it('waveform peaks default to 512-frame buckets and the standard zoom pyramid', () => {
    const report = waveformPeaks({ channels: STEREO_PLANES });
    expect(report.samplesPerBucket).toBe(512);
    expect(report.bucketCount).toBe(1);
    const pyramid = waveformPeakPyramid({ channels: STEREO_PLANES });
    expect(pyramid.map((level) => level.samplesPerBucket)).toEqual([512, 1024, 2048, 4096]);
  });
});

describe('Scale quantizer wrappers (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses a wrong-typed scale root, mode mask or pitch class as a non-integer', () => {
    const bad = 'x' as unknown as number;
    const rows: Array<[string, () => unknown, RegExp]> = [
      [
        'scaleQuantizeMidi root',
        () => scaleQuantizeMidi(bad, 1, 60),
        /scaleQuantizeMidi: root must be an integer/,
      ],
      [
        'scaleQuantizeMidi modeMask',
        () => scaleQuantizeMidi(0, bad, 60),
        /scaleQuantizeMidi: modeMask must be an integer/,
      ],
      [
        'scaleMaskForMode root',
        () => scaleMaskForMode(bad as never, 'major'),
        /scaleMaskForMode: root must be an integer/,
      ],
      [
        'scaleCorrectionSemitones root',
        () => scaleCorrectionSemitones(bad, 1, 60),
        /scaleCorrectionSemitones: root must be an integer/,
      ],
      [
        'scaleCorrectionSemitones modeMask',
        () => scaleCorrectionSemitones(0, bad, 60),
        /scaleCorrectionSemitones: modeMask must be an integer/,
      ],
      [
        'scalePitchClassEnabled root',
        () => scalePitchClassEnabled(bad, 1, 0),
        /scalePitchClassEnabled: root must be an integer/,
      ],
      [
        'scalePitchClassEnabled modeMask',
        () => scalePitchClassEnabled(0, bad, 0),
        /scalePitchClassEnabled: modeMask must be an integer/,
      ],
      [
        'scalePitchClassEnabled pitchClass',
        () => scalePitchClassEnabled(0, 1, bad),
        /scalePitchClassEnabled: pitchClass must be an integer/,
      ],
    ];
    for (const [, call, message] of rows) {
      expect(call).toThrow(TypeError);
      expect(call).toThrow(message);
    }
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
