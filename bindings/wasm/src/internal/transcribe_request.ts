import type { TranscribeOptions } from '../project_types.js';
import { assertFloatValue, assertFrequencyBounds } from '../validation.js';

/**
 * Checks the ratio fields and the F0 bounds, and maps the bounds to the native
 * `fmin` / `fmax`; every other key passes through to the native option reader.
 */
export function nativeTranscribeOptions(
  fnName: string,
  options: TranscribeOptions & Record<string, unknown>,
): TranscribeOptions {
  const { f0MinHz, f0MaxHz, ...rest } = options;
  for (const key of ['minFramePeakRatio', 'minRidgePeakRatio'] as const) {
    const value = rest[key];
    if (value !== undefined && value !== null && typeof value === 'number' && value < 0) {
      throw new RangeError(`${fnName}: ${key} must not be negative`);
    }
  }
  assertFrequencyBounds(fnName, f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const native: Record<string, unknown> = { ...rest };
  if (f0MinHz !== undefined && f0MinHz !== null) {
    native.fmin = f0MinHz;
  }
  if (f0MaxHz !== undefined && f0MaxHz !== null) {
    native.fmax = f0MaxHz;
  }
  const reattack = rest.reattackRatio;
  if (reattack === undefined || reattack === null) {
    // Native reads an absent ratio as its default split; a written 0 is "off".
    if (rest.polyphonic === true) {
      native.reattackRatio = 0;
    } else {
      delete native.reattackRatio;
    }
  } else {
    assertFloatValue(fnName, reattack, 'reattackRatio');
    if ((reattack as number) <= 1) {
      throw new RangeError(
        `${fnName}: reattackRatio must be greater than 1 (omit it for no split)`,
      );
    }
  }
  return native as unknown as TranscribeOptions;
}
