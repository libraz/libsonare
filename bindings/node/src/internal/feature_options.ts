import { assertIntegerValue, assertPositiveInteger } from '../validation.js';

/**
 * Option resolvers shared by more than one feature module.
 *
 * Deliberately internal, like `fft_options.ts`: the `feature_*` modules import
 * it, but the `features.js` barrel does not re-export it, so it stays out of the
 * package entry point.
 */

export function resolvePositiveIntegerOption(
  name: string,
  value: unknown,
  fallback: number,
): number {
  const resolved = value === undefined ? fallback : value;
  assertIntegerValue(resolved, name);
  assertPositiveInteger(resolved, name);
  return resolved;
}

/**
 * {@link resolvePositiveIntegerOption} without the positivity half, for a field
 * whose callee carries no domain at all.
 *
 * A fraction is still refused, because the addon narrowing truncates it onto a
 * value the callee accepts and answers. A `hopLength` of `512.7` would convert
 * at `512` without the caller seeing that change.
 */
export function resolveIntegerOption(name: string, value: unknown, fallback: number): number {
  const resolved = value === undefined ? fallback : value;
  assertIntegerValue(resolved, name);
  return resolved;
}
