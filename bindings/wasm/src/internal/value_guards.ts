/** The largest uint32: ids, counts and wire words the native side reads as `uint32_t`. */
export const UINT32_MAX = 0xffff_ffff;

/** The largest uint64 as a canonical decimal string; a uint64 travels as a string, never a `number`. */
export const UINT64_MAX = '18446744073709551615';

/** `undefined` or `null`: the two spellings of "not given" a facade accepts for an optional value. */
export function isAbsent(value: unknown): value is undefined | null {
  return value === undefined || value === null;
}

/** A non-null object that is not an array; a typed array or other view still passes. */
export function isNonArrayObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** A {@link isNonArrayObject} that is not an `ArrayBuffer` view either: an options or config record. */
export function isPlainRecord(value: unknown): value is Record<string, unknown> {
  return isNonArrayObject(value) && !ArrayBuffer.isView(value);
}
