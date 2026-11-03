import { describe, expect, it } from 'vitest';
import { Comparison, Manifest, NotePost, Queue, SetIndex } from '../../src/api/schemas';
import { blindBlocked, blindDraw, displayOrder, slotsInRole } from '../../src/listen/display';
import {
  comparedAgainst,
  conditions,
  evaluation,
  evidenceClaim,
  hitAt,
  lastModelKey,
  takeHits,
} from '../../src/listen/note';
import { readRoute, routeHash } from '../../src/listen/route';
import {
  activeKey,
  earlierPicksKeys,
  oracleSources,
  picksKey,
  roleClass,
  roleOf,
  selectedComparison,
} from '../../src/state/selectors';
import { getState, setState } from '../../src/state/store';
import { comparedManifest, opened, soloManifest, takeOf } from '../fixtures';

const keyAt = (slot: number) => getState().take?.keys[slot];

describe('the schemas', () => {
  it('fill the defaults a page written before them relies on', () => {
    const m = Manifest.parse({ items: [{ id: 'a' }] });
    expect(m.sources).toEqual({});
    expect(m.comparisons).toEqual([]);
    expect(m.items[0]?.tracks).toEqual({});
    expect(Comparison.parse({ id: 'x' }).status).toBe('unavailable');
  });

  it('take a set the server can say nothing about shipping for', () => {
    // serve.shipping returns {} for a set outside the repository or without a baseline render.
    const [row] = SetIndex.parse([{ id: 'x', title: 'x', takes: 1, compare: false, ships: {} }]);
    expect(row?.ships).toEqual({});
  });

  it('take the nulls a real manifest writes for what is unknown', () => {
    const m = Manifest.parse({
      title: null,
      voice: { program: 0, rig: null, patch: null },
      sources: { model: { label: 'x', role: 'model', scope: null, title: null } },
      items: [{ id: 'a', label: null, tracks: { model: 'a.wav' } }],
    });
    expect(m.voice?.rig).toBeNull();
    expect(m.items[0]?.label).toBeNull();
  });

  it('keep fields the page does not read', () => {
    const m = Manifest.parse({ items: [], provenance: { unit: 'u' }, extra: 1 });
    expect((m as Record<string, unknown>).extra).toBe(1);
  });

  it('refuse a malformed index, queue or manifest at the boundary', () => {
    expect(() => SetIndex.parse([{ id: 1 }])).toThrow();
    expect(() => Queue.parse([{ set: 'x', page: 'later' }])).toThrow();
    expect(() => Manifest.parse({ items: [{ tracks: {} }] })).toThrow();
  });

  it('refuse a note carrying a field the log does not hold', () => {
    const s = opened();
    const base = {
      schema_version: 2 as const,
      lang: 'en',
      grade: 'ok',
      tag: 'ok',
      answers: [],
      text: '',
      conditions: conditions(s, 0),
      evaluation: evaluation(s),
      evidence: evidenceClaim(s, 'model'),
    };
    expect(NotePost.parse(base)).toEqual(base);
    expect(() => NotePost.parse({ ...base, stray: 1 })).toThrow();
    expect(() =>
      NotePost.parse({ ...base, conditions: { ...base.conditions, stray: 1 } }),
    ).toThrow();
    expect(() => NotePost.parse({ ...base, schema_version: 1 })).toThrow();
  });
});

describe('the comparison in force', () => {
  it('is the best-standing one, product before instrument at equal standing', () => {
    expect(selectedComparison(opened())?.id).toBe('gm_gs_product');
    const tied = comparedManifest({
      comparisons: [
        Comparison.parse({ id: 'instrument_di', scope: 'instrument', status: 'matched' }),
        Comparison.parse({ id: 'gm_gs_product', scope: 'product', status: 'matched' }),
      ],
    });
    expect(selectedComparison(opened(tied))?.id).toBe('gm_gs_product');
  });

  it('is the chosen one once chosen', () => {
    expect(
      selectedComparison(opened(comparedManifest(), { comparisonId: 'instrument_di' }))?.id,
    ).toBe('instrument_di');
  });

  it('is none on a page without comparisons', () => {
    expect(selectedComparison(opened(soloManifest()))).toBeNull();
  });
});

