import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringPhaseScope,
  meteringPhaseScopeDecimated,
  meteringRmsDb,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDb,
  meteringVectorscope,
  meteringVectorscopeDecimated,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index';

const sampleRate = 22050;
const samples = new Float32Array(8192);
for (let i = 0; i < samples.length; i++) {
  samples[i] = 0.6 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
}
const right = samples.map((value, index) => value * (index % 3 === 0 ? -0.75 : 0.75));
const interleaved = new Float32Array(samples.length * 2);
for (let i = 0; i < samples.length; i++) {
  interleaved[i * 2] = samples[i] ?? 0;
  interleaved[i * 2 + 1] = right[i] ?? 0;
}

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
    const options = { validate: true };
    expect(meteringPeakDb({ samples, sampleRate, ...options })).toBeCloseTo(
      meteringPeakDb(samples, sampleRate, options),
      6,
    );
    expect(meteringRmsDb({ samples, sampleRate, ...options })).toBeCloseTo(
      meteringRmsDb(samples, sampleRate, options),
      6,
    );
    expect(meteringCrestFactorDb({ samples, sampleRate, ...options })).toBeCloseTo(
      meteringCrestFactorDb(samples, sampleRate, options),
      6,
    );
    expect(meteringDcOffset({ samples, sampleRate, ...options })).toBeCloseTo(
      meteringDcOffset(samples, sampleRate, options),
      6,
    );
    expect(
      meteringTruePeakDb({ samples, sampleRate, oversampleFactor: 4, ...options }),
    ).toBeCloseTo(meteringTruePeakDb(samples, sampleRate, 4, options), 6);
  });

  it('matches positional clipping and dynamic-range calls', () => {
    const clippingOptions = { threshold: 0.5, minRegionSamples: 2, validate: true };
    expect(meteringDetectClipping({ samples, sampleRate, ...clippingOptions })).toEqual(
      meteringDetectClipping(samples, sampleRate, clippingOptions),
    );
    const rangeOptions = {
      windowSec: 0.1,
      hopSec: 0.05,
      lowPercentile: 0.1,
      highPercentile: 0.9,
      validate: true,
    };
    const requested = meteringDynamicRange({ samples, sampleRate, ...rangeOptions });
    const positional = meteringDynamicRange(samples, sampleRate, rangeOptions);
    expect(requested.dynamicRangeDb).toBeCloseTo(positional.dynamicRangeDb, 6);
    expect(requested.lowPercentileDb).toBeCloseTo(positional.lowPercentileDb, 6);
    expect(requested.highPercentileDb).toBeCloseTo(positional.highPercentileDb, 6);
    expectFloatArraysEqual(requested.windowRmsDb, positional.windowRmsDb);
  });

  it('matches positional stereo meter and scope calls', () => {
    const options = { validate: true };
    expect(meteringStereoCorrelation({ left: samples, right, sampleRate, ...options })).toBeCloseTo(
      meteringStereoCorrelation(samples, right, sampleRate, options),
      6,
    );
    expect(meteringStereoWidth({ left: samples, right, sampleRate, ...options })).toBeCloseTo(
      meteringStereoWidth(samples, right, sampleRate, options),
      6,
    );
    const vectorscope = meteringVectorscope({ left: samples, right, sampleRate, ...options });
    const vectorscopePositional = meteringVectorscope(samples, right, sampleRate, options);
    expectFloatArraysEqual(vectorscope.mid, vectorscopePositional.mid);
    expectFloatArraysEqual(vectorscope.side, vectorscopePositional.side);
    const phaseScope = meteringPhaseScope({ left: samples, right, sampleRate, ...options });
    const phaseScopePositional = meteringPhaseScope(samples, right, sampleRate, options);
    expectFloatArraysEqual(phaseScope.mid, phaseScopePositional.mid);
    expect(phaseScope.correlation).toBeCloseTo(phaseScopePositional.correlation, 6);
  });

  it('matches positional decimated scopes', () => {
    const request = { left: samples, right, sampleRate, maxPoints: 64, validate: true };
    const vectorscope = meteringVectorscopeDecimated(request);
    const vectorscopePositional = meteringVectorscopeDecimated(samples, right, sampleRate, 64, {
      validate: true,
    });
    expectFloatArraysEqual(vectorscope.mid, vectorscopePositional.mid);
    expectFloatArraysEqual(vectorscope.side, vectorscopePositional.side);
    const phaseScope = meteringPhaseScopeDecimated(request);
    const phaseScopePositional = meteringPhaseScopeDecimated(samples, right, sampleRate, 64, {
      validate: true,
    });
    expectFloatArraysEqual(phaseScope.radius, phaseScopePositional.radius);
    expect(phaseScope.correlation).toBeCloseTo(phaseScopePositional.correlation, 6);
  });

  it('folds maxPoints into the primary scope functions (Node-shape parity)', () => {
    const maxPoints = 64;
    // The primary function with maxPoints must bound the point count and match
    // the decimated variant exactly.
    const vectorscope = meteringVectorscope({ left: samples, right, sampleRate, maxPoints });
    expect(vectorscope.mid.length).toBeLessThanOrEqual(maxPoints);
    const vectorscopeDecimated = meteringVectorscopeDecimated({
      left: samples,
      right,
      sampleRate,
      maxPoints,
    });
    expectFloatArraysEqual(vectorscope.mid, vectorscopeDecimated.mid);
    expectFloatArraysEqual(vectorscope.side, vectorscopeDecimated.side);

    const phaseScope = meteringPhaseScope(samples, right, sampleRate, { maxPoints });
    expect(phaseScope.radius.length).toBeLessThanOrEqual(maxPoints);
    const phaseScopeDecimated = meteringPhaseScopeDecimated({
      left: samples,
      right,
      sampleRate,
      maxPoints,
    });
    expectFloatArraysEqual(phaseScope.radius, phaseScopeDecimated.radius);

    // Omitting maxPoints stays full resolution (one point per input sample).
    const full = meteringVectorscope({ left: samples, right, sampleRate });
    expect(full.mid.length).toBe(samples.length);
  });

  it('matches positional spectrum calls', () => {
    const options = { nFft: 1024, applyOctaveSmoothing: true, octaveFraction: 3, validate: true };
    const spectrum = meteringSpectrum({ samples, sampleRate, ...options });
    const spectrumPositional = meteringSpectrum(samples, sampleRate, options);
    expectFloatArraysEqual(spectrum.magnitude, spectrumPositional.magnitude);
    expectFloatArraysEqual(spectrum.db, spectrumPositional.db);
    const frame = meteringSpectrumFrame({ samples, sampleRate, frameOffset: 128, ...options });
    const framePositional = meteringSpectrumFrame(samples, sampleRate, 128, options);
    expectFloatArraysEqual(frame.power, framePositional.power);
  });

  it('matches positional waveform bucket calls', () => {
    const peaksOptions = { samplesPerBucket: 128, validate: true };
    const peaks = waveformPeaks({ samples: interleaved, channels: 2, ...peaksOptions });
    const peaksPositional = waveformPeaks(interleaved, 2, peaksOptions);
    expectFloatArraysEqual(peaks.min, peaksPositional.min);
    expectFloatArraysEqual(peaks.max, peaksPositional.max);
    const pyramidOptions = { samplesPerBucketLevels: [128, 256], validate: true };
    const pyramid = waveformPeakPyramid({ samples: interleaved, channels: 2, ...pyramidOptions });
    const pyramidPositional = waveformPeakPyramid(interleaved, 2, pyramidOptions);
    expect(pyramid).toHaveLength(pyramidPositional.length);
    for (let i = 0; i < pyramid.length; i++) {
      const level = pyramid[i];
      const positionalLevel = pyramidPositional[i];
      if (!level || !positionalLevel) {
        throw new Error('expected matching waveform peak pyramid level');
      }
      expectFloatArraysEqual(level.min, positionalLevel.min);
      expectFloatArraysEqual(level.max, positionalLevel.max);
    }
  });
});
