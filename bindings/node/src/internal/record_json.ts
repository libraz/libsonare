/**
 * Parses a core JSON report into a result record. A `null` member is dropped,
 * so a value the report could not give is an absent field; array elements are kept.
 */
export function parseRecordJson<T>(json: string): T {
  return JSON.parse(json, function (this: unknown, _key: string, value: unknown) {
    return value === null && !Array.isArray(this) ? undefined : value;
  }) as T;
}
