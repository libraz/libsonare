import { refuse } from './entry.js';

/** `kMaxWasmFloat32Elements` in src/wasm/bindings/common/common.h, the native setters' list budget. */
const WASM_LIST_BUDGET = 64 * 1024 * 1024;

/**
 * Refuses a list longer than the native setter accepts, before any walk over
 * it: a sparse array can carry that length without holding the entries.
 */
export function requireListBudget(list: ArrayLike<unknown>, subject: string): void {
  if (list.length > WASM_LIST_BUDGET) {
    throw refuse(`${subject} exceeds the WASM Float32 input budget`);
  }
}

/** `JSON.stringify` that refuses a value too large to serialize as past the list budget, not as a raw `RangeError`. */
export function stringifyWithinBudget(value: unknown, subject: string): string {
  try {
    return JSON.stringify(value);
  } catch (error) {
    if (error instanceof RangeError) {
      throw refuse(`${subject} exceeds the WASM Float32 input budget`);
    }
    throw error;
  }
}
