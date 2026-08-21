/**
 * Numeric error codes carried by a {@link SonareError}. Mirrors the C ABI
 * `SonareError` enum (and the Node / Python surfaces), so the same failure
 * reports the same numeric code on every binding.
 */
export enum ErrorCode {
  Ok = 0,
  FileNotFound = 1,
  InvalidFormat = 2,
  DecodeFailed = 3,
  InvalidParameter = 4,
  OutOfMemory = 5,
  NotSupported = 6,
  InvalidState = 7,
  Cancelled = 8,
  EncodeFailed = 9,
  Unknown = 99,
}

/**
 * Error thrown by libsonare on a native (C++) failure. Carries a numeric
 * {@link ErrorCode} `code` plus its canonical `codeName`, so callers can branch
 * on the cause instead of matching message text.
 *
 * Narrow a caught value with {@link isSonareError} or with `instanceof`; both
 * accept the same values. The Node package exports the same class under the
 * same name.
 */
export class SonareError extends Error {
  /** Numeric error code, equal to an {@link ErrorCode} value. */
  readonly code: number;
  /** Canonical name of `code`, e.g. `'InvalidParameter'`. */
  readonly codeName: string;

  constructor(code: number, codeName: string, message: string) {
    super(message);
    this.name = 'SonareError';
    this.code = code;
    this.codeName = codeName;
  }

  /**
   * Brand-based `instanceof`: an error that carries the shape narrows here even
   * when it is not literally an instance of this class. That is not a
   * hypothetical — an error posted from the analysis worker arrives as a
   * structured clone with its prototype gone, which a prototype-based
   * `instanceof` would silently miss. Delegates to {@link isSonareError} so the
   * two never disagree.
   */
  static [Symbol.hasInstance](value: unknown): value is SonareError {
    return isSonareError(value);
  }
}

/**
 * Type guard: whether a caught value is a libsonare {@link SonareError}.
 *
 * Duck-typed on purpose: a value that crossed a worker boundary has lost its
 * prototype, so a prototype check would miss it.
 */
export function isSonareError(value: unknown): value is SonareError {
  return (
    value instanceof Error &&
    (value as { name?: unknown }).name === 'SonareError' &&
    typeof (value as { code?: unknown }).code === 'number'
  );
}
