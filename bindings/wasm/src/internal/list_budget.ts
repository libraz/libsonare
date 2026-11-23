import { refuse } from './entry.js';

/** `kMaxWasmPlainArrayElements` in src/wasm/bindings/common/common.h, the native setters' list budget. */
const WASM_LIST_BUDGET = 64 * 1024 * 1024;

/**
 * Refuses a list longer than the native setter accepts, before any walk over
 * it: a sparse array can carry that length without holding the entries.
 */
export function requireListBudget(list: ArrayLike<unknown>, subject: string): void {
  if (list.length > WASM_LIST_BUDGET) {
    throw refuse(`${subject} exceeds the input length budget`);
  }
}

/**
 * `JSON.stringify` that refuses any array past the list budget, at every depth
 * and before it is walked, and a value too large to serialize at all, as past
 * the budget rather than as a raw `RangeError`.
 */
export function stringifyWithinBudget(value: unknown, subject: string): string {
  try {
    return JSON.stringify(value, (_key, item: unknown) => {
      if (Array.isArray(item)) {
        requireListBudget(item, subject);
      }
      return item;
    });
  } catch (error) {
    if (error instanceof RangeError) {
      throw refuse(`${subject} exceeds the input length budget`);
    }
    throw error;
  }
}
