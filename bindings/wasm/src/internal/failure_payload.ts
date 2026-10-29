import { isSonareError, SonareError } from '../errors.js';

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

/**
 * Rebuilds the error a {@link failurePayload} described: a coded SonareError
 * keeps its code, and an argument refusal keeps its RangeError / TypeError class.
 */
export function failureFromPayload(payload: SonareFailurePayload): Error {
  if (payload.code !== undefined) {
    return new SonareError(payload.code, payload.codeName ?? 'Unknown', payload.message);
  }
  const error =
    payload.name === 'RangeError'
      ? new RangeError(payload.message)
      : payload.name === 'TypeError'
        ? new TypeError(payload.message)
        : new Error(payload.message);
  error.name = payload.name;
  return error;
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
