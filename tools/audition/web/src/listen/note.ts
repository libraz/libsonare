/* Everything that decides what the ear actually met, as data.
 *
 * An address names the set, the take and the version and stops there, so where
 * the playhead was, which strike that is, whether the levels were matched and
 * whether the names were hidden would otherwise be described from memory. One
 * producer, two consumers — the clipboard button and the note the page sends —
 * so a report typed into the page and one pasted out of it carry the same
 * fields. Every function takes the state and the playhead it is about.
 */

import type {
  ComparedAgainst,
  Conditions,
  Evaluation,
  EvidenceClaim,
  TakeNote,
} from '../api/schemas';
import {
  activeKey,
  currentItem,
  FUSED_S,
  roleOf,
  selectedComparison,
  setGeneration,
  sourceLabel,
} from '../state/selectors';
import type { AppState } from '../state/store';

export interface Hit {
  /** Numbered from 1 in time order. */
  n: number;
  start: number;
  notes: TakeNote[];
}

/** The take's schedule as numbered strikes, which is what a note has to be able to name. */
export function takeHits(s: AppState): Hit[] {
  const notes = currentItem(s)?.meta?.notes;
  if (!notes?.length) {
    return [];
  }
  const out: Hit[] = [];
  for (const n of [...notes].sort((a, b) => a.start - b.start || a.note - b.note)) {
    const last = out[out.length - 1];
    if (last && n.start - last.start <= FUSED_S) {
      last.notes.push(n);
    } else {
      out.push({ n: out.length + 1, start: n.start, notes: [n] });
    }
  }
  return out;
}

/** The strike the playhead is inside, or null before the first one. */
export function hitAt(s: AppState, at: number): Hit | null {
  let cur: Hit | null = null;
  for (const hit of takeHits(s)) {
    if (hit.start <= at + 0.005) {
      cur = hit;
    }
  }
  return cur;
}

/** The library version last heard in this take, which is what a note written while the reference sounds is about. */
export function lastModelKey(s: AppState): string | null {
  const keys = s.take?.keys ?? [];
  const slot = s.lastByRole.model;
  const held = slot !== undefined ? keys[slot] : undefined;
  if (held && roleOf(s, held) === 'model') {
    return held;
  }
  return keys.find((k) => roleOf(s, k) === 'model') ?? null;
}

/** The oracle a note is judged against: the listener's override, else the last selected, else null rather than a guess. */
export function comparedAgainst(s: AppState): ComparedAgainst | null {
  const picked = s.oracleOverride ?? s.lastOracle;
  if (!picked) {
    return null;
  }
  return {
    role: picked.role,
    version: picked.key,
    label: sourceLabel(s, picked.key),
    chosen: s.oracleOverride ? 'manual' : 'last_selected',
  };
}

/* What a note judges. `judged_source` is the library render the note is about
 * — the one sounding, or the last one heard while a reference plays — and is
 * recorded in blind mode too, where the page hides it but the log has to say
 * what was heard. The server re-reads scope and status from its manifest. */
export function evaluation(
  s: AppState,
  { attached = true }: { attached?: boolean } = {},
): Evaluation {
  const c = selectedComparison(s);
  const sounding = s.take ? activeKey(s) : null;
  let judged: string | null = null;
  if (attached && sounding) {
    const role = roleOf(s, sounding);
    judged = role === 'model' || !role ? sounding : lastModelKey(s);
  }
  const ca = comparedAgainst(s);
  return {
    comparison_id: c?.id ?? null,
    scope: c?.scope || null,
    judged_source: judged,
    oracle_source: ca?.version ?? null,
    comparison_status: c?.status || null,
    blind: s.blind,
  };
}

/** Which recording a note claims to be about; the server refuses a claim made against an older generation. */
export function evidenceClaim(s: AppState, source: string | null): EvidenceClaim {
  return {
    set_generation: setGeneration(s),
    take: currentItem(s)?.id ?? null,
    source: source || null,
  };
}

const r3 = (x: number) => Number(x.toFixed(3));

export function conditions(s: AppState, at: number): Conditions {
  const item = currentItem(s);
  const hit = hitAt(s, at);
  const version = s.blind || !s.take ? null : activeKey(s) || null;
  const comparison = selectedComparison(s);
  return {
    set: s.setId,
    comparison_id: comparison?.id ?? null,
    take: item?.id ?? null,
    take_label: item?.label || null,
    version,
    against: version && roleOf(s, version) === 'reference' ? lastModelKey(s) : null,
    compared_against: comparedAgainst(s),
    blind: s.blind,
    playhead: r3(at),
    duration: s.take ? r3(s.take.duration) : null,
    hit: hit
      ? {
          n: hit.n,
          start: r3(hit.start),
          into: r3(at - hit.start),
          notes: hit.notes.map((n) => ({ note: n.note, velocity: n.velocity })),
        }
      : null,
    region: s.region ? [r3(s.region[0]), r3(s.region[1])] : null,
    match_loudness: s.matchRms,
    loop: s.loop,
  };
}
