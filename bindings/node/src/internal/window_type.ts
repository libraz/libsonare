import type { WindowType } from '../types_features.js';

/** Every {@link WindowType} name, in the ordinal order of the C enum `SonareWindowType`. */
export const WINDOW_TYPES: readonly WindowType[] = ['hann', 'hamming', 'blackman', 'rectangular'];
