/**
 * Input checks the spectrogram and inverse-transform entries share.
 */

import { assertPositiveInteger } from '../validation.js';

export function validatePositiveIntegers(values: Record<string, number>): void {
  for (const [name, value] of Object.entries(values)) {
    assertPositiveInteger(value, name);
  }
}