describe('the switch', () => {
  it("shows the comparison's two sides plus context captures, model before reference", () => {
    const s = opened();
    setState({ display: displayOrder(s) });
    expect(displayOrder(s).map(keyAt)).toEqual(['model', 'cand-a', 'ref', 'ctx']);
    expect(slotsInRole(getState(), 'reference').map(keyAt)).toEqual(['ref']);
  });

  it('follows the other comparison when it is chosen', () => {
    const s = opened(comparedManifest(), { comparisonId: 'instrument_di' });
    expect(displayOrder(s).map(keyAt)).toEqual(['model-di', 'ref-di', 'ctx']);
  });

  it('puts the instrument scope after the product one within a role on a page without comparisons', () => {
    const s = opened(comparedManifest({ comparisons: [] }));
    expect(displayOrder(s).map(keyAt)).toEqual([
      'model',
      'cand-a',
      'model-di',
      'ref',
      'ref-di',
      'ctx',
    ]);
  });

  it('counts positions in the draw when blind', () => {
    const s = opened(comparedManifest(), { blind: true, blindOrder: [3, 0, 2] });
    expect(displayOrder(s)).toEqual([0, 1, 2]);
    setState({ versionIndex: 1 });
    expect(activeKey(getState())).toBe('model');
  });
});

describe('the blind draw', () => {
  it("holds exactly the selected comparison's two sides", () => {
    const s = opened();
    const drawn = blindDraw(s, () => 0.5)
      .map(keyAt)
      .sort();
    expect(drawn).toEqual(['cand-a', 'model', 'ref']);
  });

  it('draws the shipped path only on a page without comparisons', () => {
    const s = opened(comparedManifest({ comparisons: [] }));
    expect(
      blindDraw(s, () => 0.1)
        .map(keyAt)
        .sort(),
    ).toEqual(['cand-a', 'ctx', 'model', 'ref', 'ref-di']);
  });

  it('is shuffled by the random source', () => {
    const s = opened();
    expect(blindDraw(s, () => 0)).not.toEqual(blindDraw(s, () => 0.99));
  });

  it('is refused on an unmatched comparison and on a DI reference page without comparisons', () => {
    expect(blindBlocked(opened())).toBe(false);
    expect(blindBlocked(opened(comparedManifest(), { comparisonId: 'instrument_di' }))).toBe(true);
    expect(blindBlocked(opened(soloManifest()))).toBe(true);
  });
});

describe('what a note says it heard', () => {
  it('numbers strikes in time order and fuses ones closer than 35 ms', () => {
    const s = opened();
    const hits = takeHits(s);
    expect(hits.map((h) => [h.n, h.start, h.notes.map((n) => n.note)])).toEqual([
      [1, 0, [60, 67]],
      [2, 0.5, [64]],
      [3, 1, [72]],
    ]);
    expect(hitAt(s, 0.7)?.n).toBe(2);
    expect(hitAt(s, 0.497)?.n).toBe(2);
    expect(hitAt(s, -0.1)).toBeNull();
  });

  it('judges the model last heard while a reference sounds', () => {
    const s = opened(comparedManifest(), { versionIndex: 3, lastByRole: { model: 2 } });
    expect(activeKey(s)).toBe('ref');
    expect(lastModelKey(s)).toBe('cand-a');
    expect(evaluation(s).judged_source).toBe('cand-a');
    expect(conditions(s, 1.2345).against).toBe('cand-a');
  });

  it('judges nothing when not attached', () => {
    expect(evaluation(opened(), { attached: false }).judged_source).toBeNull();
  });

  it('records the listener override over the last selected oracle', () => {
    let s = opened(comparedManifest(), { lastOracle: { role: 'reference', key: 'ref' } });
    expect(comparedAgainst(s)).toEqual({
      role: 'reference',
      version: 'ref',
      label: 'Recording One',
      chosen: 'last_selected',
    });
    s = opened(comparedManifest(), {
      lastOracle: { role: 'reference', key: 'ref' },
      oracleOverride: { role: 'comparison', key: 'ctx' },
    });
    expect(comparedAgainst(s)?.chosen).toBe('manual');
    expect(evaluation(s).oracle_source).toBe('ctx');
  });

  it('hides the version but keeps the judged source when blind', () => {
    const s = opened(comparedManifest(), { blind: true, blindOrder: [0, 3], versionIndex: 0 });
    const c = conditions(s, 0.25);
    expect(c.version).toBeNull();
    expect(c.blind).toBe(true);
    expect(evaluation(s).judged_source).toBe('model');
  });

  it('rounds times to the millisecond and carries the region, loudness and loop', () => {
    const s = opened(comparedManifest(), {
      region: [0.12345, 2.00049],
      matchRms: true,
      loop: true,
    });
    const c = conditions(s, 0.51234);
    expect(c.playhead).toBe(0.512);
    expect(c.hit).toEqual({ n: 2, start: 0.5, into: 0.012, notes: [{ note: 64, velocity: 90 }] });
    expect(c.region).toEqual([0.123, 2]);
    expect(c.match_loudness).toBe(true);
    expect(c.loop).toBe(true);
    expect(c.duration).toBe(4);
  });

  it('claims the generation and take the page holds', () => {
    expect(evidenceClaim(opened(), 'model')).toEqual({
      set_generation: 'g'.repeat(64),
      take: 'music',
      source: 'model',
    });
    expect(evidenceClaim(opened(soloManifest()), '')).toEqual({
      set_generation: null,
      take: 'music',
      source: null,
    });
  });

  it('offers only oracles the open take holds', () => {
    const s = opened(comparedManifest(), {
      itemIndex: 1,
      take: { ...takeOf(['model', 'ref']) },
    });
    expect(oracleSources(s).map((o) => o.key)).toEqual(['ref']);
  });
});

