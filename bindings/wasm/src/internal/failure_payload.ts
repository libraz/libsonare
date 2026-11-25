import { ErrorCode, isSonareError } from '../errors.js';
import {
  attachRefusal,
  bareText,
  codedError,
  type RefusalRecord,
  refusalOf,
  refuse,
  refuseType,
} from './entry.js';

/** A failure as it crosses a postMessage boundary: class name, message and any SonareError code. */
export interface SonareFailurePayload {
  name: string;
  message: string;
  code?: number;
  codeName?: string;
  /** The refusal text before any entry named itself, so the receiving entry can name it. */
  bare?: string;
  /** The native refusal's parts; symbol properties do not cross structured clone. */
  refusal?: RefusalRecord;
}

/** The message of an `Error`, or the string form of any other thrown value. */
export function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** Captures `error` for posting, keeping a SonareError's code and codeName. */
export function failurePayload(error: unknown): SonareFailurePayload {
  // Nothing on the far side of a port is an entry the consumer called, so its text crosses bare.
  const bare = bareText(error) ?? (error instanceof Error ? error.message : undefined);
  const refusal = refusalOf(error);
  const tail = {
    ...(bare === undefined ? {} : { bare }),
    ...(refusal === undefined ? {} : { refusal }),
  };
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

function isRefusalRecord(value: unknown): value is RefusalRecord {
  if (typeof value !== 'object' || value === null) {
    return false;
  }
  const record = value as Partial<RefusalRecord>;
  if (
    !Number.isInteger(record.code) ||
    (record.form !== 'predicate' &&
      record.form !== 'wrongType' &&
      record.form !== 'unknownKey' &&
      record.form !== 'relation') ||
    !Array.isArray(record.subjects) ||
    record.subjects.length === 0 ||
    !Array.from(record.subjects).every(
      (subject: unknown) => typeof subject === 'string' && !subject.includes('\0'),
    ) ||
    typeof record.predicate !== 'string' ||
    typeof record.expected !== 'string' ||
    typeof record.text !== 'string' ||
    typeof record.callerSpelled !== 'boolean' ||
    (record.cEntry != null && typeof record.cEntry !== 'string')
  ) {
    return false;
  }
  const domain = record.domain;
  return (
    domain === undefined ||
    (typeof domain === 'object' &&
      domain !== null &&
      (domain.lo === undefined || Number.isFinite(domain.lo)) &&
      (domain.hi === undefined || Number.isFinite(domain.hi)) &&
      typeof domain.loIncluded === 'boolean' &&
      typeof domain.hiIncluded === 'boolean')
  );
}

/** Rebuilds the error class and preserves the refusal for the receiving entry to name. */
export function failureFromPayload(payload: SonareFailurePayload): Error {
  const text = payload.bare ?? payload.message;
  let error: Error;
  if (payload.code !== undefined) {
    error = codedError(narrowErrorCode(payload.code), text);
  } else if (payload.name === 'RangeError') {
    error = refuse(text);
  } else if (payload.name === 'TypeError') {
    error = refuseType(text);
  } else {
    error = codedError(ErrorCode.Unknown, text);
  }
  return isRefusalRecord(payload.refusal) ? attachRefusal(error, payload.refusal) : error;
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
