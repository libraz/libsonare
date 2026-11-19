import type { WindowType } from '../public_types_spectral.js';

/** Every {@link WindowType} name, in the ordinal order of the C enum `SonareWindowType`. */
export const WINDOW_TYPES: readonly WindowType[] = ['hann', 'hamming', 'blackman', 'rectangular'];

/** The C ordinal of a {@link WindowType} name: a number is a TypeError, an unknown name a RangeError. */
export function windowOrdinal(window: unknown): number {
  const ordinal = typeof window === 'string' ? WINDOW_TYPES.indexOf(window as WindowType) : -1;
  if (ordinal < 0) {
    const Refusal = typeof window === 'string' ? RangeError : TypeError;
    throw new Refusal(`window must be one of ${WINDOW_TYPES.join(', ')}`);
  }
  return ordinal;
}