describe('the address', () => {
  it('reads a fragment, a query, and a playhead', () => {
    expect(readRoute({ search: '?t=1.5', hash: '#p000-grand/music/ref?c=gm_gs_product' })).toEqual({
      set: 'p000-grand',
      take: 'music',
      ver: 'ref',
      cmp: 'gm_gs_product',
      t: 1.5,
    });
    expect(readRoute({ search: '?set=a&take=b&v=c', hash: '' })).toEqual({
      set: 'a',
      take: 'b',
      ver: 'c',
      cmp: '',
      t: null,
    });
    expect(readRoute({ search: '?t=nope', hash: '#a%2Fb' }).set).toBe('a/b');
  });

  it('names the render heard, and not the version when blind', () => {
    const s = opened(comparedManifest(), { versionIndex: 3 });
    expect(routeHash(s)).toBe('#p000-grand/music/ref?c=gm_gs_product');
    expect(routeHash({ ...s, blind: true, blindOrder: [3], versionIndex: 0 })).toBe(
      '#p000-grand/music?c=gm_gs_product',
    );
    expect(routeHash(opened(soloManifest(), { versionIndex: 0 }))).toBe('#p000-grand/music/model');
  });

  it('round-trips through its own reader', () => {
    const s = opened(comparedManifest(), { versionIndex: 2, comparisonId: 'gm_gs_product' });
    const r = readRoute({ search: '', hash: routeHash(s) });
    expect([r.set, r.take, r.ver, r.cmp]).toEqual([
      'p000-grand',
      'music',
      'cand-a',
      'gm_gs_product',
    ]);
  });
});

describe('local storage keys', () => {
  it('hold picks per comparison and generation', () => {
    expect(picksKey(opened())).toBe(`audition:picks:p000-grand:gm_gs_product@${'g'.repeat(16)}`);
    expect(picksKey(opened(soloManifest()))).toBe('audition:picks:p000-grand');
  });

  it('find earlier generations and the pre-comparison key for the product comparison only', () => {
    const s = opened();
    localStorage.setItem(picksKey(s), '{}');
    localStorage.setItem('audition:picks:p000-grand:gm_gs_product@old', '{}');
    localStorage.setItem('audition:picks:p000-grand', '{}');
    localStorage.setItem('audition:picks:p000-grand:instrument_di@x', '{}');
    expect(earlierPicksKeys(s).sort()).toEqual([
      'audition:picks:p000-grand',
      'audition:picks:p000-grand:gm_gs_product@old',
    ]);
    // The instrument comparison has its own earlier generation and never inherits the bare key.
    expect(earlierPicksKeys(opened(comparedManifest(), { comparisonId: 'instrument_di' }))).toEqual(
      ['audition:picks:p000-grand:instrument_di@x'],
    );
  });
});

describe('roles', () => {
  it('fall back to other for anything the manifest does not name', () => {
    const s = opened();
    expect(roleOf(s, 'ctx')).toBe('comparison');
    expect(roleOf(s, 'nope')).toBe('');
    expect(roleClass('reference')).toBe('role-reference');
    expect(roleClass('weird')).toBe('role-other');
  });
});
