import type { VocalErrorDetail } from './types_vocal_edit.js';
/**
 * Numeric error codes carried by a {@link SonareError}. Mirrors the C ABI
 * `SonareError` enum (and the Python `SonareError.code`), so the same failure
 * reports the same numeric code on every surface.
 */
export const ErrorCode = {
  Ok: 0,
  FileNotFound: 1,
  InvalidFormat: 2,
  DecodeFailed: 3,
  InvalidParameter: 4,
  OutOfMemory: 5,
  NotSupported: 6,
  InvalidState: 7,
  Cancelled: 8,
  EncodeFailed: 9,
  AbiMismatch: 10,
  /** An operation was attempted on a result its source has since invalidated. */
  StaleResult: 11,
  Unknown: 99,
} as const;

/** Numeric value of an {@link ErrorCode}. */
export type ErrorCode = (typeof ErrorCode)[keyof typeof ErrorCode];

/** Canonical name of an {@link ErrorCode}, e.g. `'InvalidParameter'`. */
export type ErrorCodeName = keyof typeof ErrorCode;

const ERROR_CODE_NAMES = new Map<number, ErrorCodeName>(
  (Object.keys(ErrorCode) as ErrorCodeName[]).map((name) => [ErrorCode[name], name]),
);

/** Options of the {@link SonareError} constructor. */
export interface SonareErrorOptions {
  /** Underlying cause, passed through to `Error`. */
  cause?: unknown;
}

/**
 * Error raised by libsonare on a C-ABI failure. A standard `Error` whose `name`
 * is `'SonareError'`, augmented with a numeric `code` (one of
 * {@link ErrorCode}) and its canonical `codeName`.
 *
 * A refused caller argument is not a `SonareError`: the C ABI's
 * `InvalidParameter` surfaces as a `TypeError` when the shape is wrong — a
 * wrong JS type, or a key the request object or parameter bag does not
 * declare — and as a `RangeError` when a value is outside its domain, whichever
 * layer refused it; so a coded `SonareError` always means a library or
 * object-state failure. `ErrorCode.InvalidParameter` stays for parity with the C ABI numbering.
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
  /** Always `'SonareError'`. */
  override readonly name = 'SonareError' as const;
  /** Numeric error code, one of the {@link ErrorCode} values. */
  readonly code: ErrorCode;
  /** Canonical name of `code`, e.g. `'InvalidParameter'`. */
  readonly codeName: ErrorCodeName;
  /** Structured detail of a vocal-edit failure: the reason token and the field it concerns. */
  declare readonly detail?: VocalErrorDetail;

  /**
   * @param code Numeric code; a value outside {@link ErrorCode} reads back as `codeName` `'Unknown'`.
   * @param message Human-readable description.
   * @param options `cause` is passed through to `Error`.
   */
  constructor(code: ErrorCode, message: string, options?: SonareErrorOptions) {
    super(message, options);
    this.code = code;
    this.codeName = ERROR_CODE_NAMES.get(code) ?? 'Unknown';
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
