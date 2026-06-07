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
  Unknown = 99,
}

/**
 * Shape of the `Error` thrown by libsonare on a C-ABI failure. The runtime
 * value is a standard `Error` whose `name` is `'SonareError'`, augmented with a
 * numeric `code` (one of {@link ErrorCode}) and its canonical `codeName`. Use
 * {@link isSonareError} to narrow a caught value.
 */
export interface SonareError extends Error {
  name: 'SonareError';
  /** Numeric error code, equal to an {@link ErrorCode} value. */
  code: number;
  /** Canonical name of `code`, e.g. `'InvalidParameter'`. */
  codeName: string;
}

/** Type guard: whether a caught value is a libsonare {@link SonareError}. */
export function isSonareError(value: unknown): value is SonareError {
  return (
    value instanceof Error &&
    (value as { name?: unknown }).name === 'SonareError' &&
    typeof (value as { code?: unknown }).code === 'number'
  );
}
