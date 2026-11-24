import { describe, expect, it } from 'vitest';
import { mfcc, mfccToAudio, mfccToMel } from '../src/index.js';

const sampleRate = 22050;
const samples = new Float32Array(512);
const coefficients = { rows: 8, cols: 4, data: new Float32Array(32) };
const geometry = { sampleRate, nFft: 512, hopLength: 128, nMels: 16 };

const routes = [
  { name: 'mfcc', run: (lifter: number) => mfcc({ samples, ...geometry, nMfcc: 8, lifter }) },
  { name: 'mfccToMel', run: (lifter: number) => mfccToMel({ coefficients, nMels: 16, lifter }) },
  {
    name: 'mfccToAudio',
    run: (lifter: number) => mfccToAudio({ coefficients, ...geometry, nIter: 1, lifter }),
  },
];

describe('MFCC lifter admission', () => {
  it.each(
    routes.flatMap(({ name, run }) =>
      [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, -1, -1e-50].map(
        (lifter) => ({ name, run, lifter }),
      ),
    ),
  )('$name names the rejected lifter $lifter', ({ name, run, lifter }) => {
    expect(() => run(lifter)).toThrow(
      new RangeError(`${name}: lifter must be finite and at least 0`),
    );
  });

  it.each(routes)('$name accepts an explicitly written negative zero', ({ run }) => {
    expect(() => run(-0)).not.toThrow();
  });

  it.each([
    {
      name: 'mfccToMel',
      run: (data: Float32Array) =>
        mfccToMel({ coefficients: { ...coefficients, data }, nMels: 16, lifter: -1e-50 }),
    },
    {
      name: 'mfccToAudio',
      run: (data: Float32Array) =>
        mfccToAudio({
          coefficients: { ...coefficients, data },
          ...geometry,
          nIter: 1,
          lifter: -1e-50,
        }),
    },
  ])('$name checks a tiny negative lifter before coefficient values', ({ name, run }) => {
    const data = coefficients.data.slice();
    data[1] = Number.NaN;
    expect(() => run(data)).toThrow(
      new RangeError(`${name}: lifter must be finite and at least 0`),
    );
  });

  it('admits forward geometry, then lifter, then the sample values', () => {
    const lifter = Number.NaN;
    expect(() => mfcc({ samples, ...geometry, nMfcc: 8, fmaxHz: sampleRate, lifter })).toThrow(
      new RangeError('mfcc: fmaxHz must be finite and at most 11025'),
    );
    const invalidSamples = samples.slice();
    invalidSamples[1] = Number.NaN;
    expect(() => mfcc({ samples: invalidSamples, ...geometry, nMfcc: 8, lifter })).toThrow(
      new RangeError('mfcc: lifter must be finite and at least 0'),
    );
  });

  it('checks nMels before lifter in the inverse paths', () => {
    const lifter = Number.NaN;
    expect(() => mfccToMel({ coefficients, nMels: 0, lifter })).toThrow(
      new RangeError('mfccToMel: nMels must be an integer in [1, 2147483647]'),
    );
    expect(() => mfccToAudio({ coefficients, ...geometry, nMels: 0, nIter: 1, lifter })).toThrow(
      new RangeError('mfccToAudio: nMels must be an integer in [1, 2147483647]'),
    );
  });
});
