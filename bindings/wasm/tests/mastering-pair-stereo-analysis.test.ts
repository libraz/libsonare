import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringAnalyzePair } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const SAMPLE_RATE = 22050;
const SAMPLE_COUNT = SAMPLE_RATE;
const LOUDNESS_PAIR_OFFSET_DB = 10 * Math.log10(2);

function makePair(): { source: Float32Array; reference: Float32Array } {
  const source = new Float32Array(SAMPLE_COUNT);
  const reference = new Float32Array(SAMPLE_COUNT);
  for (let i = 0; i < SAMPLE_COUNT; i++) {
    source[i] =
      0.2 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE) +
      0.11 * Math.sin((2 * Math.PI * 733 * i) / SAMPLE_RATE) +
      0.04 * Math.sin((2 * Math.PI * 3011 * i) / SAMPLE_RATE);
  }
  for (let i = 19; i < SAMPLE_COUNT; i++) {
    reference[i] = 0.5 * source[i - 19];
  }
  return { source, reference };
}

function expectEveryMeasuredFieldClose(actual: unknown, expected: unknown, path = 'result'): void {
  if (typeof expected === 'number') {
    expect(typeof actual, path).toBe('number');
    const measured = actual as number;
    if (!Number.isFinite(expected)) {
      expect(measured, path).toBe(expected);
      return;
    }
    const tolerance = Math.max(1e-5, Math.abs(expected) * 1e-5);
    expect(Math.abs(measured - expected), path).toBeLessThanOrEqual(tolerance);
    return;
  }
  if (Array.isArray(expected)) {
    expect(actual, path).toBeInstanceOf(Array);
    const measured = actual as unknown[];
    expect(measured.length, `${path}.length`).toBe(expected.length);
    for (let index = 0; index < expected.length; index++) {
      expectEveryMeasuredFieldClose(measured[index], expected[index], `${path}[${index}]`);
    }
    return;
  }
  if (expected !== null && typeof expected === 'object') {
    expect(actual !== null && typeof actual === 'object', path).toBe(true);
    const measured = actual as Record<string, unknown>;
    const fields = Object.keys(expected as object).sort();
    expect(Object.keys(measured).sort(), `${path} fields`).toEqual(fields);
    for (const field of fields) {
      expectEveryMeasuredFieldClose(
        measured[field],
        (expected as Record<string, unknown>)[field],
        `${path}.${field}`,
      );
    }
    return;
  }
  expect(actual, path).toEqual(expected);
}

describe('masteringAnalyzePair planar stereo analysis', () => {
  const analyses = [
    'match.referenceLoudness',
    'match.tonalBalance',
    'match.tonalBalanceLogBands',
    'match.matchEqCurve',
    'match.estimateReferenceDelaySamples',
  ] as const;

  for (const analysis of analyses) {
    it(`matches every measured field for duplicate-channel ${analysis}`, () => {
      const { source, reference } = makePair();
      const mono = masteringAnalyzePair({ analysis, source, reference, sampleRate: SAMPLE_RATE });
      const stereo = masteringAnalyzePair({
        analysis,
        source: [source, source],
        reference: [reference, reference],
        sampleRate: SAMPLE_RATE,
      });
      let expected: unknown = mono;
      if (analysis === 'match.referenceLoudness') {
        const loudness = mono as unknown as { sourceLufs: number; referenceLufs: number };
        expected = {
          ...loudness,
          sourceLufs: loudness.sourceLufs + LOUDNESS_PAIR_OFFSET_DB,
          referenceLufs: loudness.referenceLufs + LOUDNESS_PAIR_OFFSET_DB,
        };
      }
      expectEveryMeasuredFieldClose(stereo, expected);
    });
  }

  it('keeps a flat mono buffer compatible with a one-plane input', () => {
    const { source, reference } = makePair();
    const flat = masteringAnalyzePair({
      analysis: 'match.tonalBalance',
      source,
      reference,
      sampleRate: SAMPLE_RATE,
    });
    const onePlane = masteringAnalyzePair({
      analysis: 'match.tonalBalance',
      source: [source],
      reference: [reference],
      sampleRate: SAMPLE_RATE,
    });
    expect(onePlane).toEqual(flat);
  });

  it('refuses different source and reference channel counts by name', () => {
    const { source, reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: [source, source],
        reference: [reference],
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(
      new RangeError(
        'masteringAnalyzePair: source and reference must have the same number of channels',
      ),
    );
  });

  it('refuses more than two channels by name', () => {
    const { source, reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: [source, source, source],
        reference,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(new RangeError('masteringAnalyzePair: source must hold 1 or 2 channels'));
  });

  it('refuses an empty planar input by name', () => {
    const { reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: [],
        reference,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(new RangeError('masteringAnalyzePair: source must hold 1 or 2 channels'));
  });

  it('refuses unequal lengths within a planar input by name', () => {
    const { source, reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: [source, source.subarray(1)],
        reference,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(
      new RangeError('masteringAnalyzePair: source[1] must have the same length as source[0]'),
    );
  });

  it('refuses a wrong input shape by name as TypeError', () => {
    const { reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: {} as never,
        reference,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(
      new TypeError(
        'masteringAnalyzePair: source must be a Float32Array or an array of Float32Array',
      ),
    );
  });

  it('refuses a non-Float32Array plane by name as TypeError', () => {
    const { source, reference } = makePair();
    expect(() =>
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source: [source, new Uint8Array(source.length) as never],
        reference,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrowError(new TypeError('masteringAnalyzePair: source[1] must be a Float32Array'));
  });
});
