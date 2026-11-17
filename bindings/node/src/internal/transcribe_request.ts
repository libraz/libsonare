import { assertFloatValue, assertFrequencyBounds } from '../validation.js';

/** The request fields the facade checks and renames before the addon reads them. */
interface TranscribeNativeFields {
  f0MinHz?: number;
  f0MaxHz?: number;
  polyphonic?: boolean;
  reattackRatio?: number;
}

/**
 * Checks the F0 bounds and `reattackRatio`, and spells them as the addon reads
 * them: `fmin` / `fmax`, and an absent polyphonic ratio as 0 (no split).
 */
export function nativeTranscribeRequest(
  fnName: string,
  request: TranscribeNativeFields,
): Record<string, unknown> {
  const { f0MinHz, f0MaxHz, ...rest } = request;
  assertFrequencyBounds(fnName, f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const native: Record<string, unknown> = { ...rest, fmin: f0MinHz, fmax: f0MaxHz };
  const { reattackRatio } = request;
  if (reattackRatio === undefined || reattackRatio === null) {
    // The addon reads an absent ratio as its default split and a written 0 as "off".
    if (request.polyphonic === true) {
      native.reattackRatio = 0;
    } else {
      delete native.reattackRatio;
    }
  } else {
    assertFloatValue(fnName, reattackRatio, 'reattackRatio');
    if (reattackRatio <= 1) {
      throw new RangeError(
        `${fnName}: reattackRatio must be greater than 1 (omit it for no split)`,
      );
    }
  }
  return native;
}
