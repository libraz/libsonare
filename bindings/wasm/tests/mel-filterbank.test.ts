/**
 * melFilterbank and melFrequencies against the librosa references, and the
 * structure that ties the two together.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, type Matrix2D, melFilterbank, melFrequencies } from '../dist/index.js';

type FilterbankRow = {
  sr: number;
  n_fft: number;
  n_mels: number;
  htk: boolean;
  fmax?: number | null;
  norm?: null;
  shape: number[];
  sum: number;
  max: number;
  row_sums: number[];
  first_rows?: number[][];
};
type FrequenciesRow = {
  n_mels: number;
  fmin: number;
  fmax: number;
  htk: boolean;
  values: number[];
};

function rows<T>(name: string): T[] {
  const url = new URL(`../../../tests/librosa/reference/${name}`, import.meta.url);
  return (JSON.parse(readFileSync(url, 'utf8')) as { data: T[] }).data;
}

/** |got - want| within a relative tolerance of want. */
function expectClose(got: number, want: number, rtol: number, label: string): void {
  expect(Math.abs(got - want), label).toBeLessThanOrEqual(rtol * Math.abs(want));
}

function rowOf(m: Matrix2D, r: number): Float32Array {
  return m.data.subarray(r * m.cols, (r + 1) * m.cols);
}

const sum = (values: ArrayLike<number>): number => Array.from(values).reduce((a, b) => a + b, 0);

describe('melFilterbank matches librosa', () => {
  beforeAll(async () => {
    await init();
  });
  for (const row of rows<FilterbankRow>('mel_filterbank.json')) {
    const norm = 'norm' in row && row.norm === null ? 'none' : 'slaney';
    it(`sr=${row.sr} nFft=${row.n_fft} nMels=${row.n_mels} htk=${row.htk} fmax=${row.fmax} norm=${norm}`, () => {
      const fb = melFilterbank({
        sampleRate: row.sr,
        nFft: row.n_fft,
        nMels: row.n_mels,
        ...(row.fmax == null ? {} : { fmaxHz: row.fmax }),
        htk: row.htk,
        norm,
      });
      expect([fb.rows, fb.cols]).toEqual(row.shape);
      expectClose(sum(fb.data), row.sum, 1e-3, 'sum');
      expectClose(
        fb.data.reduce((m, x) => Math.max(m, x), 0),
        row.max,
        1e-3,
        'max',
      );
      row.row_sums.forEach((want, m) => {
        expectClose(sum(rowOf(fb, m)), want, 1e-3, `row ${m} sum`);
      });
      (row.first_rows ?? []).forEach((want, m) => {
        const got = rowOf(fb, m);
        want.forEach((w, k) => {
          if (w === 0) {
            expect(Math.abs(got[k]), `row ${m} bin ${k}`).toBeLessThanOrEqual(1e-7);
          } else {
            expectClose(got[k], w, 1e-3, `row ${m} bin ${k}`);
          }
        });
      });
    });
  }
});

describe('melFrequencies matches librosa', () => {
  beforeAll(async () => {
    await init();
  });
  for (const row of rows<FrequenciesRow>('mel_frequencies.json')) {
    it(`nMels=${row.n_mels} fmin=${row.fmin} fmax=${row.fmax} htk=${row.htk}`, () => {
      const points = melFrequencies({
        nMels: row.n_mels,
        fminHz: row.fmin,
        fmaxHz: row.fmax,
        htk: row.htk,
      });
      expect(points).toHaveLength(row.values.length);
      row.values.forEach((want, i) => {
        if (want === 0) {
          expect(Math.abs(points[i]), `point ${i}`).toBeLessThanOrEqual(1e-3);
        } else {
          expectClose(points[i], want, 1e-4, `point ${i}`);
        }
      });
    });
  }
});

describe('the filterbank and its mel points', () => {
  beforeAll(async () => {
    await init();
  });
  it('peaks each band at its centre point', () => {
    const [sampleRate, nFft, nMels] = [22050, 2048, 40];
    const fb = melFilterbank({ sampleRate, nFft, nMels, norm: 'none' });
    const points = melFrequencies({ nMels: nMels + 2, fminHz: 0, fmaxHz: sampleRate / 2 });
    const binHz = sampleRate / nFft;
    for (let m = 0; m < nMels; m += 1) {
      const band = rowOf(fb, m);
      const peak = band.indexOf(band.reduce((a, b) => Math.max(a, b), 0));
      expect(Math.abs(peak * binHz - points[m + 1]), `band ${m}`).toBeLessThanOrEqual(binHz);
    }
  });

  it('reads an omitted fmaxHz as the Nyquist and refuses one above it', () => {
    const omitted = melFilterbank({ sampleRate: 44100, nFft: 1024, nMels: 32 });
    const nyquist = melFilterbank({ sampleRate: 44100, nFft: 1024, nMels: 32, fmaxHz: 22050 });
    expect(nyquist.data).toEqual(omitted.data);
    expect(() => melFilterbank({ sampleRate: 44100, nFft: 1024, nMels: 32, fmaxHz: 0 })).toThrow(
      new RangeError('melFilterbank: fmaxHz must be greater than 0; omit it for the default'),
    );
    expect(() =>
      melFilterbank({ sampleRate: 44100, nFft: 1024, nMels: 32, fmaxHz: 22051 }),
    ).toThrow(new RangeError('melFilterbank: fmaxHz must be finite and at most 22050'));
  });

  it("divides each band by its width under 'slaney' and leaves it at unit peak under 'none'", () => {
    const slaney = melFilterbank({ sampleRate: 22050, nFft: 1024, nMels: 16 });
    const none = melFilterbank({ sampleRate: 22050, nFft: 1024, nMels: 16, norm: 'none' });
    const points = melFrequencies({ nMels: 18, fminHz: 0, fmaxHz: 11025 });
    for (let m = 0; m < 16; m += 1) {
      const width = 2 / (points[m + 2] - points[m]);
      const plain = rowOf(none, m);
      const scaled = rowOf(slaney, m);
      expect(plain.reduce((a, b) => Math.max(a, b), 0)).toBeLessThanOrEqual(1);
      plain.forEach((x, k) => {
        expect(Math.abs(scaled[k] - x * width), `band ${m} bin ${k}`).toBeLessThanOrEqual(
          1e-4 * Math.abs(x * width) + 1e-9,
        );
      });
    }
  });
});

describe('a non-boolean htk is refused by name', () => {
  beforeAll(async () => {
    await init();
  });
  it('on both entries', () => {
    expect(() => melFilterbank({ sampleRate: 22050, nFft: 1024, htk: 'yes' as never })).toThrow(
      new TypeError('melFilterbank: htk must be a boolean'),
    );
    expect(() => melFrequencies({ htk: 1 as never })).toThrow(
      new TypeError('melFrequencies: htk must be a boolean'),
    );
  });
});
