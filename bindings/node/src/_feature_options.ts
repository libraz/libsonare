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
  if (typeof resolved !== 'number') {
    throw new TypeError(`${fnName}: ${name} must be an integer`);
  }
  // A fractional or non-finite number is the right type out of domain, which is
  // the RangeError side of the split the branch below is already on.
  if (!Number.isInteger(resolved)) {
    throw new RangeError(`${fnName}: ${name} must be an integer`);
  }
  if (resolved <= 0 || resolved > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: ${name} must be a positive integer`);
  }
  return resolved;
}
