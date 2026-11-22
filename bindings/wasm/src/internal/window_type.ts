import type { WindowType } from '../public_types_spectral.js';
import { assertClosedToken } from '../validation.js';

/** Every {@link WindowType} name, in the ordinal order of the C enum `SonareWindowType`. */
export const WINDOW_TYPES: readonly WindowType[] = ['hann', 'hamming', 'blackman', 'rectangular'];

/** The C ordinal of a {@link WindowType} name: a number is a TypeError, an unknown name a RangeError. */
export function windowOrdinal(window: unknown): number {
  assertClosedToken(window, WINDOW_TYPES, 'window');
  return WINDOW_TYPES.indexOf(window);
}
