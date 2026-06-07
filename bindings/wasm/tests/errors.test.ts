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
