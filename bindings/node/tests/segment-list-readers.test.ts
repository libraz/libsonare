/**
 * The tempo and time-signature segment lists of a project and an engine are read alike: a
 * non-object element, an absent field and a wrong type are refused as TypeError, a value outside
 * its domain as RangeError, and every refusal names the element by its index.
 */

import { describe, expect, it } from 'vitest';
import { Project, RealtimeEngine } from '../dist/index.js';

type Segment = Record<string, unknown> | number;
interface Target {
  tempo(segments: Segment[]): unknown;
  signatures(segments: Segment[]): unknown;
  tempoEntry: string;
  signatureEntry: string;
}

const project = (): Target => {
  const p = new Project();
  return {
    tempo: (segments) => p.setTempoSegments(segments as never),
    signatures: (segments) => p.setTimeSignatures(segments as never),
    tempoEntry: 'Project.setTempoSegments',
    signatureEntry: 'Project.setTimeSignatures',
  };
};
const engine = (): Target => {
  const e = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  return {
    tempo: (segments) => e.setTempoSegments(segments as never),
    signatures: (segments) => e.setTimeSignatureSegments(segments as never),
    tempoEntry: 'RealtimeEngine.setTempoSegments',
    signatureEntry: 'RealtimeEngine.setTimeSignatureSegments',
  };
};

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

const tempo = { startPpq: 0, bpm: 120 };
const signature = { startPpq: 0, numerator: 4, denominator: 4 };

const tempoCases: ReadonlyArray<readonly [string, Segment[], typeof TypeError, string]> = [
  ['a non-object element', [tempo, 120], TypeError, 'segments[1] must be an object'],
  ['an absent start', [{ bpm: 120 }], TypeError, 'segments[0].startPpq must be a number'],
  ['an absent tempo', [{ startPpq: 0 }], TypeError, 'segments[0].bpm must be a number'],
  [
    'a string end tempo',
    [{ ...tempo, endBpm: '90' }],
    TypeError,
    'segments[0].endBpm must be a number',
  ],
  [
    'a negative start',
    [{ startPpq: -0.5, bpm: 120 }],
    RangeError,
    'segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a start above the public maximum',
    [{ startPpq: 1e12 + 1, bpm: 120 }],
    RangeError,
    'segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a very large start',
    [{ startPpq: 1e100, bpm: 120 }],
    RangeError,
    'segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'an infinite start',
    [{ startPpq: Number.POSITIVE_INFINITY, bpm: 120 }],
    RangeError,
    'segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a NaN start',
    [{ startPpq: Number.NaN, bpm: 120 }],
    RangeError,
    'segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a tempo of 0 at the second element',
    [tempo, { startPpq: 960, bpm: 0 }],
    RangeError,
    'segments[1].bpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a sub-floor tempo at the second element',
    [tempo, { startPpq: 960, bpm: 1e-8 }],
    RangeError,
    'segments[1].bpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a sub-floor nonzero ramp target',
    [{ ...tempo, endBpm: 1e-8 }],
    RangeError,
    'segments[0].endBpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a negative ramp target',
    [{ ...tempo, endBpm: -1 }],
    RangeError,
    'segments[0].endBpm must be finite and in [1e-6, 100000]',
  ],
];

const signatureCases: ReadonlyArray<readonly [string, Segment[], typeof TypeError, string]> = [
  ['a non-object element', [signature, 4], TypeError, 'segments[1] must be an object'],
  [
    'an absent start',
    [{ numerator: 4, denominator: 4 }],
    TypeError,
    'segments[0].startPpq must be a number',
  ],
  [
    'an absent numerator',
    [{ startPpq: 0, denominator: 4 }],
    TypeError,
    'segments[0].numerator must be an integer',
  ],
  [
    'an absent denominator',
    [{ startPpq: 0, numerator: 4 }],
    TypeError,
    'segments[0].denominator must be an integer',
  ],
  [
    'a fractional numerator at the second element',
    [signature, { startPpq: 960, numerator: 4.5, denominator: 4 }],
    RangeError,
    'segments[1].numerator must be an integer in [1, 2147483647]',
  ],
  [
    'a denominator of 0',
    [{ startPpq: 0, numerator: 4, denominator: 0 }],
    RangeError,
    'segments[0].denominator must be an integer in [1, 2147483647]',
  ],
];

describe.each([
  ['a project', project],
  ['an engine', engine],
])('the segment lists of %s', (_name, make) => {
  it.each(tempoCases)('refuse a tempo list with %s', (_case, segments, errorClass, text) => {
    const target = make();
    const error = caught(() => target.tempo(segments));
    expect(error).toBeInstanceOf(errorClass);
    expect(error.message).toBe(`${target.tempoEntry}: ${text}`);
  });

  it.each(signatureCases)(
    'refuse a signature list with %s',
    (_case, segments, errorClass, text) => {
      const target = make();
      const error = caught(() => target.signatures(segments));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`${target.signatureEntry}: ${text}`);
    },
  );

  it('accept a fractional PPQ start within the public domain', () => {
    const target = make();
    expect(() => target.tempo([{ startPpq: 0.5, bpm: 120 }])).not.toThrow();
  });

  it('accept a list with an omitted end tempo', () => {
    const target = make();
    expect(() =>
      target.tempo([
        { ...tempo, bpm: 1e-6, endBpm: 1e-6 },
        { startPpq: 960, bpm: 90, endBpm: 0 },
        { startPpq: 1920, bpm: 90 },
      ]),
    ).not.toThrow();
    expect(() =>
      target.signatures([signature, { startPpq: 960, numerator: 3, denominator: 4 }]),
    ).not.toThrow();
  });
});
