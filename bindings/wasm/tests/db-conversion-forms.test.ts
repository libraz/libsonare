/**
 * The four dB conversions are elementwise, so they take a flat array or a
 * matrix and return the same form: a matrix keeps its rows and columns and
 * its data equals the flat conversion of the same values.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { amplitudeToDb, dbToAmplitude, dbToPower, init, powerToDb } from '../dist/index.js';

const DATA = new Float32Array([1, 0.5, 1e-3, 0.25, 2, 1e-6]);

const CONVERSIONS = [
  ['powerToDb', (values: never) => powerToDb({ values, topDb: 40 })],
  ['amplitudeToDb', (values: never) => amplitudeToDb({ values, topDb: 40 })],
  ['dbToPower', (values: never) => dbToPower({ values })],
  ['dbToAmplitude', (values: never) => dbToAmplitude({ values })],
] as const;

describe('dB conversion forms', () => {
  beforeAll(async () => {
    await init();
  });

  it.each(CONVERSIONS)('%s returns a matrix shaped like its input', (_name, convert) => {
    const flat = convert(DATA as never) as Float32Array;
    expect(flat).toBeInstanceOf(Float32Array);
    const matrix = convert({ data: DATA, rows: 2, cols: 3 } as never) as {
      data: Float32Array;
      rows: number;
      cols: number;
    };
    expect(matrix.rows).toBe(2);
    expect(matrix.cols).toBe(3);
    expect(Array.from(matrix.data)).toEqual(Array.from(flat));
  });

  it.each(CONVERSIONS)(
    '%s refuses a matrix whose data does not match its shape',
    (_name, convert) => {
      expect(() => convert({ data: DATA, rows: 4, cols: 2 } as never)).toThrow(RangeError);
    },
  );

  it.each(CONVERSIONS)('%s refuses a plain array by naming both forms', (_name, convert) => {
    expect(() => convert([1, 2] as never)).toThrow(
      new TypeError(`${_name}: values must be a Float32Array or a Matrix2D`),
    );
  });
});

describe('dB conversion topDb', () => {
  const LEVELS = [
    ['powerToDb', powerToDb, new Float32Array([1, 1e-12]), 1e-15],
    ['amplitudeToDb', amplitudeToDb, new Float32Array([1, 1e-6]), 1e-9],
  ] as const;

  it.each(LEVELS)('%s clips at topDb under the peak', (_name, convert, values, amin) => {
    const out = convert({ values, amin, topDb: 40 }) as Float32Array;
    expect(out[0]).toBeCloseTo(0, 4);
    expect(out[1]).toBeCloseTo(-40, 4);
  });

  it.each(LEVELS)('%s keeps every level with topDb Infinity', (_name, convert, values, amin) => {
    const out = convert({ values, amin, topDb: Number.POSITIVE_INFINITY }) as Float32Array;
    expect(out[0]).toBeCloseTo(0, 4);
    expect(out[1]).toBeCloseTo(-120, 3);
  });

  it.each(LEVELS)('%s refuses a negative or NaN topDb', (_name, convert, values, amin) => {
    for (const topDb of [-1, Number.NaN]) {
      expect(() => convert({ values, amin, topDb })).toThrow(RangeError);
      expect(() => convert({ values, amin, topDb })).toThrow(/topDb must be at least 0/);
    }
  });
});
