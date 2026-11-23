import { describe, expect, it } from 'vitest';
import { analyzeMelody, decomposeStems, decomposeStemsLinked, noteStretch } from '../src/index.js';

// An argument that would size a result past the library's limit is refused by name, before the
// large allocation, so each case passes a small input with one extreme argument.
const SR = 22050;
const ELEMENT_LIMIT = 500000000;

const sine = (count: number): Float32Array =>
  Float32Array.from({ length: count }, (_, i) => Math.fround(Math.sin(i * 0.05)));

function refusal(call: () => unknown): Error {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(RangeError);
  return caught as Error;
}

describe('an argument sizing a result past the work size limit', () => {
  it('analyzeMelody refuses hopLength by name', () => {
    const error = refusal(() =>
      analyzeMelody({
        samples: new Float32Array(220000),
        sampleRate: SR,
        f0MinHz: SR / 2040,
        f0MaxHz: SR / 2.1,
        frameLength: 2048,
        hopLength: 1,
        usePyin: true,
      }),
    );
    expect(error.message).toBe(
      `analyzeMelody: hopLength must keep the observation matrix within ${ELEMENT_LIMIT} elements`,
    );
  });

  it('decomposeStems refuses nComponents by name', () => {
    const samples = sine(8192);
    expect(
      refusal(() =>
        decomposeStems({
          samples,
          sampleRate: SR,
          nComponents: ELEMENT_LIMIT + 1,
          nFft: 4,
          hopLength: 1,
        }),
      ).message,
    ).toBe(
      `decomposeStems: nComponents must keep the component weights within ${ELEMENT_LIMIT} elements`,
    );
    expect(
      refusal(() =>
        decomposeStems({ samples, sampleRate: SR, nComponents: 100000, nFft: 4, hopLength: 1 }),
      ).message,
    ).toBe(
      `decomposeStems: nComponents must keep the component activations within ${ELEMENT_LIMIT} elements`,
    );
  });

  it('decomposeStemsLinked refuses nComponents by name', () => {
    const channels = [sine(8192), sine(8192)];
    expect(
      refusal(() =>
        decomposeStemsLinked({
          channels,
          sampleRate: SR,
          nComponents: ELEMENT_LIMIT + 1,
          nFft: 4,
          hopLength: 1,
        }),
      ).message,
    ).toBe(
      `decomposeStemsLinked: nComponents must keep the component weights within ${ELEMENT_LIMIT} elements`,
    );
    expect(
      refusal(() =>
        decomposeStemsLinked({
          channels,
          sampleRate: SR,
          nComponents: 100000,
          nFft: 4,
          hopLength: 1,
        }),
      ).message,
    ).toBe(
      `decomposeStemsLinked: nComponents must keep the component activations within ${ELEMENT_LIMIT} elements`,
    );
  });

  it('noteStretch refuses stretchRatio by name', () => {
    const error = refusal(() =>
      noteStretch({
        samples: sine(8192),
        sampleRate: SR,
        onsetSample: 1000,
        offsetSample: 6000,
        stretchRatio: 1e6,
      }),
    );
    expect(error.message).toBe(
      'noteStretch: stretchRatio must keep the stretched note within 2147483647 elements',
    );
  });
});
