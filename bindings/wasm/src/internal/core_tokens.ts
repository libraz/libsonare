import { ErrorCode, type SonareError } from '../errors.js';
import { codedError } from './entry.js';

/**
 * A value the library returned that this binding's mirrored table cannot name is a version
 * skew, not a refused argument: it is a coded `InvalidFormat`, never folded into a default.
 */

/** The failure of a native result this binding cannot read; `InvalidFormat`. */
export function skewError(text: string): SonareError {
  return codedError(ErrorCode.InvalidFormat, text);
}

/** The failure for an ordinal outside the table of @p noun. */
export function unknownOrdinal(noun: string, ordinal: unknown): SonareError {
  return skewError(`unknown ${noun} ordinal: ${String(ordinal)}`);
}

/** The failure for a string token outside the table of @p noun. */
export function unknownToken(noun: string, token: unknown): SonareError {
  return skewError(`unknown ${noun} token: ${String(token)}`);
}

/** The token at a native ordinal; an ordinal the table does not reach is refused. */
export function ordinalToken<T extends string>(
  tokens: readonly T[],
  ordinal: unknown,
  noun: string,
): T {
  if (typeof ordinal !== 'number' || !Number.isInteger(ordinal) || ordinal < 0) {
    throw unknownOrdinal(noun, ordinal);
  }
  const token = tokens[ordinal];
  if (token === undefined) {
    throw unknownOrdinal(noun, ordinal);
  }
  return token;
}

/** A token the library returned, held to the mirrored set; one outside it is refused. */
export function libraryToken<T extends string>(
  tokens: readonly T[],
  token: unknown,
  noun: string,
): T {
  if (typeof token !== 'string' || !tokens.includes(token as T)) {
    throw unknownToken(noun, token);
  }
  return token as T;
}
