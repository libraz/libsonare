import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, type SonareError, synthPresetPatch } from '../src/index';

describe('SonareError', () => {
  it('exposes an ErrorCode enum aligned with the C ABI', () => {
    expect(ErrorCode.Ok).toBe(0);
    expect(ErrorCode.FileNotFound).toBe(1);
    expect(ErrorCode.InvalidParameter).toBe(4);
    expect(ErrorCode.NotSupported).toBe(6);
    expect(ErrorCode.InvalidState).toBe(7);
    expect(ErrorCode.Unknown).toBe(99);
  });

  it('carries name, numeric code, and codeName on a C-ABI failure', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(Error);
    expect(isSonareError(caught)).toBe(true);

    const err = caught as SonareError;
    expect(err.name).toBe('SonareError');
    expect(err.code).toBe(ErrorCode.InvalidParameter);
    expect(err.codeName).toBe('InvalidParameter');
    expect(typeof err.message).toBe('string');
    expect(err.message.length).toBeGreaterThan(0);
  });
});
