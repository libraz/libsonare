import type { Matrix2D } from '../types_analysis.js';
import { assertFiniteValues, assertPositiveInteger } from '../validation.js';

/**
 * Matrix helpers shared by the `feature_*` modules.
 *
 * Internal, like `fft_options.ts`: nothing re-exports it, so it stays out of
 * the package entry point and out of the C-ABI parity surface.
 */

/** Wraps a native row-major buffer and its dimensions as a {@link Matrix2D}. */
export function toMatrix2D(data: Float32Array, rows: number, cols: number): Matrix2D {
  return { data, rows, cols };
}

/**
 * Checks a caller-supplied {@link Matrix2D}: `TypeError` unless an object whose
 * `data` is a `Float32Array`, `RangeError` unless `rows` and `cols` are positive
 * integers and `data.length === rows * cols`. The NaN/Inf scan runs unless
 * `validate` is `false`.
 */
export function assertMatrix2D(
  fnName: string,
  value: unknown,
  argName: string,
  validate: boolean | null | undefined = true,
): asserts value is Matrix2D {
  if (typeof value !== 'object' || value === null) {
    throw new TypeError(`${fnName}: ${argName} must be a Matrix2D`);
  }
  const { data, rows, cols } = value as Record<string, unknown>;
  if (!(data instanceof Float32Array)) {
    throw new TypeError(`${fnName}: ${argName}.data must be a Float32Array`);
  }
  if (typeof rows !== 'number' || typeof cols !== 'number') {
    throw new TypeError(`${fnName}: ${argName}.rows and ${argName}.cols must be numbers`);
  }
  assertPositiveInteger(fnName, rows, `${argName}.rows`);
  assertPositiveInteger(fnName, cols, `${argName}.cols`);
  if (data.length !== rows * cols) {
    throw new RangeError(`${fnName}: ${argName}.data length must equal rows * cols`);
  }
  if (validate !== false) {
    assertFiniteValues(fnName, data, `${argName}.data`);
  }
}

/** The index list shapes the addon reads (`Int32Array` or a plain array); other array-likes are copied. */
export function toNativeIndexList(
  fnName: string,
  values: Int32Array | ArrayLike<number>,
  argName: string,
): Int32Array | number[] {
  if (values instanceof Int32Array || Array.isArray(values)) {
    return values;
  }
  if (typeof values !== 'object' || values === null || typeof values.length !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be an Int32Array or an array of numbers`);
  }
  return Array.from(values);
}
