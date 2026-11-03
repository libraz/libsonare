/* Which sets the voice palette lists, and under which headings. Pure over the set index and the
 * bank rows, so the ordering is tested without a screen.
 */

import type { SetEntry } from '../api/schemas';
import type { BankVoiceRow } from '../bank/schema';

export interface PaletteRow {
  set: SetEntry;
  /** The bank's row for the set; a set served from outside the repository has none. */
  voice: BankVoiceRow | null;
  /** The heading this row opens, or '' when it continues the previous group. */
  heading: string;
}

/** Searched over everything on the row: a voice is looked for by program number and by engine too. */
export function paletteRows(sets: SetEntry[], voices: BankVoiceRow[], find: string): PaletteRow[] {
  const needle = find.trim().toLowerCase();
  const bySlug = new Map(voices.map((v) => [v.slug, v]));
  const out: PaletteRow[] = [];
  let group: string | null = null;
  for (const set of sets) {
    const voice = bySlug.get(set.id) ?? null;
    const hay = [set.id, set.title, set.group, voice?.name, voice?.patch, voice?.engine]
      .filter(Boolean)
      .join(' ')
      .toLowerCase();
    if (needle && !hay.includes(needle)) {
      continue;
    }
    let heading = '';
    if ((set.group || '') !== group) {
      group = set.group || '';
      heading = group;
    }
    out.push({ set, voice, heading });
  }
  return out;
}
