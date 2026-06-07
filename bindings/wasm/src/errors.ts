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
  Unknown = 99,
}

/**
 * Error thrown by libsonare on a native (C++) failure. Carries a numeric
 * {@link ErrorCode} `code` plus its canonical `codeName`, so callers can branch
 * on the cause instead of matching message text.
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
}

/** Type guard: whether a caught value is a libsonare {@link SonareError}. */
export function isSonareError(value: unknown): value is SonareError {
  return (
    value instanceof Error &&
    (value as { name?: unknown }).name === 'SonareError' &&
    typeof (value as { code?: unknown }).code === 'number'
  );
}
