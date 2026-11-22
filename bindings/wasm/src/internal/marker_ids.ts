import { refuse } from './entry.js';

/** `kMaxWasmFloat32Elements` in src/wasm/bindings/common/common.h, the native setter's list budget. */
const MARKER_LIST_BUDGET = 64 * 1024 * 1024;

/**
 * Refuses a marker list longer than the native setter accepts, before any walk
 * over it: a sparse array can carry that length without holding the entries.
 */
export function requireMarkerListBudget(markers: ReadonlyArray<unknown>): void {
  if (markers.length > MARKER_LIST_BUDGET) {
    throw refuse('markers exceeds the WASM Float32 input budget');
  }
}

/** A marker entry whose `id` is optional; an absent id is allocated. */
export type MarkerIdInput = { id?: number };

/**
 * Gives every id-less marker the lowest id at or above `nextId` that no entry
 * of the list carries, keeping explicit ids. Validation of explicit ids is the
 * setter's; entries that are not objects pass through for it to refuse.
 *
 * @returns The entries in the order given, and the counter for the next call
 *   (above every id the list now holds).
 */
export function assignMarkerIds<T extends MarkerIdInput>(
  markers: ReadonlyArray<T>,
  nextId: number,
): { markers: Array<T & { id: number }>; nextId: number } {
  const taken = new Set<number>();
  for (const marker of markers) {
    if (typeof marker === 'object' && marker !== null && typeof marker.id === 'number') {
      taken.add(marker.id);
    }
  }
  let next = nextId;
  let highest = nextId - 1;
  const out = markers.map((marker) => {
    if (typeof marker !== 'object' || marker === null) {
      return marker as T & { id: number };
    }
    if (marker.id !== undefined) {
      if (Number.isInteger(marker.id) && marker.id > highest) {
        highest = marker.id;
      }
      return marker as T & { id: number };
    }
    while (taken.has(next)) {
      next++;
    }
    taken.add(next);
    highest = Math.max(highest, next);
    return { ...marker, id: next++ };
  });
  return { markers: out, nextId: highest + 1 };
}
