import { ErrorCode, SonareError } from './errors.js';

/** Builds the `InvalidState` error every handle class throws when used after `delete()`. */
export function releasedHandleError(className: string): SonareError {
  return new SonareError(ErrorCode.InvalidState, 'InvalidState', `${className} has been released`);
}

/** Throws `InvalidState` once `released` is set; a boolean test, safe on per-block paths. */
export function assertHandleLive(released: boolean, className: string): void {
  if (released) {
    throw releasedHandleError(className);
  }
}
