/**
 * Input checks the spectrogram and inverse-transform entries share.
 */

import type { ValidateOptions } from './validation';
import { assertFiniteScalar, assertPositiveInteger } from './validation';

export type GuardedOptions = ValidateOptions;

export function validatePositiveIntegers(fnName: string, values: Record<string, number>): void {
  for (const [name, value] of Object.entries(values)) {
    assertPositiveInteger(fnName, value, name);
  }
}

export function validateMelFrequencyRange(
  fnName: string,
  fmin: number,
  fmax: number,
  sampleRate: number,
): void {
  assertFiniteScalar(fnName, fmin, 'fmin');
  assertFiniteScalar(fnName, fmax, 'fmax');
  if (fmin < 0) {
    throw new RangeError(`${fnName}: fmin must be non-negative`);
  }
  if (fmax < 0) {
    throw new RangeError(`${fnName}: fmax must be non-negative`);
  }
  const effectiveFmax = fmax === 0 ? sampleRate / 2 : fmax;
  if (effectiveFmax <= fmin) {
    throw new RangeError(`${fnName}: fmax must be greater than fmin`);
  }
}
