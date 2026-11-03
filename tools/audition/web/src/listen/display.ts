/* Which versions the switch shows, in which order, and what the blind draw holds. */

import {
  comparisonKeys,
  ROLE_ORDER,
  roleOf,
  scopeOf,
  selectedComparison,
  sourceOf,
} from '../state/selectors';
import type { AppState } from '../state/store';

/* Slots in the order they are shown, which is the order 1…9 count in.
 *
 * A blind run is one question, run on one comparison's two sides. Outside one,
 * the selected comparison decides what is shown: its model side, its oracle,
 * and any capture offered beside them for context. */
export function displayOrder(s: AppState): number[] {
  if (s.blind) {
    return s.blindOrder.map((_, i) => i);
  }
  const keys = s.take?.keys ?? [];
  let slots = keys.map((_, i) => i);
  const c = selectedComparison(s);
  if (c) {
    const shown = comparisonKeys(c);
    const kept = slots.filter((slot) => {
      const key = keys[slot] ?? '';
      return shown.has(key) || roleOf(s, key) === 'comparison';
    });
    if (kept.length) {
      slots = kept;
    }
  }
  const rank = (slot: number) => {
    const key = keys[slot] ?? '';
    const role = roleOf(s, key);
    const at = role ? ROLE_ORDER.indexOf(role) : ROLE_ORDER.length;
    // The instrument scope after the product one within a role, unless the manifest marks which block is primary.
    const { block } = sourceOf(s, key);
    let sub = scopeOf(s, key) === 'instrument' ? 1 : 0;
    if (block) {
      sub = block === 'primary' ? 0 : 1;
    }
    return at * 2 + sub;
  };
  // Stable, so a role's own versions keep the order the manifest gave them.
  return slots
    .map((slot, i) => [slot, i] as const)
    .sort((a, b) => rank(a[0]) - rank(b[0]) || a[1] - b[1])
    .map(([slot]) => slot);
}

/* The draw for a blind run: the selected comparison's two sides and nothing
 * else, so a product run never holds an instrument render; a page without
 * comparisons draws the shipped path only. `random` is injectable for tests. */
export function blindDraw(s: AppState, random: () => number = Math.random): number[] {
  const keys = s.take?.keys ?? [];
  const c = selectedComparison(s);
  const drawn = c ? comparisonKeys(c) : null;
  const order = keys
    .map((_, i) => i)
    .filter((i) => {
      const key = keys[i] ?? '';
      return drawn ? drawn.has(key) : scopeOf(s, key) !== 'instrument';
    });
  for (let i = order.length - 1; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [order[i], order[j]] = [order[j] as number, order[i] as number];
  }
  return order;
}

/* A blind run is a ranking, so it runs only on a comparison whose two sides
 * stand at the same boundary. A page written before comparisons falls back to
 * the rule it had: a direct-input reference set against the amplified path is
 * exactly that mismatch. */
export function blindBlocked(s: AppState): boolean {
  const c = selectedComparison(s);
  if (c) {
    return c.status !== 'matched';
  }
  return s.manifest?.voice?.rig === 'none';
}

/** The slots belonging to a role, in display order. */
export const slotsInRole = (s: AppState, role: string): number[] =>
  s.display.filter((slot) => roleOf(s, s.take?.keys[slot]) === role);
