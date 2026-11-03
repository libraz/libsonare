/* Questions about the store, answered from the store alone.
 *
 * Every function here takes the state rather than reaching for it, so each one
 * is a pure function of what it is handed and is tested that way.
 */

import type { Comparison, Item, Role, Source } from '../api/schemas';
import type { AppState } from './store';

/** Source roles, in the order their rows are shown. `comparison` is heard beside the reference, never the target. */
export const ROLE_ORDER: readonly Role[] = ['model', 'reference', 'comparison'];

/** Seconds: instant to the ear, long enough not to click. */
export const SWITCH_RAMP = 0.006;
/** Strikes closer together than this are one audible event, the rule `shape/hits.py` groups by. */
export const FUSED_S = 0.035;

export const isRole = (r: unknown): r is Role => ROLE_ORDER.includes(r as Role);

export const sourceOf = (s: AppState, key: string): Source => s.manifest?.sources[key] ?? {};

export const sourceLabel = (s: AppState, key: string): string => sourceOf(s, key).label || key;

/** A source's role, or '' for a version the manifest gives none. */
export function roleOf(s: AppState, key: string | null | undefined): Role | '' {
  if (!key) {
    return '';
  }
  const r = sourceOf(s, key).role;
  return isRole(r) ? r : '';
}

/** The class that carries a role's colour; one spelling, so the stylesheet is the only other place it appears. */
export const roleClass = (role: string): string => `role-${isRole(role) ? role : 'other'}`;

/* Which boundary a model render stops at — an axis rather than a choice. */
export const scopeOf = (s: AppState, key: string): string => sourceOf(s, key).scope || '';

/** The block a version belongs to: one question, one block. */
export const blockOf = (s: AppState, key: string): string => `${roleOf(s, key)}|${scopeOf(s, key)}`;

/** The comparisons the manifest defines; a page written before them has none. */
export const comparisons = (s: AppState): Comparison[] =>
  (s.manifest?.comparisons ?? []).filter((c) => c?.id);

const STATUS_RANK: Record<string, number> = {
  matched: 0,
  context_only: 1,
  unverified: 2,
  unavailable: 3,
};

/** The comparison being listened to: the one chosen, else the best-standing one, product before instrument. */
export function selectedComparison(s: AppState): Comparison | null {
  const all = comparisons(s);
  const held = all.find((c) => c.id === s.comparisonId);
  if (held) {
    return held;
  }
  if (!all.length) {
    return null;
  }
  const rank = (c: Comparison) =>
    (STATUS_RANK[c.status] ?? 4) * 2 + (c.scope === 'product' ? 0 : 1);
  return all.reduce((best, c) => (rank(c) < rank(best) ? c : best));
}

/** The versions one comparison sets against each other: its model side and its oracle. */
export const comparisonKeys = (c: Comparison): Set<string> =>
  new Set([...c.model_sources, ...c.oracle_sources]);

export interface OracleSource {
  role: Role;
  key: string;
  label: string;
}

/** Every source a note could be judged against, reference before comparison; with a take open, only its own. */
export function oracleSources(s: AppState): OracleSource[] {
  const sources = s.manifest?.sources ?? {};
  return (['reference', 'comparison'] as const).flatMap((role) =>
    Object.keys(sources)
      .filter((key) => sources[key]?.role === role && (!s.take || s.take.keys.includes(key)))
      .map((key) => ({ role, key, label: sourceLabel(s, key) })),
  );
}

/** The manifest's generation digest, or null for a page written before it. */
export const setGeneration = (s: AppState): string | null => s.manifest?.set_generation ?? null;

export const currentItem = (s: AppState): Item | null => s.items[s.itemIndex] ?? null;

/** The version sounding: the slot read through the draw when blind. */
export function activeKey(s: AppState): string {
  if (!s.take) {
    return '';
  }
  const idx = s.blind ? s.blindOrder[s.versionIndex] : s.versionIndex;
  return (idx !== undefined && s.take.keys[idx]) || '';
}

/** The letter a blind slot is shown under. */
export const blindLetter = (s: AppState, slot: number): string =>
  String.fromCharCode(65 + Math.max(0, s.display.indexOf(slot)));

/* ---------------------------------------------------------- local storage */

export const notesKey = (s: AppState): string => `audition:picks-note:${s.setId || 'untitled'}`;

const picksBase = (s: AppState, c: Comparison | null): string =>
  `audition:picks:${s.setId || 'untitled'}${c ? `:${c.id}` : ''}`;

/** Picks are held per comparison and per generation; a page with neither keeps the key it always had. */
export function picksKey(s: AppState): string {
  const gen = setGeneration(s);
  return picksBase(s, selectedComparison(s)) + (gen ? `@${gen.slice(0, 16)}` : '');
}

/** Keys holding picks for this set and comparison under another generation or an older key. Shown apart, never tallied. */
export function earlierPicksKeys(s: AppState, storage: Storage = localStorage): string[] {
  const current = picksKey(s);
  const c = selectedComparison(s);
  const base = picksBase(s, c);
  const out: string[] = [];
  for (let i = 0; i < storage.length; i++) {
    const key = storage.key(i);
    if (key === null || key === current) {
      continue;
    }
    if (key === base || key.startsWith(`${base}@`)) {
      out.push(key);
    }
  }
  // The pre-comparison key held the product draw, so only that comparison inherits it.
  const bare = picksBase(s, null);
  if (c && c.scope === 'product' && storage.getItem(bare) !== null) {
    out.push(bare);
  }
  return out;
}

/** The set index row for a slug, and whether its unmodified render ships today. */
export const setOf = (s: AppState, slug: string | null) =>
  s.sets.find((x) => x.id === slug) ?? null;
export const shipsOf = (s: AppState, slug: string | null) => setOf(s, slug)?.ships ?? null;
