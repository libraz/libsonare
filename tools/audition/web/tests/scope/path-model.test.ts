import { describe, expect, it } from 'vitest';
import type { Item } from '../../src/api/schemas';
import { setLang } from '../../src/i18n';
import {
  buildPathView,
  evidenceOf,
  partChain,
  pathIndex,
  topologies,
  topologyIndexAt,
} from '../../src/path/model';
import { Evidence, type Part, type PathRecord, type Unit } from '../../src/path/schema';
import { getState, setState } from '../../src/state/store';
import { loadTake } from './fakes';

const part = (over: Partial<Part> = {}): Part => ({ part: 0, stages: [], enabled: [], ...over });
const topo = (frame: number, parts: Part[] = [part()], units: Unit[] = []) => ({
  kind: 'topology' as const,
  frame,
  parts,
  units,
});

function withEvidence(evidence: unknown, over = {}) {
  loadTake({
    items: [{ id: 't', tracks: {}, evidence: { m: evidence } } as unknown as Item],
    ...over,
  });
}

describe('topologies', () => {
  it('keeps only topology events, in order', () => {
    const record = {
      events: [
        topo(0),
        { kind: 'param', frame: 5, unit: 0, slot: 1, value: 3 },
        topo(100),
        { kind: 'future', frame: 7 },
      ],
    } as unknown as PathRecord;
    expect(topologies(record).map((e) => e.frame)).toEqual([0, 100]);
    expect(topologies(null)).toEqual([]);
    expect(topologies(undefined)).toEqual([]);
  });
});

describe('topologyIndexAt', () => {
  const events = [{ frame: 0 }, { frame: 1000 }, { frame: 5000 }];

  it('picks the last event at or before the frame', () => {
    expect(topologyIndexAt(events, 0)).toBe(0);
    expect(topologyIndexAt(events, 999)).toBe(0);
    expect(topologyIndexAt(events, 1000)).toBe(1);
    expect(topologyIndexAt(events, 4999)).toBe(1);
    expect(topologyIndexAt(events, 5000)).toBe(2);
    expect(topologyIndexAt(events, 1e9)).toBe(2);
  });

  it('falls back to the first before any event', () => {
    expect(topologyIndexAt([{ frame: 50 }, { frame: 90 }], 10)).toBe(0);
    expect(topologyIndexAt([], 10)).toBe(0);
  });
});

describe('partChain', () => {
  it('starts at the voice and lists the rig stages', () => {
    const { chain, branch } = partChain(
      part({ stages: ['saturation.ampSim', 'rig.cabinet'], send_tap: 'post_unit' }),
      [],
    );
    expect(chain).toBe('voice → amp → cabinet → sends');
    expect(branch).toBeNull();
  });

  it('marks a bypassed stage', () => {
    const { chain } = partChain(
      part({
        stages: ['saturation.overdrive', 'rig.cabinet'],
        enabled: [false, true],
        send_tap: 'post_unit',
      }),
      [],
    );
    expect(chain).toContain('overdrive (bypassed)');
    expect(chain).not.toContain('cabinet (bypassed)');
  });

  it('adds the insertion unit with its stages', () => {
    const units: Unit[] = [
      {
        unit: 2,
        type: 'Stereo EQ',
        realization: 'classic',
        stages: ['fx.eq', 'fx.lim'],
        enabled: [true, false],
      },
      { unit: 3, type: 'Other', realization: 'modern', stages: [], enabled: [] },
    ];
    const { chain } = partChain(part({ unit: 2, send_tap: 'post_unit' }), units);
    expect(chain).toBe('voice → GS Stereo EQ unit (classic) [eq, lim (bypassed)] → sends');
  });

  it('ignores a unit the record does not hold', () => {
    expect(partChain(part({ unit: 9, send_tap: 'post_unit' }), []).chain).toBe('voice → sends');
  });

  it('says there are no sends when the part has no tap', () => {
    expect(partChain(part(), []).chain).toBe('voice  ·  no sends');
  });

  it('draws a pre-rig tap as a branch, not a stage', () => {
    const r = partChain(part({ stages: ['rig.cabinet'], send_tap: 'pre_rig' }), []);
    expect(r.chain).toBe('voice → cabinet');
    expect(r.branch).toBe('voice ↳ sends taken here, before the rig');
  });

  it('reports sends written at zero whatever the tap', () => {
    const r = partChain(part({ send_tap: 'pre_rig' }), [], [0, 0, 0]);
    expect(r.chain).toBe('voice  ·  sends at zero (no reverb or chorus)');
    expect(r.branch).toBeNull();
    expect(partChain(part({ send_tap: 'post_unit' }), [], [0, 1]).chain).toBe('voice → sends');
    expect(partChain(part({ send_tap: 'post_unit' }), [], []).chain).toBe('voice → sends');
  });

  it('follows the language', () => {
    setLang('ja');
    expect(partChain(part({ send_tap: 'post_unit' }), []).chain).toBe('音源 → センド');
  });
});

