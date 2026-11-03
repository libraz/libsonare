/* The bank's questions with no screen in them: which rows a query keeps, in what order, and the
 * totals the masthead shows. Pure over the voices and a view of which pages exist.
 */

import type { SetEntry } from '../api/schemas';
import { type Method, methodOf, type SlotKind, slotKind } from '../voice/identity';
import type { BankVoiceRow } from './schema';

export type Rung = 0 | 1 | 2 | 3 | 4 | 5;
/** The six rungs, whose wording is argued in `tools/voicematch/docs/acceptance.md`. */
export const RUNGS: readonly Rung[] = [0, 1, 2, 3, 4, 5];

export type BankFilter =
  | 'oracle'
  | 'no-oracle'
  | 'unwritten'
  | 'page'
  | 'stale'
  | 'current'
  | 't-instrument'
  | 't-machine';
export type SortKey = 'address' | 'stage' | 'name';

export interface BankQuery {
  method: Method | '';
  slot: SlotKind | '';
  filter: BankFilter | '';
  /** A rung index, or null for every rung. */
  stage: Rung | null;
  find: string;
  sort: SortKey;
}

export const emptyQuery = (): BankQuery => ({
  method: '',
  slot: '',
  filter: '',
  stage: null,
  find: '',
  sort: 'address',
});

export const stageIndex = (v: { stage: number }): Rung =>
  Math.max(0, Math.min(5, Math.round(v.stage * 5))) as Rung;

/** Which pages are rendered and whether each plays today's voice. */
export interface PageContext {
  hasPage(slug: string): boolean;
  /** 'current', 'stale', … or '' with no page. */
  shipState(slug: string): string;
}

export function pageContext(sets: SetEntry[]): PageContext {
  const byId = new Map(sets.map((s) => [s.id, s]));
  return {
    hasPage: (slug) => byId.has(slug),
    shipState: (slug) => byId.get(slug)?.ships?.state ?? '',
  };
}

export function matches(v: BankVoiceRow, q: BankQuery, ctx: PageContext): boolean {
  const find = q.find.trim().toLowerCase();
  if (q.method && methodOf(v.engine) !== q.method) {
    return false;
  }
  if (q.slot && slotKind(v) !== q.slot) {
    return false;
  }
  if (q.stage !== null && stageIndex(v) !== q.stage) {
    return false;
  }
  switch (q.filter) {
    case 'oracle':
      if (!v.capture) {
        return false;
      }
      break;
    case 'no-oracle':
      if (v.capture) {
        return false;
      }
      break;
    case 'unwritten':
      if (!v.open_candidates.length) {
        return false;
      }
      break;
    case 'page':
      if (!ctx.hasPage(v.slug)) {
        return false;
      }
      break;
    case 'stale':
    case 'current':
      if (ctx.shipState(v.slug) !== q.filter) {
        return false;
      }
      break;
    case 't-instrument':
    case 't-machine':
      if (v.target?.timbre !== q.filter.slice(2)) {
        return false;
      }
      break;
    default:
      break;
  }
  if (find) {
    const hay = [v.name, v.engine, v.patch, v.slug, v.tone_class].filter(Boolean).join(' ');
    if (!hay.toLowerCase().includes(find)) {
      return false;
    }
  }
  return true;
}

const SORTS: Record<SortKey, ((a: BankVoiceRow, b: BankVoiceRow, lang: string) => number) | null> =
  {
    // The generated order, which is already program order.
    address: null,
    stage: (a, b) => b.stage - a.stage || a.program - b.program,
    name: (a, b, lang) => a.name.localeCompare(b.name, lang),
  };

/** The rows a query keeps, in the order it asks for. */
export function shownVoices(
  voices: BankVoiceRow[],
  q: BankQuery,
  ctx: PageContext,
  lang: string,
): BankVoiceRow[] {
  const kept = voices.filter((v) => matches(v, q, ctx));
  const cmp = SORTS[q.sort];
  return cmp ? kept.sort((a, b) => cmp(a, b, lang)) : kept;
}

/** The cursor's index in the shown rows: the voice it holds if still shown, else the first row. */
export function cursorIndex(shown: BankVoiceRow[], cursor: string | null): number {
  const at = cursor === null ? -1 : shown.findIndex((v) => v.slug === cursor);
  return at >= 0 ? at : shown.length ? 0 : -1;
}

/** How many of the voices sit on each rung. */
export function stageCounts(voices: { stage: number }[]): number[] {
  const counts = RUNGS.map(() => 0);
  for (const v of voices) {
    counts[stageIndex(v)] = (counts[stageIndex(v)] ?? 0) + 1;
  }
  return counts;
}

export interface BankStats {
  voices: number;
  withOracle: number;
  unwritten: number;
  pages: number;
  current: number;
  stale: number;
  aimInstrument: number;
  aimMachine: number;
  perStage: number[];
}

export function bankStats(voices: BankVoiceRow[], ctx: PageContext): BankStats {
  const count = (pred: (v: BankVoiceRow) => boolean) => voices.filter(pred).length;
  return {
    voices: voices.length,
    withOracle: count((v) => !!v.capture),
    unwritten: voices.reduce((n, v) => n + v.open_candidates.length, 0),
    pages: count((v) => ctx.hasPage(v.slug)),
    current: count((v) => ctx.shipState(v.slug) === 'current'),
    stale: count((v) => ctx.shipState(v.slug) === 'stale'),
    aimInstrument: count((v) => v.target?.timbre === 'instrument'),
    aimMachine: count((v) => v.target?.timbre === 'machine'),
    perStage: stageCounts(voices),
  };
}
