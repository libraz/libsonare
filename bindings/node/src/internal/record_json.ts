import { ErrorCode } from '../errors.js';
import { codedError } from './entry.js';
import { NULL_MEANS } from './result_null_means.js';

/** A result family of `schemas/result-null-means.json`. */
export type ResultFamily = keyof typeof NULL_MEANS;

/** Parses a core JSON report into a result record of @p family; see {@link restoreNullMeans}. */
export function parseRecordJson<T>(json: string, family: ResultFamily): T {
  return restoreNullMeans(JSON.parse(json), family) as T;
}

/**
 * Parses a catalog document. A catalog `null` means "not applicable" (no bound, no choices),
 * so the member is dropped and reads as an absent field; array elements are kept.
 */
export function parseCatalogJson<T>(json: string): T {
  return JSON.parse(json, function (this: unknown, _key: string, value: unknown) {
    return value === null && !Array.isArray(this) ? undefined : value;
  }) as T;
}

/**
 * Replaces each `null` of a parsed report with the infinite end its family names for that path.
 * A key the report left out stays absent; a `null` at a path the family does not name is a
 * defect in the library's own output, refused rather than guessed at.
 */
export function restoreNullMeans(value: unknown, family: ResultFamily): unknown {
  const table = NULL_MEANS[family];
  const walk = (node: unknown, path: string): unknown => {
    if (node === null) {
      const end = table[path];
      if (end === undefined) {
        throw codedError(ErrorCode.InvalidFormat, `${family} report holds a null at ${path}`);
      }
      return end;
    }
    if (Array.isArray(node)) {
      return node.map((element) => walk(element, `${path}[]`));
    }
    if (typeof node === 'object') {
      const record = node as Record<string, unknown>;
      for (const key of Object.keys(record)) {
        record[key] = walk(record[key], path === '' ? key : `${path}.${key}`);
      }
    }
    return node;
  };
  return walk(value, '');
}
