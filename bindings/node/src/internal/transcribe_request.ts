import { assertFloatValue, assertFrequencyBounds } from '../validation.js';

/** The request fields the facade checks before the addon reads them. */
interface TranscribeNativeFields {
  f0MinHz?: number;
  f0MaxHz?: number;
  reattackRatio?: number;
}

/**
 * Checks the F0 bounds and `reattackRatio`; the request reaches the addon as
 * written, an absent ratio meaning no split.
 */
export function nativeTranscribeRequest<T extends TranscribeNativeFields>(
  fnName: string,
  request: T,
): T {
  assertFrequencyBounds(fnName, request.f0MinHz, request.f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const { reattackRatio } = request;
  if (reattackRatio !== undefined && reattackRatio !== null) {
    assertFloatValue(fnName, reattackRatio, 'reattackRatio');
    if (reattackRatio <= 1) {
      throw new RangeError(
        `${fnName}: reattackRatio must be greater than 1 (omit it for no split)`,
      );
    }
  }
  return request;
}
