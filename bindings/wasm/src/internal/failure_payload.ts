import { ErrorCode, isSonareError } from '../errors.js';
import { bareText, codedError, refuse, refuseType } from './entry.js';

/** A failure as it crosses a postMessage boundary: class name, message and any SonareError code. */
export interface SonareFailurePayload {
  name: string;
  message: string;
  code?: number;
  codeName?: string;
  /** The refusal text before any entry named itself, so the receiving entry can name it. */
  bare?: string;
}

/** Captures `error` for posting, keeping a SonareError's code and codeName. */
export function failurePayload(error: unknown): SonareFailurePayload {
  // Nothing on the far side of a port is an entry the consumer called, so its text crosses bare.
  const bare = bareText(error) ?? (error instanceof Error ? error.message : undefined);
  const tail = bare === undefined ? {} : { bare };
  if (isSonareError(error)) {
    return {
      name: error.name,
      message: error.message,
      code: error.code,
      codeName: error.codeName,
      ...tail,
    };
  }
  if (error instanceof Error) {
    return { name: error.name, message: error.message, ...tail };
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
 * and any other failure becomes a SonareError with code `Unknown`. A refusal that crossed with its
 * bare text arrives bare, so the entry the caller invoked on this side names it.
 */
export function failureFromPayload(payload: SonareFailurePayload): Error {
  if (payload.bare !== undefined) {
    const text = payload.bare;
    if (payload.code !== undefined) {
      return codedError(narrowErrorCode(payload.code), text);
    }
    if (payload.name === 'RangeError') {
      return refuse(text);
    }
    if (payload.name === 'TypeError') {
      return refuseType(text);
    }
    return codedError(ErrorCode.Unknown, text);
  }
  if (payload.code !== undefined) {
    return codedError(narrowErrorCode(payload.code), payload.message);
  }
  if (payload.name === 'RangeError') {
    return refuse(payload.message);
  }
  if (payload.name === 'TypeError') {
    return refuseType(payload.message);
  }
  return codedError(ErrorCode.Unknown, payload.message);
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
    (candidate.codeName === undefined || typeof candidate.codeName === 'string') &&
    (candidate.bare === undefined || typeof candidate.bare === 'string')
  );
}