describe('evidence', () => {
  it('is absent, unreadable or found', () => {
    loadTake({ items: [{ id: 't', tracks: {} } as Item] });
    expect(evidenceOf(getState(), 'm').kind).toBe('none');
    withEvidence({ status: 3 });
    expect(evidenceOf(getState(), 'm').kind).toBe('unreadable');
    withEvidence({ status: 'recorded' });
    expect(evidenceOf(getState(), 'm').kind).toBe('found');
    // What a real render writes: unknowns as null, a reference with no completeness at all.
    withEvidence({
      status: 'recorded',
      complete: null,
      reason: null,
      source_id: null,
      channel: 0,
      sends: [0, 0, 0],
      path: { schema: '1', complete: true, reason: null, events: [] },
    });
    expect(evidenceOf(getState(), 'm').kind).toBe('found');
    expect(evidenceOf(getState(), 'r').kind).toBe('none');
  });
});

describe('buildPathView', () => {
  const recorded = (extra = {}) => ({
    status: 'recorded',
    complete: true,
    path: {
      events: [
        topo(0, [part({ stages: ['rig.cabinet'], send_tap: 'post_unit' })]),
        { kind: 'param', frame: 500, unit: 1, slot: 4, value: 77 },
        topo(1000, [part({ stages: ['rig.cabinet'], enabled: [false], send_tap: 'post_unit' })]),
      ],
    },
    ...extra,
  });
  const texts = (key = 'm', at = 0) => buildPathView(getState(), key, at).spans.map((s) => s.text);

  it('is empty without a take', () => {
    expect(buildPathView(getState(), 'm', 0).spans).toEqual([]);
  });

  it('shows the topology in force at the playhead', () => {
    withEvidence(recorded());
    expect(texts('m', 0)[1]).toBe('voice → cabinet → sends');
    expect(texts('m', 0.999)[1]).toBe('voice → cabinet → sends');
    expect(texts('m', 1)[1]).toBe('voice → cabinet (bypassed) → sends');
    expect(pathIndex(getState(), 'm', 0.5)).toBe(0);
    expect(pathIndex(getState(), 'm', 1.5)).toBe(1);
  });

  it('notes that the path changes, with times and the index shown', () => {
    withEvidence(recorded());
    const view = buildPathView(getState(), 'm', 1.2);
    const change = view.spans.find((s) => s.cls === 'path-change');
    expect(change?.text).toBe('the path changes during this take, at 1.00 s (showing 2 of 2)');
  });

  it('carries the raw record and the parameter bytes inside the interval as the tooltip', () => {
    withEvidence(recorded());
    const first = buildPathView(getState(), 'm', 0).title.split('\n');
    expect(first).toHaveLength(2);
    expect(first[1]).toBe('@0.50s unit 1 slot 4 = 77');
    expect(buildPathView(getState(), 'm', 1.5).title.split('\n')).toHaveLength(1);
  });

  it('uses the sample rate of the version', () => {
    withEvidence(recorded());
    const take = getState().take;
    if (take) {
      setState({ take: { ...take, rates: { m: 500 } } });
    }
    expect(buildPathView(getState(), 'm', 1.9).spans.some((s) => s.cls === 'path-change')).toBe(
      true,
    );
    expect(texts('m', 1.9)[1]).toBe('voice → cabinet → sends');
    expect(texts('m', 2)[1]).toBe('voice → cabinet (bypassed) → sends');
  });

  it('labels parts when there are several and filters by channel', () => {
    const parts = [
      part({ part: 0, send_tap: 'post_unit' }),
      part({ part: 1, send_tap: 'post_unit' }),
    ];
    withEvidence({ status: 'recorded', path: { events: [topo(0, parts)] } });
    expect(texts()).toEqual(['path', 'part 1: voice → sends', 'part 2: voice → sends']);
    withEvidence({ status: 'recorded', channel: 1, path: { events: [topo(0, parts)] } });
    expect(texts()).toEqual(['path', 'voice → sends']);
  });

  it('warns when the path is incomplete', () => {
    withEvidence(recorded({ complete: false, reason: 'dropped' }));
    const warn = buildPathView(getState(), 'm', 0).spans.find((s) => s.cls === 'path-warn');
    expect(warn?.text).toBe('path incomplete: dropped');
  });

  it('says why a path was not recorded', () => {
    withEvidence({ status: 'unavailable', reason: 'engine too old' });
    expect(texts()[1]).toBe('path not recorded: engine too old');
    withEvidence({ status: 'recorded', path: { events: [] } });
    expect(texts()[1]).toBe('path not recorded: no topology event');
    withEvidence({ status: 'skipped' });
    expect(texts()[1]).toBe('path not recorded: skipped');
  });

  it('says a page without evidence predates it', () => {
    loadTake({ items: [{ id: 't', tracks: {} } as Item] });
    expect(texts()[1]).toBe(
      'path not recorded: this page was rendered before renders carried evidence',
    );
  });

  it('says an unreadable record is unreadable', () => {
    withEvidence({ status: 'recorded', path: { events: [{ kind: 'topology', frame: 'x' }] } });
    expect(texts()[1]).toBe('path not recorded: the path record could not be read');
  });

  it('shows no library path for a recording', () => {
    withEvidence(recorded());
    expect(texts('r')).toEqual(['path', 'a recording; no library path']);
  });

  it('hides every name when blind but keeps the comparison and its standing', () => {
    withEvidence(recorded(), {
      blind: true,
      manifest: {
        sources: {},
        comparisons: [
          {
            id: 'instrument_di',
            label: 'x',
            scope: 'instrument',
            status: 'matched',
            model_sources: [],
            oracle_sources: [],
            reasons: [],
          },
        ],
      },
    });
    expect(texts()).toEqual(['path', 'DI · comparable  ·  path hidden while blind']);
    expect(buildPathView(getState(), 'm', 0).title).toBe('');
  });

  it('follows the language', () => {
    withEvidence(recorded());
    setLang('ja');
    expect(texts()[0]).toBe('経路');
  });
});

