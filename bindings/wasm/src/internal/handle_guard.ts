import { ErrorCode, SonareError } from '../errors.js';

/**
 * Builds the `InvalidState` error every handle, client and engine reports once
 * it was released, disposed, closed or destroyed — for a new call and for a
 * request that was still pending at that point alike. `state` completes the
 * message after the owner's name.
 */
export function releasedHandleError(className: string, state = 'has been released'): SonareError {
  return new SonareError(ErrorCode.InvalidState, 'InvalidState', `${className} ${state}`);
}

/** Throws `InvalidState` once `released` is set; a boolean test, safe on per-block paths. */
export function assertHandleLive(released: boolean, className: string): void {
  if (released) {
    throw releasedHandleError(className);
  }
}
