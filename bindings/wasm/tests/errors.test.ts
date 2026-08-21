/**
 * WASM error-surface tests: native (C++) failures must reach JS as a
 * SonareError carrying name / numeric code / codeName, matching the C ABI.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, SonareError, synthPresetPatch } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('SonareError', () => {
  it('exposes an ErrorCode enum aligned with the C ABI', () => {
    expect(ErrorCode.Ok).toBe(0);
    expect(ErrorCode.FileNotFound).toBe(1);
    expect(ErrorCode.InvalidParameter).toBe(4);
    expect(ErrorCode.NotSupported).toBe(6);
    expect(ErrorCode.InvalidState).toBe(7);
    expect(ErrorCode.Unknown).toBe(99);
  });

  it('rethrows a native C++ exception as a coded SonareError', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect(isSonareError(caught)).toBe(true);

    const err = caught as SonareError;
    expect(err.name).toBe('SonareError');
    expect(err.code).toBe(ErrorCode.InvalidParameter);
    expect(err.codeName).toBe('InvalidParameter');
    // The native detail message survives the pointer round-trip.
    expect(err.message).toContain('preset');
  });
});

// `SonareError` used to be a runtime class in the WASM package and a type-only
// interface in the Node package, both re-exported from the index under the same
// name. Importing it in a module shared between the two therefore resolved to
// `undefined` at runtime on one side, and `instanceof` was a compile error
// there. Both packages now export the same value class.
describe('SonareError is a value class with brand-based instanceof', () => {
  it('is a constructible runtime value, not a type-only name', () => {
    expect(typeof SonareError).toBe('function');
    const built = new SonareError(ErrorCode.InvalidParameter, 'InvalidParameter', 'built by hand');
    expect(built).toBeInstanceOf(Error);
    expect(built.name).toBe('SonareError');
    expect(built.code).toBe(4);
    expect(built.codeName).toBe('InvalidParameter');
  });

  it('narrows a native failure through instanceof as well as isSonareError', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(isSonareError(caught)).toBe(true);
    // The two must never disagree: instanceof delegates to the same predicate.
    expect(caught instanceof SonareError).toBe(true);
  });

  it('still narrows an error that lost its prototype crossing a boundary', () => {
    // What a structured clone leaves behind: the shape, not the prototype.
    const cloned = Object.assign(new Error('cloned'), {
      name: 'SonareError',
      code: ErrorCode.InvalidState,
      codeName: 'InvalidState',
    });
    expect(Object.getPrototypeOf(cloned)).toBe(Error.prototype);
    expect(cloned instanceof SonareError).toBe(true);
    expect(isSonareError(cloned)).toBe(true);
  });

  it('rejects a plain Error and a look-alike without a numeric code', () => {
    expect(new Error('plain') instanceof SonareError).toBe(false);
    const noCode = Object.assign(new Error('x'), { name: 'SonareError', codeName: 'Unknown' });
    expect(noCode instanceof SonareError).toBe(false);
    expect(isSonareError(noCode)).toBe(false);
  });
});
