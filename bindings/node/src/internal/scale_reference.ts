import { assertFloatValue } from '../validation.js';
import { refuse } from './entry.js';

/**
 * The native argument for the scale helpers' optional `scaleReferenceMidi`. Their
 * native entry reads 0 as A4, so absent maps to 0 and an explicit 0, which would
 * silently become A4 rather than MIDI note 0, is refused until native reads it literally.
 */
export function scaleReferenceMidiArg(value: number | undefined): number {
  if (value === undefined) {
    return 0;
  }
  assertFloatValue(value, 'scaleReferenceMidi');
  if (value === 0) {
    throw refuse(`scaleReferenceMidi 0 is temporarily unsupported (omit it for the A4 default)`);
  }
  return value;
}
