import type { TranscribeOptions } from '../project_types.js';
import { assertFloatValue, assertFrequencyBounds } from '../validation.js';
import { refuse } from './entry.js';

/**
 * Checks the ratio fields, the F0 bounds and `reattackRatio`; every key passes
 * through to the native option reader as written.
 */
export function nativeTranscribeOptions(
  options: TranscribeOptions & Record<string, unknown>,
): TranscribeOptions {
  for (const key of ['minFramePeakRatio', 'minRidgePeakRatio'] as const) {
    const value = options[key];
    if (value !== undefined && value !== null && typeof value === 'number' && value < 0) {
      throw refuse(`${key} must not be negative`);
    }
  }
  assertFrequencyBounds(options.f0MinHz, options.f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const reattack = options.reattackRatio;
  if (reattack !== undefined && reattack !== null) {
    assertFloatValue(reattack, 'reattackRatio');
    if ((reattack as number) <= 1) {
      throw refuse(`reattackRatio must be greater than 1 (omit it for no split)`);
    }
  }
  return options;
}
