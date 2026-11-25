/** The per-call options argument a Worker client method takes, refused by name before dispatch. */

import { assertDeclaredKeys, refuseType } from './entry.js';

/**
 * Refuses a call-options argument by name: a non-object, a key outside @p allowed, a `copy` that
 * is not a boolean and a `signal` that is not an `AbortSignal`. Absent (`undefined` / `null`) is
 * the caller's business; this sees only a given value.
 */
export function checkWorkerCallOptions(options: unknown, allowed: readonly string[]): void {
  if (typeof options !== 'object' || options === null || Array.isArray(options)) {
    throw refuseType('options must be an object');
  }
  assertDeclaredKeys(options, allowed, 'options');
  const { copy, signal } = options as Record<string, unknown>;
  if (copy !== undefined && typeof copy !== 'boolean') {
    throw refuseType('options.copy must be a boolean');
  }
  if (
    signal !== undefined &&
    (typeof signal !== 'object' ||
      signal === null ||
      typeof (signal as AbortSignal).addEventListener !== 'function' ||
      typeof (signal as AbortSignal).aborted !== 'boolean')
  ) {
    throw refuseType('options.signal must be an AbortSignal');
  }
}
