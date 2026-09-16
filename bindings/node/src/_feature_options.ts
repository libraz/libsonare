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

/**
 * {@link resolvePositiveIntegerOption} without the positivity half, for a field
 * whose callee carries no domain at all.
 *
 * A fraction is still refused, because the addon narrowing truncates it onto a
 * value the callee accepts and answers: a `hopLength` of `512.7` converts at
 * `512` with nothing anywhere reporting the substitution. What is deliberately
 * not refused is a non-positive value, which the conversion helpers define --
 * `samplesToFrames` answers 0 frames for a non-positive hop rather than failing.
 */
export function resolveIntegerOption(
  fnName: string,
  name: string,
  value: unknown,
  fallback: number,
): number {
  const resolved = value === undefined ? fallback : value;
  assertIntegerValue(fnName, resolved, name);
  return resolved;
}
