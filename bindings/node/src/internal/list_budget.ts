import { refuse } from './entry.js';

/** `kNodeListBudget` in src/addon/sonare_wrap_options.h, the addon's list length budget. */
const LIST_BUDGET = 64 * 1024 * 1024;

function overBudget(subject: string): RangeError {
  return refuse(`${subject} exceeds the input length budget`);
}

/**
 * Refuses a list longer than the addon accepts, before any walk over it: a
 * sparse array can carry that length without holding the entries.
 */
export function requireListBudget(list: ArrayLike<unknown>, subject: string): void {
  if (list.length > LIST_BUDGET) {
    throw overBudget(subject);
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
      throw overBudget(subject);
    }
    throw error;
  }
}
