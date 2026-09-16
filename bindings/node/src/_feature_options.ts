import { assertIntegerValue, assertPositiveInteger } from './validation.js';

/**
 * Option resolvers shared by more than one feature module.
 *
 * Deliberately internal, like `_fft_options.ts`: the `feature_*` modules import
 * it, but the `features.js` barrel does not re-export it, so it stays out of the
 * package entry point.
 */

export function resolvePositiveIntegerOption(
  fnName: string,
  name: string,
  value: unknown,
  fallback: number,
): number {
  const resolved = value === undefined ? fallback : value;
  assertIntegerValue(fnName, resolved, name);
  assertPositiveInteger(fnName, resolved, name);
  return resolved;
}
