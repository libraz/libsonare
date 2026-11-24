import { describe, expect, it } from 'vitest';
import { masteringAnalyzePair, masteringAnalyzeStereo } from '../src/index.js';

const SAMPLE_RATE = 22050;
const DUPLICATE_LOUDNESS_DB = 10 * Math.log10(2);

function signal(frequency: number, amplitude: number, sampleRate = SAMPLE_RATE): Float32Array {
  const samples = new Float32Array(sampleRate);
  for (let index = 0; index < samples.length; index++) {
    samples[index] = amplitude * Math.sin((2 * Math.PI * frequency * index) / sampleRate);
  }
  return samples;
}

function shifted(samples: Float32Array, delay: number): Float32Array {
  const result = new Float32Array(samples.length);
  result.set(samples.subarray(0, samples.length - delay), delay);
  return result;
}

function expectClose(actual: number, expected: number): void {
  if (!Number.isFinite(actual) || !Number.isFinite(expected)) {
    expect(actual).toBe(expected);
    return;
  }
  const tolerance = 2e-5 * Math.max(1, Math.abs(actual), Math.abs(expected));
  expect(Math.abs(actual - expected)).toBeLessThanOrEqual(tolerance);
}

function expectOptionalClose(actual: number | undefined, expected: number | undefined): void {
  expect(actual === undefined).toBe(expected === undefined);
  if (actual !== undefined && expected !== undefined) {
    expectClose(actual, expected);
  }
}

