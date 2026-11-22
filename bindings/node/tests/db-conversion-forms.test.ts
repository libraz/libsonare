/**
 * The four dB conversions are elementwise, so they take a flat array or a
 * matrix and return the same form: a matrix keeps its rows and columns and
 * its data equals the flat conversion of the same values.
 */

import { describe, expect, it } from 'vitest';
import { amplitudeToDb, dbToAmplitude, dbToPower, powerToDb } from '../src/index.js';

const DATA = new Float32Array([1, 0.5, 1e-3, 0.25, 2, 1e-6]);

const CONVERSIONS = [
  ['powerToDb', (values: never) => powerToDb({ values, topDb: 40 })],
  ['amplitudeToDb', (values: never) => amplitudeToDb({ values, topDb: 40 })],
  ['dbToPower', (values: never) => dbToPower({ values })],
  ['dbToAmplitude', (values: never) => dbToAmplitude({ values })],
] as const;

describe('dB conversion forms', () => {
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
