/**
 * Every public callable whose parameters reach a caller-built list is either exercised by a
 * budget row, exempt with a reason, or named as a known gap. The callables come from the package's
 * own type program, so a new list-taking method cannot slip past the budget evidence unseen.
 */

import { describe, expect, it } from 'vitest';
import { LIST_BUDGET_COVERED } from './_list_budget_rows.js';
import { enumerateCallables } from './_list_taking_callables.js';

const ENTRIES = ['index.ts'];

/** Callables with no row, and why none is owed. */
const EXEMPT: Record<string, string> = {
  'FileClipPageProvider.supply':
    'inherited unchanged from ClipPageProvider.supply, whose row exercises the shared body',
};

const UNGUARDED = 'facade walks the list unguarded; fix pending';

/** Callables that walk a caller list in the facade with no budget check, by the walking site. */
const KNOWN_UNGUARDED: Record<string, string> = {
  splitNote: `${UNGUARDED} (effects_note_ops.ts assertNoteSetEntries and resolveEntryTimes over notes)`,
  mergeNotes: `${UNGUARDED} (effects_note_ops.ts assertNoteSetEntries and resolveEntryTimes over notes)`,
  'Project.rehydrateVocalEdits': `${UNGUARDED} (vocal_project.ts projectRehydrateVocalEdits index loop over originals)`,
};

const MIN_LIST_TAKING = 88;
const MIN_CALLABLES = 636;

describe('list-taking public callables are covered by the caller-list budget evidence', () => {
  const callables = enumerateCallables(ENTRIES);
  const listTaking = [...callables.values()].filter((c) => c.listTaking);
  const covered = new Set(LIST_BUDGET_COVERED);

  it('finds enough of the surface for the walk to be trusted', () => {
    expect(callables.size).toBeGreaterThanOrEqual(MIN_CALLABLES);
    expect(listTaking.length).toBeGreaterThanOrEqual(MIN_LIST_TAKING);
  });

  it('leaves no list-taking callable without a row, an exemption or a recorded gap', () => {
    const missing = listTaking
      .filter((c) => !covered.has(c.name) && !(c.name in EXEMPT) && !(c.name in KNOWN_UNGUARDED))
      .map((c) => `${c.name} (${c.where}): ${c.listParams.join(', ')}`);
    expect(missing, 'add a row to _list_budget_rows.ts, or an EXEMPT reason').toEqual([]);
  });

  it('holds no stale row, exemption or recorded gap', () => {
    // A row may name a callable whose declared types carry no list (the facade accepts one the
    // types do not name); it must still exist. An exemption or a recorded gap must be list-taking.
    const live = new Set(listTaking.map((c) => c.name));
    const stale = [
      ...[...covered].filter((n) => !callables.has(n)).map((n) => `row ${n}`),
      ...Object.keys(EXEMPT)
        .filter((n) => !live.has(n))
        .map((n) => `EXEMPT ${n}`),
      ...Object.keys(KNOWN_UNGUARDED)
        .filter((n) => !live.has(n))
        .map((n) => `KNOWN_UNGUARDED ${n}`),
    ];
    expect(stale).toEqual([]);
  });

  it('keeps an exemption apart from a row and from a recorded gap', () => {
    const overlap = Object.keys(EXEMPT).filter((n) => covered.has(n) || n in KNOWN_UNGUARDED);
    expect(overlap).toEqual([]);
  });

  it('gives every exemption a reason', () => {
    for (const [name, reason] of Object.entries(EXEMPT)) {
      expect(reason.length, name).toBeGreaterThan(20);
    }
  });
});
