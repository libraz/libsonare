import { EXPECTED_ABI_VERSION } from './abi_version.js';
import { ErrorCode, SonareError } from './errors.js';

export { EXPECTED_ABI_VERSION };

/**
 * Throw an `AbiMismatch` {@link SonareError} when the native module reports a
 * packed ABI version other than the one this package was built against.
 *
 * @param actual Version reported by the native module's `abiVersion()`.
 */
export function checkAbiVersion(actual: unknown): void {
  const expected = EXPECTED_ABI_VERSION;
  if (actual === expected) {
    return;
  }
  const hex = (value: unknown): string =>
    typeof value === 'number' ? `0x${(value >>> 0).toString(16).padStart(8, '0')}` : String(value);
  throw new SonareError(
    ErrorCode.AbiMismatch,
    'AbiMismatch',
    `libsonare native addon ABI mismatch: the addon reports version ${hex(actual)}, ` +
      `but this package expects ${hex(expected)}. Rebuild the addon (yarn build) so it matches the package.`,
  );
}
