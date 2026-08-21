/**
 * Numeric error codes carried by a {@link SonareError}. Mirrors the C ABI
 * `SonareError` enum (and the Python `SonareError.code`), so the same failure
 * reports the same numeric code on every surface.
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
 * Error raised by libsonare on a C-ABI failure. A standard `Error` whose `name`
 * is `'SonareError'`, augmented with a numeric `code` (one of
 * {@link ErrorCode}) and its canonical `codeName`.
 *
 * Narrow a caught value with {@link isSonareError} or with `instanceof`; both
 * accept the same values. This class exists as a runtime value so the WASM
 * package and this one export the same *kind* of thing under this name — a
 * shared TypeScript module could previously import it from one package and
 * find it `undefined` at runtime in the other.
 *
 * The native addon raises plain `Error` objects carrying this shape rather than
 * instances of this class, so `instanceof` is brand-based (see
 * {@link SonareError[Symbol.hasInstance]}) rather than prototype-based.
 * Constructing one directly is supported for callers that re-raise a native
 * failure across a boundary that does not preserve prototypes.
 */
export class SonareError extends Error {
  override readonly name = 'SonareError' as const;
  /** Numeric error code, equal to an {@link ErrorCode} value. */
  readonly code: number;
  /** Canonical name of `code`, e.g. `'InvalidParameter'`. */
  readonly codeName: string;

  constructor(code: number, codeName: string, message: string) {
    super(message);
    this.code = code;
    this.codeName = codeName;
  }

  /**
   * Brand-based `instanceof`: an error that carries the shape narrows here even
   * though the addon never constructs this class, and so does one that lost its
   * prototype crossing a worker or `structuredClone` boundary. Delegates to
   * {@link isSonareError} so the two never disagree.
   */
  static [Symbol.hasInstance](value: unknown): value is SonareError {
    return isSonareError(value);
  }
}

/**
 * Type guard: whether a caught value is a libsonare {@link SonareError}.
 *
 * Duck-typed on purpose. The addon raises plain `Error` objects carrying the
 * shape, and a value that crossed a worker boundary has lost its prototype, so
 * a prototype check would miss both.
 */
export function isSonareError(value: unknown): value is SonareError {
  return (
    value instanceof Error &&
    (value as { name?: unknown }).name === 'SonareError' &&
    typeof (value as { code?: unknown }).code === 'number'
  );
}
