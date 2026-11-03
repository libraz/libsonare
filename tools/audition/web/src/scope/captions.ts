import { roleOf } from '../state/selectors';
import type { AppState } from '../state/store';

/** The roles of the versions on show, once each, in the order the page lists them. */
export function legendRoles(s: AppState): string[] {
  if (!s.take || s.blind || !s.compare) {
    return [];
  }
  const seen = new Set<string>();
  for (const slot of s.display) {
    seen.add(roleOf(s, s.take.keys[slot]) || 'other');
  }
  return [...seen];
}
