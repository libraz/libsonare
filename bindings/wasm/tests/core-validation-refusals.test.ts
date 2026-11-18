import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, mfccToAudio, mfccToMel } from '../src/index';
import { getSonareModule } from '../src/module_state';
import { expectRefusalOf } from './_helpers';

// The core function owns each refusal in this corpus. The embind wrappers call the
// core directly, so the refusal is asserted on the raw module as well as on the facade.

type Cause = 'argument';
type MfccInput = { n_mfcc: number; n_frames: number; fill: number };
type MfccCase = { id: string; n_mels: number; cause?: Cause; message?: string };
type MfccRoute = { input: MfccInput; accepted: MfccCase[]; refused: MfccCase[] };

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/core_validation_refusals.json', import.meta.url),
    'utf8',
  ),
) as { mfcc_to_mel: MfccRoute };

const errorClassFor: Record<Cause, typeof RangeError> = { argument: RangeError };

type RawInverse = {
  mfccToMel(
    data: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
    lifter: number,
  ): {
    nMels: number;
  };
  mfccToAudio(...args: unknown[]): Float32Array;
};

beforeAll(async () => {
  await init();
});

describe('mfcc_to_mel refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mfcc_to_mel;
  const data = new Float32Array(input.n_mfcc * input.n_frames).fill(input.fill);
  const coefficients = { data, rows: input.n_mfcc, cols: input.n_frames };
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ n_mels }) => {
    expect(mfccToMel({ coefficients, nMels: n_mels }).rows).toBe(n_mels);
    expect(raw().mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0).nMels).toBe(n_mels);
  });

  it.each(refused)('refuses $id', ({ n_mels, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = new RegExp(message as string);
    const calls: Array<() => unknown> = [
      () => mfccToMel({ coefficients, nMels: n_mels }),
      () => raw().mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0),
      () => mfccToAudio({ coefficients, nMels: n_mels, nIter: 1 }),
      () =>
        raw().mfccToAudio(
          data,
          input.n_mfcc,
          input.n_frames,
          n_mels,
          22050,
          2048,
          512,
          0,
          0,
          1,
          false,
          0,
        ),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});
