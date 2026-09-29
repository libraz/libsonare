/**
 * Every field this pass moved off a silently-substituting reader now refuses a
 * present, wrong-typed value by name instead of silently keeping the default.
 *
 * `onset-options-reader-agreement.test.ts` already carries the fuller,
 * cross-surface, "the input has to change the answer" proof for
 * `detectOnsets`'s `preMax`/`postMax`/`wait` (and the pre-existing
 * `threshold`/`delta`). This file is the simpler, table-driven check the
 * finding's own acceptance criterion asks for -- one wrong-typed value per
 * key, asserting the refusal -- for the remaining fields across every bag
 * this pass touched, none of which shares `detectOnsets`'s fixture.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init as wasmInit } from '../../wasm/dist/index.js';
import {
  analyze,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  StreamAnalyzer,
  spectralEdit,
} from '../src/index.js';

const SAMPLE_RATE = 22050;
const tone = new Float32Array(SAMPLE_RATE).map((_, i) =>
  Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE),
);

beforeAll(async () => {
  await wasmInit();
});

/** One case: a call taking `{ [key]: value }` merged into its options bag. */
interface Case {
  bag: string;
  key: string;
  /** A present value of a type the field never accepts. */
  wrongValue: unknown;
  run: (options: Record<string, unknown>) => unknown;
}

const CASES: Case[] = [
  // analyze / ReadMusicAnalyzeOptions -- every field but meterCandidateNumerators
  // (an array, checked separately below).
  ...(
    [
      'bpmMin',
      'bpmMax',
      'startBpm',
      'chromaHighpassHz',
      'chromaHopMultiplier',
      'chordHmmBeamWidth',
      'tempoUpdateIntervalBeats',
      'meterDenominator',
      'tuning',
    ] as const
  ).map((key) => ({
    bag: 'analyze',
    key,
    wrongValue: `${key}`,
    run: (options: Record<string, unknown>) => analyze(tone, SAMPLE_RATE, options),
  })),
  ...(
    [
      'useTriadsOnly',
      'useHpss',
      'useBassWeighted',
      'useChordHmm',
      'useChordKeyContext',
      'detectChordInversions',
      'adaptiveTempo',
      'computeTempoCurve',
    ] as const
  ).map((key) => ({
    bag: 'analyze',
    key,
    wrongValue: 'true',
    run: (options: Record<string, unknown>) => analyze(tone, SAMPLE_RATE, options),
  })),

  // detectOnsets -- preMax/postMax/wait are covered, with the fuller proof, by
  // onset-options-reader-agreement.test.ts.
  ...(['preAvg', 'postAvg', 'backtrackRange'] as const).map((key) => ({
    bag: 'detectOnsets',
    key,
    wrongValue: `${key}`,
    run: (options: Record<string, unknown>) => detectOnsets(tone, SAMPLE_RATE, options),
  })),

  // detectKey / detectKeyCandidates -- two entry points sharing one options
  // reader, both fixed in the same pass.
  ...(['detectKey', 'detectKeyCandidates'] as const).flatMap((bag) => {
    const run =
      bag === 'detectKey'
        ? (options: Record<string, unknown>) => detectKey(tone, SAMPLE_RATE, options)
        : (options: Record<string, unknown>) => detectKeyCandidates(tone, SAMPLE_RATE, options);
    return [
      { bag, key: 'useHpss', wrongValue: 'true', run },
      { bag, key: 'loudnessWeighted', wrongValue: 'true', run },
      { bag, key: 'highPassHz', wrongValue: 'highPassHz', run },
    ];
  }),

  // spectralEdit -- fields on the per-region op object, not the top-level
  // config; startSample/gainDb were already strict before this pass.
  ...(['endSample', 'lowHz', 'highHz'] as const).map((key) => ({
    bag: 'spectralEdit',
    key,
    wrongValue: `${key}`,
    run: (opFields: Record<string, unknown>) =>
      spectralEdit(tone, SAMPLE_RATE, [{ startSample: 0, endSample: 1024, ...opFields }]),
  })),

  // StreamAnalyzer construction config.
  ...(
    ['nMels', 'fmin', 'fmax', 'tuningRefHz', 'emitEveryNFrames', 'magnitudeDownsample'] as const
  ).map((key) => ({
    bag: 'StreamAnalyzer',
    key,
    wrongValue: `${key}`,
    run: (options: Record<string, unknown>) => new StreamAnalyzer(options),
  })),
  ...(['computeMel', 'computeChroma', 'computeOnset', 'computeSpectral'] as const).map((key) => ({
    bag: 'StreamAnalyzer',
    key,
    wrongValue: 'true',
    run: (options: Record<string, unknown>) => new StreamAnalyzer(options),
  })),
];

describe.each(CASES)('$bag refuses a wrong-typed $key', ({ key, wrongValue, run }) => {
  it('refuses the wrong-typed value by name', () => {
    expect(() => run({ [key]: wrongValue })).toThrowError(new RegExp(`\\b${key}\\b`));
  });

  it('still takes the default when the field is omitted', () => {
    expect(() => run({})).not.toThrow();
  });
});

describe('analyze meterCandidateNumerators refuses a non-array value', () => {
  it('throws naming the field for a present non-array value', () => {
    expect(() =>
      analyze(tone, SAMPLE_RATE, { meterCandidateNumerators: 'meterCandidateNumerators' }),
    ).toThrowError(/meterCandidateNumerators/);
  });

  it('still takes the default when omitted', () => {
    expect(() => analyze(tone, SAMPLE_RATE, {})).not.toThrow();
  });

  it('still accepts a legal array', () => {
    expect(() => analyze(tone, SAMPLE_RATE, { meterCandidateNumerators: [3, 4, 6] })).not.toThrow();
  });
});
