import { describe, expect, it } from 'vitest';
import {
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringPhaseScope,
  meteringRmsDb,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDbtp,
  meteringVectorscope,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index.js';
import { present } from './_helpers.js';

const sampleRate = 22050;
const samples = Float32Array.from(
  { length: 4096 },
  (_, i) => Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5,
);
const left = samples;
const right = Float32Array.from(samples, (sample, i) => sample * (i % 2 === 0 ? 1 : 0.8));

describe('metering request-object compatibility', () => {
  it('preserves all mono scalar calls', () => {
    expect(meteringPeakDb({ samples, sampleRate })).toEqual(meteringPeakDb(samples, sampleRate));
    expect(meteringRmsDb({ samples, sampleRate })).toEqual(meteringRmsDb(samples, sampleRate));
    expect(meteringCrestFactorDb({ samples, sampleRate })).toEqual(
      meteringCrestFactorDb(samples, sampleRate),
    );
    expect(meteringDcOffset({ samples, sampleRate })).toEqual(
      meteringDcOffset(samples, sampleRate),
    );
    expect(meteringTruePeakDbtp({ samples, sampleRate, oversampleFactor: 2 })).toEqual(
      meteringTruePeakDbtp(samples, sampleRate, { oversampleFactor: 2 }),
    );
  });

  it('preserves report and spectrum options', () => {
    const clipping = { thresholdDb: -8, minRegionSamples: 2 };
    expect(meteringDetectClipping({ samples, sampleRate, ...clipping })).toEqual(
      meteringDetectClipping(samples, sampleRate, clipping),
    );
    const range = { windowSec: 0.1, hopSec: 0.05, lowPercentile: 0.1, highPercentile: 0.9 };
    expect(meteringDynamicRange({ samples, sampleRate, ...range })).toEqual(
      meteringDynamicRange(samples, sampleRate, range),
    );
    const spectrum = { nFft: 256, applyOctaveSmoothing: true, octaveFraction: 3 };
    expect(meteringSpectrum({ samples, sampleRate, ...spectrum })).toEqual(
      meteringSpectrum(samples, sampleRate, spectrum),
    );
    expect(meteringSpectrumFrame({ samples, sampleRate, startSample: 32, ...spectrum })).toEqual(
      meteringSpectrumFrame(samples, sampleRate, { startSample: 32, ...spectrum }),
    );
  });

  it('rejects a negative clipping-region length before unsigned conversion', () => {
    expect(() => meteringDetectClipping({ samples, sampleRate, minRegionSamples: -1 })).toThrow(
      RangeError,
    );
  });

  it('runs the stereo calls from a request and bounds the scope through maxPoints', () => {
    expect(Number.isFinite(meteringStereoCorrelation({ left, right, sampleRate }))).toBe(true);
    expect(Number.isFinite(meteringStereoWidth({ left, right, sampleRate }))).toBe(true);
    const full = meteringVectorscope({ left, right, sampleRate });
    expect(full.mid).toHaveLength(left.length);
    const bounded = meteringVectorscope({ left, right, sampleRate, maxPoints: 128 });
    expect(bounded.mid.length).toBeLessThanOrEqual(128);
    const phase = meteringPhaseScope({ left, right, sampleRate, maxPoints: 128 });
    expect(phase.mid.length).toBeLessThanOrEqual(128);
    expect(phase.correlation).toBeCloseTo(
      present(meteringPhaseScope({ left, right, sampleRate }).correlation, 'correlation'),
      6,
    );
  });

  it('preserves waveform calls over planar channels', () => {
    const report = waveformPeaks({ channels: [left, right], samplesPerBucket: 64 });
    expect(report.min).toHaveLength(2);
    expect(report.bucketCount).toBe(left.length / 64);
    const levels = waveformPeakPyramid({
      channels: [left, right],
      samplesPerBucketLevels: [64, 128],
    });
    expect(levels).toHaveLength(2);
    expect(levels[0]?.min[0]).toEqual(report.min[0]);
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posEmpty = captureThrow(() => meteringPeakDb(empty, sampleRate));
    const reqEmpty = captureThrow(() => meteringPeakDb({ samples: empty, sampleRate }));
    expect(posEmpty.threw).toBe(true);
    expect(reqEmpty.threw).toBe(true);
    expect(reqEmpty.message).toBe(posEmpty.message);

    const posFactor = captureThrow(() =>
      meteringTruePeakDbtp(samples, sampleRate, { oversampleFactor: 3 }),
    );
    const reqFactor = captureThrow(() =>
      meteringTruePeakDbtp({ samples, sampleRate, oversampleFactor: 3 }),
    );
    expect(posFactor.threw).toBe(true);
    expect(reqFactor.threw).toBe(true);
    expect(reqFactor.message).toBe(posFactor.message);

    const reqPair = captureThrow(() =>
      meteringStereoCorrelation({ left, right: right.subarray(1), sampleRate }),
    );
    expect(reqPair.threw).toBe(true);
  });
});

function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
  try {
    fn();
    return { threw: false, message: '' };
  } catch (error) {
    return { threw: true, message: error instanceof Error ? error.message : String(error) };
  }
}
