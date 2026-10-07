import { describe, expect, it } from 'vitest';
import { checkAbiVersion, EXPECTED_ABI_VERSION } from '../src/abi.js';
import { ErrorCode, isSonareError, SonareError } from '../src/errors.js';
import { addon } from '../src/native.js';

describe('ABI version check', () => {
  it('accepts the version the loaded addon reports', () => {
    expect(addon.abiVersion()).toBe(EXPECTED_ABI_VERSION);
    expect(() => checkAbiVersion(addon.abiVersion())).not.toThrow();
  });

  it('throws a coded AbiMismatch naming both versions and the remedy', () => {
    let caught: unknown;
    try {
      checkAbiVersion(EXPECTED_ABI_VERSION - 1);
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect(isSonareError(caught)).toBe(true);
    const error = caught as SonareError;
    expect(error.code).toBe(ErrorCode.AbiMismatch);
    expect(error.codeName).toBe('AbiMismatch');
    expect(error.message).toContain(
      `0x${(EXPECTED_ABI_VERSION - 1).toString(16).padStart(8, '0')}`,
    );
    expect(error.message).toContain(`0x${EXPECTED_ABI_VERSION.toString(16).padStart(8, '0')}`);
    expect(error.message).toMatch(/rebuild/i);
  });

  it('treats a missing version as a mismatch', () => {
    expect(() => checkAbiVersion(undefined)).toThrow(SonareError);
  });
});
