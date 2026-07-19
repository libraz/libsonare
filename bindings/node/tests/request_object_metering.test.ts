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
  meteringTruePeakDb,
  meteringVectorscope,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index.js';

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
    expect(meteringTruePeakDb({ samples, sampleRate, oversampleFactor: 2 })).toEqual(
      meteringTruePeakDb(samples, sampleRate, 2),
    );
  });

  it('preserves report and spectrum options', () => {
    const clipping = { threshold: 0.4, minRegionSamples: 2 };
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
    expect(meteringSpectrumFrame({ samples, sampleRate, frameOffset: 32, ...spectrum })).toEqual(
      meteringSpectrumFrame(samples, sampleRate, 32, spectrum),
    );
  });

  it('preserves all stereo calls', () => {
    expect(meteringStereoCorrelation({ left, right, sampleRate })).toEqual(
      meteringStereoCorrelation(left, right, sampleRate),
    );
    expect(meteringStereoWidth({ left, right, sampleRate })).toEqual(
      meteringStereoWidth(left, right, sampleRate),
    );
    const scope = { maxPoints: 128 };
    expect(meteringVectorscope({ left, right, sampleRate, ...scope })).toEqual(
      meteringVectorscope(left, right, sampleRate, scope),
    );
    expect(meteringPhaseScope({ left, right, sampleRate, ...scope })).toEqual(
      meteringPhaseScope(left, right, sampleRate, scope),
    );
  });

  it('preserves waveform calls', () => {
    const interleaved = new Float32Array(samples.length * 2);
    for (let i = 0; i < samples.length; i++) {
      interleaved[2 * i] = left[i] ?? 0;
      interleaved[2 * i + 1] = right[i] ?? 0;
    }
    expect(waveformPeaks({ samples: interleaved, channels: 2, samplesPerBucket: 64 })).toEqual(
      waveformPeaks(interleaved, 2, { samplesPerBucket: 64 }),
    );
    const levels = [64, 128];
    expect(
      waveformPeakPyramid({ samples: interleaved, channels: 2, samplesPerBucketLevels: levels }),
    ).toEqual(waveformPeakPyramid(interleaved, 2, { samplesPerBucketLevels: levels }));
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

    const posFactor = captureThrow(() => meteringTruePeakDb(samples, sampleRate, 3));
    const reqFactor = captureThrow(() =>
      meteringTruePeakDb({ samples, sampleRate, oversampleFactor: 3 }),
    );
    expect(posFactor.threw).toBe(true);
    expect(reqFactor.threw).toBe(true);
    expect(reqFactor.message).toBe(posFactor.message);

    const posPair = captureThrow(() =>
      meteringStereoCorrelation(left, right.subarray(1), sampleRate),
    );
    const reqPair = captureThrow(() =>
      meteringStereoCorrelation({ left, right: right.subarray(1), sampleRate }),
    );
    expect(posPair.threw).toBe(true);
    expect(reqPair.threw).toBe(true);
    expect(reqPair.message).toBe(posPair.message);
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
