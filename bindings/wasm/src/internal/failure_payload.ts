import { ErrorCode, isSonareError, SonareError } from '../errors.js';

/** A failure as it crosses a postMessage boundary: class name, message and any SonareError code. */
export interface SonareFailurePayload {
  name: string;
  message: string;
  code?: number;
  codeName?: string;
}

/** Captures `error` for posting, keeping a SonareError's code and codeName. */
export function failurePayload(error: unknown): SonareFailurePayload {
  if (isSonareError(error)) {
    return { name: error.name, message: error.message, code: error.code, codeName: error.codeName };
  }
  if (error instanceof Error) {
    return { name: error.name, message: error.message };
  }
  return { name: 'Error', message: String(error) };
}

const KNOWN_ERROR_CODES: ReadonlySet<number> = new Set(Object.values(ErrorCode));

/** A numeric code outside the {@link ErrorCode} table reads as `Unknown`. */
function narrowErrorCode(code: number): ErrorCode {
  return KNOWN_ERROR_CODES.has(code) ? (code as ErrorCode) : ErrorCode.Unknown;
}

/**
 * Rebuilds the error a {@link failurePayload} described: a coded SonareError
 * keeps its code (an unlisted number becomes `Unknown`), an argument refusal keeps its RangeError / TypeError class,
 * and any other failure becomes a SonareError with code `Unknown`.
 */
export function failureFromPayload(payload: SonareFailurePayload): Error {
  if (payload.code !== undefined) {
    return new SonareError(narrowErrorCode(payload.code), payload.message);
  }
  if (payload.name === 'RangeError') {
    return new RangeError(payload.message);
  }
  if (payload.name === 'TypeError') {
    return new TypeError(payload.message);
  }
  return new SonareError(ErrorCode.Unknown, payload.message);
}

/** Whether a value received over a port is a {@link SonareFailurePayload}. */
export function isFailurePayload(value: unknown): value is SonareFailurePayload {
  if (typeof value !== 'object' || value === null) {
    return false;
  }
  const candidate = value as Record<string, unknown>;
  return (
    typeof candidate.name === 'string' &&
    typeof candidate.message === 'string' &&
    (candidate.code === undefined || typeof candidate.code === 'number') &&
    (candidate.codeName === undefined || typeof candidate.codeName === 'string')
  );
}
