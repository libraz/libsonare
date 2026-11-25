import { beforeAll, describe, expect, it } from 'vitest';
import { melSpectrogram, mfcc, mfccToAudio, mfccToMel } from '../src/analysis.js';
import { getSonareModule, initModule } from '../src/module_state.js';

beforeAll(async () => {
  await initModule('analysis', undefined, async () =>
    process.env.SONARE_TEST_WASM_FULL === '1'
      ? (await import('../dist/sonare.js')).default
      : (await import('../dist/sonare-analysis.js')).default,
  );
});
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

describe('Mel bridge input ownership', () => {
  it.each([
    {
      name: 'melSpectrogram',
      length: 512,
      run: (samples: Float32Array) =>
        getSonareModule().melSpectrogram(samples, sampleRate, 512, 128, 16, 0, 0, false).power,
    },
    {
      name: 'mfcc',
      length: 512,
      run: (samples: Float32Array) =>
        getSonareModule().mfcc(samples, sampleRate, 512, 128, 16, 8, 0, 0, false, 0).coefficients,
    },
    {
      name: 'mfccToAudio',
      length: 32,
      run: (data: Float32Array) =>
        getSonareModule().mfccToAudio(data, 8, 4, 16, sampleRate, 512, 128, 0, 0, 1, false, 0),
    },
  ])('$name scans the same sample values it consumes', ({ length, run }) => {
    let reads = 0;
    const data = Array<number>(length).fill(0);
    Object.defineProperty(data, '0', { get: () => (reads++ === 0 ? 0 : Number.NaN) });
    const result = run(data as unknown as Float32Array);
    expect(reads).toBe(1);
    expect(result.length).toBeGreaterThan(0);
    expect(result.every(Number.isFinite)).toBe(true);
  });

  it('checks ordinary Mel scalar shape before sample values', () => {
    const invalid = samples.slice();
    invalid[1] = Number.NaN;
    expect(() =>
      melSpectrogram({ ...geometry, samples: invalid, nFft: '512' as unknown as number }),
    ).toThrow(new TypeError('melSpectrogram: nFft must be an integer'));
  });

  it.each([0, 1e-50])(
    'keeps samples ahead of a written fmaxHz that becomes zero (%s)',
    (fmaxHz) => {
      const invalid = samples.slice();
      invalid[1] = Number.NaN;
      expect(() => melSpectrogram({ ...geometry, samples: invalid, fmaxHz })).toThrow(
        /samples.*non-finite/,
      );
    },
  );

  it('keeps nonfinite samples ahead of the heap preflight', () => {
    const invalid = new Float32Array(131072);
    invalid[1] = Number.NaN;
    expect(() =>
      melSpectrogram({ samples: invalid, sampleRate, nMels: 4096, nFft: 4, hopLength: 1 }),
    ).toThrow(/samples.*non-finite/);
  });

  it.each([1, 1.5])('keeps the samples refusal ahead of the numeric FFT domain for %s', (nFft) => {
    const invalid = samples.slice();
    invalid[1] = Number.NaN;
    expect(() => melSpectrogram({ ...geometry, samples: invalid, nFft })).toThrow(
      /samples.*non-finite/,
    );
  });
});
