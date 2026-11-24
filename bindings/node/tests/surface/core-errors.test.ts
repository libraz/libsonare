import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  ErrorCode,
  type ErrorCodeName,
  isSonareError,
  SonareError,
  type SonareErrorOptions,
} from '../../dist/index.js';

const expectedTable: Record<string, number> = {
  Ok: 0,
  FileNotFound: 1,
  InvalidFormat: 2,
  DecodeFailed: 3,
  InvalidParameter: 4,
  OutOfMemory: 5,
  NotSupported: 6,
  InvalidState: 7,
  Cancelled: 8,
  EncodeFailed: 9,
  AbiMismatch: 10,
  StaleResult: 11,
  Unknown: 99,
};

describe('ErrorCode', () => {
  it('is a plain object whose value-to-name table matches the C ABI', () => {
    expect({ ...ErrorCode }).toEqual(expectedTable);
    expect(Object.getPrototypeOf(ErrorCode)).toBe(Object.prototype);
  });

  it('has no reverse mapping from value to name', () => {
    expect(Object.keys(ErrorCode)).toHaveLength(Object.keys(expectedTable).length);
    expect((ErrorCode as unknown as Record<number, unknown>)[4]).toBeUndefined();
  });

  it('types a code as the union of its values and a name as the union of its keys', () => {
    expectTypeOf<ErrorCodeName>().toEqualTypeOf<
      | 'Ok'
      | 'FileNotFound'
      | 'InvalidFormat'
      | 'DecodeFailed'
      | 'InvalidParameter'
      | 'OutOfMemory'
      | 'NotSupported'
      | 'InvalidState'
      | 'Cancelled'
      | 'EncodeFailed'
      | 'AbiMismatch'
      | 'StaleResult'
      | 'Unknown'
    >();
    expectTypeOf<ErrorCode>().toEqualTypeOf<0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 99>();
    expectTypeOf<SonareError['codeName']>().toEqualTypeOf<ErrorCodeName>();
    expectTypeOf<SonareError['code']>().toEqualTypeOf<ErrorCode>();
  });
});

describe('SonareError', () => {
  it('derives codeName from code and keeps the cause', () => {
    const cause = new Error('aborted');
    const error = new SonareError(ErrorCode.Cancelled, 'x', { cause });
    expect(error.code).toBe(8);
    expect(error.codeName).toBe('Cancelled');
    expect(error.message).toBe('x');
    expect(error.name).toBe('SonareError');
    expect((error as Error & { cause?: unknown }).cause).toBe(cause);
  });

  it('carries no cause when none is given', () => {
    const error = new SonareError(ErrorCode.StaleResult, 'stale');
    expect(error.codeName).toBe('StaleResult');
    expect('cause' in error).toBe(false);
  });

  it('reports an unknown numeric code as Unknown', () => {
    const error = new SonareError(1234 as ErrorCode, 'odd');
    expect(error.code).toBe(1234);
    expect(error.codeName).toBe('Unknown');
  });

  it('narrows through isSonareError and instanceof', () => {
    const error = new SonareError(ErrorCode.InvalidState, 'closed');
    expect(isSonareError(error)).toBe(true);
    expect(error instanceof SonareError).toBe(true);
    expect(error).toBeInstanceOf(Error);
    expect(isSonareError(new Error('plain'))).toBe(false);
    expect(new RangeError('range') instanceof SonareError).toBe(false);
  });

  it('narrows an error that lost its prototype crossing a boundary', () => {
    const cloned = Object.assign(new Error('cloned'), {
      name: 'SonareError',
      code: ErrorCode.InvalidState,
      codeName: 'InvalidState',
    });
    expect(cloned instanceof SonareError).toBe(true);
  });

  it('takes the migrated (code, message, options) constructor shape only', () => {
    expectTypeOf<ConstructorParameters<typeof SonareError>>().toEqualTypeOf<
      [ErrorCode, string, SonareErrorOptions?]
    >();
    // Type-level only: the closure is never called.
    const legacyForm = () =>
      // @ts-expect-error the codeName argument is gone
      new SonareError(ErrorCode.InvalidState, 'InvalidState', 'closed');
    expect(typeof legacyForm).toBe('function');
  });
});