describe('schema', () => {
  const ok = { status: 'recorded', path: { events: [topo(0)] } };

  it('accepts a record and fills defaults', () => {
    const e = Evidence.parse({
      status: 'recorded',
      path: { events: [{ kind: 'topology', frame: 0 }] },
    });
    const ev = e.path?.events[0];
    expect(ev && 'parts' in ev ? ev.parts : null).toEqual([]);
    expect(Evidence.safeParse(ok).success).toBe(true);
    expect(Evidence.safeParse({}).success).toBe(true);
  });

  it('keeps fields it does not know', () => {
    expect((Evidence.parse({ future: 1 }) as Record<string, unknown>).future).toBe(1);
  });

  it.each([
    ['a status that is not a string', { status: 4 }],
    ['events that are not a list', { path: { events: 'x' } }],
    ['an event with no frame', { path: { events: [{ kind: 'topology' }] } }],
    ['an event whose frame is text', { path: { events: [{ kind: 'topology', frame: '0' }] } }],
    [
      'a param event with a text slot',
      { path: { events: [{ kind: 'param', frame: 0, unit: 0, slot: 'a', value: 1 }] } },
    ],
    ['sends that are not numbers', { sends: ['a'] }],
    [
      'a part with no number',
      { path: { events: [{ kind: 'topology', frame: 0, parts: [{ stages: [] }] }] } },
    ],
    [
      'a unit with no realization',
      { path: { events: [{ kind: 'topology', frame: 0, units: [{ unit: 1, type: 'x' }] }] } },
    ],
    ['a non-object', 'recorded'],
  ])('rejects %s', (_name, raw) => {
    expect(Evidence.safeParse(raw).success).toBe(false);
  });
});
