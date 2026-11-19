/**
 * A wrong-typed number is refused by its type alone ("must be an integer" /
 * "must be a number", TypeError); a number of the right type outside the domain
 * is refused with the domain stated (RangeError).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import * as sonare from '../dist/index.js';
import {
  assertFiniteScalar,
  assertIntegerValue,
  assertNonNegativeSafeInteger,
  assertSampleRate,
  assertU7,
} from '../src/validation.js';

function caught(call: () => unknown): Error {
  try {
    call();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
}

function refusal(call: () => unknown): { errorClass: string; message: string } {
  const error = caught(call);
  return { errorClass: error.constructor.name, message: error.message };
}

describe('the shared numeric readers split the type cause from the value cause', () => {
  beforeAll(async () => {
    await sonare.init();
  });

  it('a sample rate', () => {
    expect(refusal(() => assertSampleRate('22050'))).toEqual({
      errorClass: 'TypeError',
      message: 'sampleRate must be an integer',
    });
    for (const value of [22050.5, 4000, 400000, Number.NaN]) {
      expect(refusal(() => assertSampleRate(value))).toEqual({
        errorClass: 'RangeError',
        message: 'sampleRate must be an integer in [8000, 384000]',
      });
    }
  });

  it('a bounded integer', () => {
    expect(refusal(() => assertU7('5' as never, 'data1'))).toEqual({
      errorClass: 'TypeError',
      message: 'data1 must be an integer',
    });
    expect(refusal(() => assertU7(5.5, 'data1'))).toEqual({
      errorClass: 'RangeError',
      message: 'data1 must be an integer in [0, 127]',
    });
  });

  it('an integer whose domain is the native int', () => {
    expect(refusal(() => assertIntegerValue(true, 'hopLength'))).toEqual({
      errorClass: 'TypeError',
      message: 'hopLength must be an integer',
    });
    expect(refusal(() => assertIntegerValue(512.5, 'hopLength'))).toEqual({
      errorClass: 'RangeError',
      message: 'hopLength must be an integer in [-2147483648, 2147483647]',
    });
  });

  it('a count', () => {
    expect(refusal(() => assertNonNegativeSafeInteger('3' as never, 'count'))).toEqual({
      errorClass: 'TypeError',
      message: 'count must be an integer',
    });
    expect(refusal(() => assertNonNegativeSafeInteger(-1, 'count'))).toEqual({
      errorClass: 'RangeError',
      message: 'count must be an integer in [0, 9007199254740991]',
    });
  });

  it('a finite scalar', () => {
    expect(refusal(() => assertFiniteScalar('1' as never, 'frameRate'))).toEqual({
      errorClass: 'TypeError',
      message: 'frameRate must be a number',
    });
    expect(refusal(() => assertFiniteScalar(Number.POSITIVE_INFINITY, 'frameRate'))).toEqual({
      errorClass: 'RangeError',
      message: 'frameRate must be finite',
    });
  });

  it('reaches a caller through the entry prefix', () => {
    const samples = new Float32Array(2048);
    expect(refusal(() => sonare.detectBpm({ samples, sampleRate: '22050' as never }))).toEqual({
      errorClass: 'TypeError',
      message: 'detectBpm: sampleRate must be an integer',
    });
    expect(refusal(() => sonare.detectBpm({ samples, sampleRate: 22050.5 }))).toEqual({
      errorClass: 'RangeError',
      message: 'detectBpm: sampleRate must be an integer in [8000, 384000]',
    });
  });
});