function expectNamedError(
  run: () => unknown,
  errorType: typeof TypeError | typeof RangeError,
  subject: string,
): void {
  let caught: unknown;
  try {
    run();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(errorType);
  expect((caught as Error).message).toContain(subject);
}

describe('planar mastering pair analysis', () => {
  const source = signal(220, 0.2);
  const reference = shifted(signal(440, 0.35), 37);
  const monoRequest = { source, reference, sampleRate: SAMPLE_RATE };

  it('matches mono for all pair analyses when both channels duplicate the mono input', () => {
    const loudness = masteringAnalyzePair({
      ...monoRequest,
      analysis: 'match.referenceLoudness',
    });
    const stereoLoudness = masteringAnalyzePair({
      ...monoRequest,
      source: [source, source],
      reference: [reference, reference],
      analysis: 'match.referenceLoudness',
    });
    expectClose(stereoLoudness.sourceLufs, loudness.sourceLufs + DUPLICATE_LOUDNESS_DB);
    expectClose(stereoLoudness.referenceLufs, loudness.referenceLufs + DUPLICATE_LOUDNESS_DB);
    expectOptionalClose(stereoLoudness.gainToMatchDb, loudness.gainToMatchDb);

    const compareBands = (analysis: 'match.tonalBalance' | 'match.tonalBalanceLogBands'): void => {
      const mono = masteringAnalyzePair({ ...monoRequest, analysis });
      const stereo = masteringAnalyzePair({
        ...monoRequest,
        source: [source, source],
        reference: [reference, reference],
        analysis,
      });
      expect(stereo.bands).toHaveLength(mono.bands.length);
      for (let index = 0; index < mono.bands.length; index++) {
        const monoBand = mono.bands[index];
        const stereoBand = stereo.bands[index];
        expect(stereoBand?.lowHz).toBe(monoBand?.lowHz);
        expect(stereoBand?.highHz).toBe(monoBand?.highHz);
        expectOptionalClose(stereoBand?.sourceDb, monoBand?.sourceDb);
        expectOptionalClose(stereoBand?.referenceDb, monoBand?.referenceDb);
        expectOptionalClose(stereoBand?.deviationDb, monoBand?.deviationDb);
      }
    };
    compareBands('match.tonalBalance');
    compareBands('match.tonalBalanceLogBands');

    const monoCurve = masteringAnalyzePair({ ...monoRequest, analysis: 'match.matchEqCurve' });
    const stereoCurve = masteringAnalyzePair({
      ...monoRequest,
      source: [source, source],
      reference: [reference, reference],
      analysis: 'match.matchEqCurve',
    });
    expect(stereoCurve.frequencies).toHaveLength(monoCurve.frequencies.length);
    expect(stereoCurve.gainDb).toHaveLength(monoCurve.gainDb.length);
    stereoCurve.frequencies.forEach((value, index) => {
      expectClose(value, monoCurve.frequencies[index] as number);
    });
    stereoCurve.gainDb.forEach((value, index) => {
      expectClose(value, monoCurve.gainDb[index] as number);
    });

    const monoDelay = masteringAnalyzePair({
      ...monoRequest,
      analysis: 'match.estimateReferenceDelaySamples',
    });
    const stereoDelay = masteringAnalyzePair({
      ...monoRequest,
      source: [source, source],
      reference: [reference, reference],
      analysis: 'match.estimateReferenceDelaySamples',
    });
    expectClose(stereoDelay.delaySamples, monoDelay.delaySamples);
  });

  it('accepts one-element planar inputs as mono', () => {
    expect(
      masteringAnalyzePair({
        ...monoRequest,
        source: [source],
        reference: [reference],
        analysis: 'match.referenceLoudness',
      }),
    ).toEqual(masteringAnalyzePair({ ...monoRequest, analysis: 'match.referenceLoudness' }));
  });

  it('refuses empty and over-wide channel lists by name', () => {
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: [],
          analysis: 'match.referenceLoudness',
        } as never),
      RangeError,
      'source',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          reference: [],
          analysis: 'match.referenceLoudness',
        } as never),
      RangeError,
      'reference',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: [source, source, source],
          analysis: 'match.referenceLoudness',
        } as never),
      RangeError,
      'source',
    );
  });

  it('refuses mismatched channel counts and unequal planes by name', () => {
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: [source, source],
          analysis: 'match.referenceLoudness',
        }),
      RangeError,
      'source and reference must have the same number of channels',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: [source, source.subarray(1)],
          analysis: 'match.referenceLoudness',
        }),
      RangeError,
      'source[1] must have the same length as source[0]',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          reference: [reference, reference.subarray(1)],
          analysis: 'match.referenceLoudness',
        }),
      RangeError,
      'reference[1] must have the same length as reference[0]',
    );
  });

  it('refuses wrong buffer and planar element types by name', () => {
    const sparseSource = new Array<Float32Array>(2);
    sparseSource[0] = source;
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: new Float64Array(source.length),
          analysis: 'match.referenceLoudness',
        } as never),
      TypeError,
      'source',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          reference: [reference, new Float64Array(reference.length)],
          analysis: 'match.referenceLoudness',
        } as never),
      TypeError,
      'reference[1]',
    );
    expectNamedError(
      () =>
        masteringAnalyzePair({
          ...monoRequest,
          source: sparseSource,
          analysis: 'match.referenceLoudness',
        } as never),
      TypeError,
      'source[1]',
    );
  });

  it('names stereo log-band params from the caller and keeps their defaults', () => {
    const sampleRate = 48000;
    const left = signal(220, 0.2, sampleRate);
    const right = signal(440, 0.35, sampleRate);
    const request = {
      analysis: 'stereo.monoCompatCheckLogBands' as const,
      left,
      right,
      sampleRate,
    };

    const defaults = masteringAnalyzeStereo(request);
    const spelledDefaults = masteringAnalyzeStereo({
      ...request,
      params: { bandsPerOctave: 3, lowHz: 20, highHz: 20000 },
    });
    expect(defaults.bands).toHaveLength(spelledDefaults.bands.length);
    defaults.bands.forEach((band, index) => {
      const spelledBand = spelledDefaults.bands[index];
      expect(spelledBand).toBeDefined();
      if (!spelledBand) {
        return;
      }
      expectClose(band.lowHz, spelledBand.lowHz);
      expectClose(band.highHz, spelledBand.highHz);
      expectOptionalClose(band.correlation, spelledBand.correlation);
      expectClose(band.sideRms, spelledBand.sideRms);
    });

    const nearBoundary = masteringAnalyzeStereo({
      ...request,
      params: { bandsPerOctave: 1, lowHz: 1, highHz: sampleRate / 2 },
    });
    expect(nearBoundary.bands.length).toBeGreaterThan(0);

    expectNamedError(
      () =>
        masteringAnalyzeStereo({
          ...request,
          params: { bandsPerOctave: 0 },
        }),
      RangeError,
      'params.bandsPerOctave',
    );
    expectNamedError(
      () =>
        masteringAnalyzeStereo({
          ...request,
          params: { lowHz: 0 },
        }),
      RangeError,
      'params.lowHz',
    );
    expectNamedError(
      () =>
        masteringAnalyzeStereo({
          ...request,
          params: { highHz: 0 },
        }),
      RangeError,
      'params.highHz',
    );
  });
});
