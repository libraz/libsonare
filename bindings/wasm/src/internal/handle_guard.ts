import { ErrorCode, type SonareError } from '../errors.js';
import { codedError } from './entry.js';

/**
 * Builds the `InvalidState` error every handle, client and engine reports once
 * its `destroy()` has run — for a new call and for a request that was still
 * pending at that point alike.
 */
export function releasedHandleError(className: string): SonareError {
  return codedError(ErrorCode.InvalidState, `${className} has been destroyed`, {
    reason: 'releasedHandle',
  });
}

/** Throws `InvalidState` once `released` is set; a boolean test, safe on per-block paths. */
export function assertHandleLive(released: boolean, className: string): void {
  if (released) {
    throw releasedHandleError(className);
  }
}
