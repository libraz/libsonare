import type { VocalErrorDetail } from './public_types_vocal_edit.js';
/**
 * Numeric error codes carried by a {@link SonareError}. Mirrors the C ABI
 * `SonareError` enum (and the Node / Python surfaces), so the same failure
 * reports the same numeric code on every binding.
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
 * Error thrown by libsonare on a native (C++) failure. Carries a numeric
 * {@link ErrorCode} `code` plus its canonical `codeName`, so callers can branch
 * on the cause instead of matching message text.
 *
 * Narrow a caught value with {@link isSonareError} or with `instanceof`; both
 * accept the same values. The Node package exports the same class under the
 * same name.
 */
export class SonareError extends Error {
  /** Numeric error code, one of the {@link ErrorCode} values. */
  readonly code: ErrorCode;
  /** Canonical name of `code`, e.g. `'InvalidParameter'`. */
  readonly codeName: ErrorCodeName;
  /** The failure this error wraps, when there is one (a cancellation carries the abort reason). */
  declare readonly cause?: unknown;
  /** Structured detail of a vocal-edit failure: the reason token and the field it concerns. */
  declare readonly detail?: VocalErrorDetail;

  /**
   * @param code Numeric code; a value outside {@link ErrorCode} reads back as `codeName` `'Unknown'`.
   * @param message Human-readable description.
   * @param options `cause` is passed through to `Error`.
   */
  constructor(code: ErrorCode, message: string, options?: SonareErrorOptions) {
    super(message);
    if (options !== undefined && 'cause' in options) {
      Object.defineProperty(this, 'cause', {
        value: options.cause,
        writable: true,
        configurable: true,
      });
    }
    this.name = 'SonareError';
    this.code = code;
    this.codeName = ERROR_CODE_NAMES.get(code) ?? 'Unknown';
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
