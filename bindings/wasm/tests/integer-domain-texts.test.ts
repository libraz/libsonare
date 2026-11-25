/**
 * A refusal states the domain the C ABI checks for that parameter: an int capped at the
 * 32-bit limit reads "an integer in [lo, hi]", a 64-bit count with no ceiling "of at least N".
 * The texts match the Node addon, which inherits them from the C entries.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ClipPageProvider, init, RealtimeEngine } from '../dist/index.js';

const engine = () => new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

const cases: ReadonlyArray<readonly [string, () => unknown, string]> = [
  [
    'a time signature numerator of 0',
    () => engine().setTimeSignature(0, 4),
    'RealtimeEngine.setTimeSignature: numerator must be an integer in [1, 2147483647]',
  ],
  [
    'a negative time signature denominator',
    () => engine().setTimeSignature(4, -3),
    'RealtimeEngine.setTimeSignature: denominator must be an integer in [1, 2147483647]',
  ],
  [
    'a segment denominator of 0, named by its index',
    () =>
      engine().setTimeSignatureSegments([
        { startPpq: 0, numerator: 4, denominator: 4 },
        { startPpq: 10, numerator: 3, denominator: 0 },
      ]),
    'RealtimeEngine.setTimeSignatureSegments: segments[1].denominator must be an integer in [1, 2147483647]',
  ],
  [
    'a segment numerator of 0',
    () => engine().setTimeSignatureSegments([{ startPpq: 0, numerator: 0, denominator: 4 }]),
    'RealtimeEngine.setTimeSignatureSegments: segments[0].numerator must be an integer in [1, 2147483647]',
  ],
  [
    'a segment start before the timeline',
    () => engine().setTimeSignatureSegments([{ startPpq: -1, numerator: 4, denominator: 4 }]),
    'RealtimeEngine.setTimeSignatureSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a tempo segment start above the public PPQ maximum',
    () => engine().setTempoSegments([{ startPpq: 1e12 + 1, bpm: 120 }]),
    'RealtimeEngine.setTempoSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a negative fractional tempo segment start',
    () => engine().setTempoSegments([{ startPpq: -0.5, bpm: 120 }]),
    'RealtimeEngine.setTempoSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a NaN tempo segment start',
    () => engine().setTempoSegments([{ startPpq: Number.NaN, bpm: 120 }]),
    'RealtimeEngine.setTempoSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a non-finite tempo segment start',
    () => engine().setTempoSegments([{ startPpq: Number.POSITIVE_INFINITY, bpm: 120 }]),
    'RealtimeEngine.setTempoSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'an unrepresentable tempo segment start',
    () => engine().setTempoSegments([{ startPpq: Number.MAX_VALUE, bpm: 120 }]),
    'RealtimeEngine.setTempoSegments: segments[0].startPpq must be finite and in [0, 1e12]',
  ],
  [
    'a tempo segment of 0 bpm',
    () => engine().setTempoSegments([{ startPpq: 0, bpm: 0 }]),
    'RealtimeEngine.setTempoSegments: segments[0].bpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a sub-floor tempo segment',
    () => engine().setTempoSegments([{ startPpq: 0, bpm: 1e-8 }]),
    'RealtimeEngine.setTempoSegments: segments[0].bpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a negative tempo segment end, named by its index',
    () =>
      engine().setTempoSegments([
        { startPpq: 0, bpm: 120 },
        { startPpq: 10, bpm: 120, endBpm: -1 },
      ]),
    'RealtimeEngine.setTempoSegments: segments[1].endBpm must be finite and in [1e-6, 100000]',
  ],
  [
    'a clip page provider with no channels',
    () => new ClipPageProvider({ numChannels: 0, numSamples: 100, pageFrames: 10 }),
    'ClipPageProvider: numChannels must be an integer in [1, 64]',
  ],
  [
    'a clip page provider with no samples',
    () => new ClipPageProvider({ numChannels: 1, numSamples: 0, pageFrames: 10 }),
    'ClipPageProvider: numSamples must be an integer of at least 1',
  ],
  [
    'a clip page provider with no page frames',
    () => new ClipPageProvider({ numChannels: 1, numSamples: 100, pageFrames: 0 }),
    'ClipPageProvider: pageFrames must be an integer in [1, 16777216]',
  ],
  [
    'a clip page provider spanning too many pages',
    () => new ClipPageProvider({ numChannels: 1, numSamples: 1e12, pageFrames: 1 }),
    'ClipPageProvider: numSamples must span at most 1048576 pages of pageFrames frames',
  ],
];

describe('integer refusals state the C domain', () => {
  beforeAll(async () => {
    await init();
  });

  it.each(cases)('refuses %s', (_label, run, text) => {
    const error = caught(run);
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(text);
  });
  it('accepts a fractional tempo segment position', () => {
    expect(() => engine().setTempoSegments([{ startPpq: 0.5, bpm: 120 }])).not.toThrow();
  });
});
