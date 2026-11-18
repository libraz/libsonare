import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
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
} from '../src/index';

const sampleRate = 22050;
const samples = new Float32Array(8192);
for (let i = 0; i < samples.length; i++) {
  samples[i] = 0.6 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
}
const right = samples.map((value, index) => value * (index % 3 === 0 ? -0.75 : 0.75));

function expectFloatArraysEqual(actual: Float32Array, expected: Float32Array): void {
  expect(actual).toHaveLength(expected.length);
  for (let i = 0; i < actual.length; i++) {
    expect(actual[i]).toBeCloseTo(expected[i] ?? 0, 6);
  }
}

describe('metering request object compatibility (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('matches positional single-channel meter calls', () => {
    expect(meteringPeakDb({ samples, sampleRate })).toBeCloseTo(
      meteringPeakDb(samples, sampleRate),
      6,
    );
    expect(meteringRmsDb({ samples, sampleRate })).toBeCloseTo(
      meteringRmsDb(samples, sampleRate),
      6,
    );
    expect(meteringCrestFactorDb({ samples, sampleRate })).toBeCloseTo(
      meteringCrestFactorDb(samples, sampleRate),
      6,
    );
    expect(meteringDcOffset({ samples, sampleRate })).toBeCloseTo(
      meteringDcOffset(samples, sampleRate),
      6,
    );
    expect(meteringTruePeakDbtp({ samples, sampleRate, oversampleFactor: 4 })).toBeCloseTo(
      meteringTruePeakDbtp(samples, sampleRate, { oversampleFactor: 4 }),
      6,
    );
  });

  it('matches positional clipping and dynamic-range calls', () => {
    const clippingOptions = { thresholdDb: -6, minRegionSamples: 2 };
    expect(meteringDetectClipping({ samples, sampleRate, ...clippingOptions })).toEqual(
      meteringDetectClipping(samples, sampleRate, clippingOptions),
    );
    const rangeOptions = {
      windowSec: 0.1,
      hopSec: 0.05,
      lowPercentile: 0.1,
      highPercentile: 0.9,
    };
    const requested = meteringDynamicRange({ samples, sampleRate, ...rangeOptions });
    const positional = meteringDynamicRange(samples, sampleRate, rangeOptions);
    expect(requested.dynamicRangeDb).toBeCloseTo(positional.dynamicRangeDb, 6);
    expect(requested.lowPercentileDb).toBeCloseTo(positional.lowPercentileDb, 6);
    expect(requested.highPercentileDb).toBeCloseTo(positional.highPercentileDb, 6);
    expectFloatArraysEqual(requested.windowRmsDb, positional.windowRmsDb);
  });

  it('runs the stereo meter and scope calls from a request', () => {
    expect(meteringStereoCorrelation({ left: samples, right, sampleRate })).toBeCloseTo(
      meteringStereoCorrelation({ left: samples, right, sampleRate }),
      6,
    );
    expect(Number.isFinite(meteringStereoWidth({ left: samples, right, sampleRate }))).toBe(true);
    const vectorscope = meteringVectorscope({ left: samples, right, sampleRate });
    expect(vectorscope.mid).toHaveLength(samples.length);
    const phaseScope = meteringPhaseScope({ left: samples, right, sampleRate });
    expect(phaseScope.mid).toHaveLength(samples.length);
    expect(Number.isFinite(phaseScope.correlation)).toBe(true);
  });

  it('bounds the scope point count through maxPoints and keeps full-resolution stats', () => {
    const maxPoints = 64;
    const vectorscope = meteringVectorscope({ left: samples, right, sampleRate, maxPoints });
    expect(vectorscope.mid.length).toBeLessThanOrEqual(maxPoints);
    const phaseScope = meteringPhaseScope({ left: samples, right, sampleRate, maxPoints });
    expect(phaseScope.radius.length).toBeLessThanOrEqual(maxPoints);
    const full = meteringPhaseScope({ left: samples, right, sampleRate });
    expect(phaseScope.correlation).toBeCloseTo(full.correlation, 6);
    // Omitting maxPoints stays full resolution (one point per input sample); 0 is refused.
    expect(meteringVectorscope({ left: samples, right, sampleRate }).mid.length).toBe(
      samples.length,
    );
    expect(() => meteringVectorscope({ left: samples, right, sampleRate, maxPoints: 0 })).toThrow(
      /maxPoints must not be 0/,
    );
  });

  it('matches positional spectrum calls', () => {
    const options = { nFft: 1024, applyOctaveSmoothing: true, octaveFraction: 3 };
    const spectrum = meteringSpectrum({ samples, sampleRate, ...options });
    const spectrumPositional = meteringSpectrum(samples, sampleRate, options);
    expectFloatArraysEqual(spectrum.magnitude, spectrumPositional.magnitude);
    expectFloatArraysEqual(spectrum.db, spectrumPositional.db);
    const frame = meteringSpectrumFrame({ samples, sampleRate, startSample: 128, ...options });
    const framePositional = meteringSpectrumFrame(samples, sampleRate, {
      startSample: 128,
      ...options,
    });
    expectFloatArraysEqual(frame.power, framePositional.power);
  });

  it('runs waveform bucket calls from planar requests', () => {
    const channels = [samples, right];
    const peaks = waveformPeaks({ channels, samplesPerBucket: 128 });
    expect(peaks.min).toHaveLength(2);
    expect(peaks.bucketCount).toBe(samples.length / 128);
    // Channel 0 carries the larger level.
    expect(Math.max(...(peaks.max[0] as Float32Array))).toBeGreaterThan(
      Math.max(...(peaks.max[1] as Float32Array)),
    );
    const pyramid = waveformPeakPyramid({
      channels,
      samplesPerBucketLevels: [128, 256],
    });
    expect(pyramid).toHaveLength(2);
    expect(pyramid[0]?.bucketCount).toBe(samples.length / 128);
    expect(pyramid[1]?.bucketCount).toBe(samples.length / 256);
    expectFloatArraysEqual(pyramid[0]?.min[0] as Float32Array, peaks.min[0] as Float32Array);
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
      meteringStereoCorrelation({ left: samples, right: right.subarray(1), sampleRate }),
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
