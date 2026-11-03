import { describe, expect, it } from 'vitest';
import {
  bankStats,
  cursorIndex,
  emptyQuery,
  matches,
  pageContext,
  shownVoices,
  stageCounts,
  stageIndex,
} from '../../src/bank/logic';
import { bank, setEntry } from './fixtures';

const voices = bank();
const ctx = pageContext([
  setEntry('p000', { ships: { state: 'current' } }),
  setEntry('p002', { ships: { state: 'stale' } }),
  setEntry('k000'),
]);
const slugs = (q: Partial<ReturnType<typeof emptyQuery>>, sort = 'en') =>
  shownVoices(voices, { ...emptyQuery(), ...q }, ctx, sort).map((v) => v.slug);

describe('stageIndex', () => {
  it('rounds the stage in fifths and clamps it', () => {
    expect(stageIndex({ stage: 0 })).toBe(0);
    expect(stageIndex({ stage: 0.2 })).toBe(1);
    expect(stageIndex({ stage: 0.5 })).toBe(3);
    expect(stageIndex({ stage: 1 })).toBe(5);
    expect(stageIndex({ stage: 1.4 })).toBe(5);
    expect(stageIndex({ stage: -1 })).toBe(0);
  });
});

describe('pageContext', () => {
  it('knows which pages exist and their ship state', () => {
    expect(ctx.hasPage('p000')).toBe(true);
    expect(ctx.hasPage('p001')).toBe(false);
    expect(ctx.shipState('p002')).toBe('stale');
    expect(ctx.shipState('k000')).toBe('');
    expect(ctx.shipState('nowhere')).toBe('');
  });
});

describe('filtering', () => {
  it('keeps everything for an empty query, in generated order', () => {
    expect(slugs({})).toEqual(['p000', 'p001', 'p002', 'p002b8', 'p002b1', 'p003', 'k000']);
  });

  it('filters by method', () => {
    expect(slugs({ method: 'fm' })).toEqual(['p002', 'p002b8', 'p002b1']);
    expect(slugs({ method: 'classic' })).toEqual(['p001', 'p003']);
    expect(slugs({ method: 'physical' })).toEqual(['p000', 'k000']);
  });

  it('filters by slot kind', () => {
    expect(slugs({ slot: 'kit' })).toEqual(['k000']);
    expect(slugs({ slot: 'gs' })).toEqual(['p002b8']);
    expect(slugs({ slot: 'gm2' })).toEqual(['p002b1']);
    expect(slugs({ slot: 'gm' })).toEqual(['p000', 'p001', 'p002', 'p003']);
  });

  it('filters by rung', () => {
    expect(slugs({ stage: 5 })).toEqual(['p002']);
    expect(slugs({ stage: 0 })).toEqual(['k000']);
    expect(slugs({ stage: 2 })).toEqual(['p002b8', 'p002b1']);
  });

  it('filters by reference, unwritten settings and page', () => {
    expect(slugs({ filter: 'oracle' })).toEqual(['p000', 'p002', 'p003']);
    expect(slugs({ filter: 'no-oracle' })).toEqual(['p001', 'p002b8', 'p002b1', 'k000']);
    expect(slugs({ filter: 'unwritten' })).toEqual(['p002']);
    expect(slugs({ filter: 'page' })).toEqual(['p000', 'p002', 'k000']);
  });

  it('filters by whether the page plays what ships', () => {
    expect(slugs({ filter: 'current' })).toEqual(['p000']);
    expect(slugs({ filter: 'stale' })).toEqual(['p002']);
  });

  it('filters by what the slot is aimed at', () => {
    expect(slugs({ filter: 't-instrument' })).toEqual(['p001']);
    expect(slugs({ filter: 't-machine' })).toEqual(['p002']);
  });

  it('finds over name, engine, patch, slug and tone class, ignoring case and padding', () => {
    expect(slugs({ find: '  GLASS ' })).toEqual(['p000']);
    expect(slugs({ find: 'subtractive' })).toEqual(['p001']);
    expect(slugs({ find: 'p002b' })).toEqual(['p002b8', 'p002b1']);
    expect(slugs({ find: 'struck' })).toEqual(['p000', 'p001', 'p002', 'p002b8', 'p002b1', 'k000']);
    expect(slugs({ find: 'zzz' })).toEqual([]);
  });

  it('combines conditions', () => {
    expect(slugs({ method: 'fm', filter: 'no-oracle', find: 'rubber' })).toEqual([
      'p002b8',
      'p002b1',
    ]);
    expect(slugs({ method: 'fm', slot: 'gs' })).toEqual(['p002b8']);
  });

  it('matches a single row the same way', () => {
    const v = voices[0];
    expect(v && matches(v, { ...emptyQuery(), find: 'harmonica' }, ctx)).toBe(true);
  });
});

describe('sorting', () => {
  it('puts the highest stage first, ties by program', () => {
    expect(slugs({ sort: 'stage' })).toEqual([
      'p002',
      'p003',
      'p000',
      'p002b8',
      'p002b1',
      'p001',
      'k000',
    ]);
  });

  it('sorts by name without mutating the source order', () => {
    const before = voices.map((v) => v.slug);
    expect(slugs({ sort: 'name' })[0]).toBe('p003');
    expect(slugs({ sort: 'name' }).at(-1)).toBe('p001');
    expect(voices.map((v) => v.slug)).toEqual(before);
  });
});

describe('stage buckets and masthead totals', () => {
  it('counts voices per rung', () => {
    expect(stageCounts(voices)).toEqual([1, 1, 2, 1, 1, 1]);
  });

  it('totals the masthead figures', () => {
    expect(bankStats(voices, ctx)).toEqual({
      voices: 7,
      withOracle: 3,
      unwritten: 2,
      pages: 3,
      current: 1,
      stale: 1,
      aimInstrument: 1,
      aimMachine: 1,
      perStage: [1, 1, 2, 1, 1, 1],
    });
  });

  it('is all zeros for an empty bank', () => {
    expect(bankStats([], ctx).perStage).toEqual([0, 0, 0, 0, 0, 0]);
  });
});

describe('cursorIndex', () => {
  const shown = voices;

  it('follows the voice the cursor holds', () => {
    expect(cursorIndex(shown, 'p003')).toBe(5);
  });

  it('falls to the first row when the held voice is gone or nothing is held', () => {
    expect(cursorIndex(shown, 'gone')).toBe(0);
    expect(cursorIndex(shown, null)).toBe(0);
  });

  it('is -1 with nothing shown', () => {
    expect(cursorIndex([], 'p000')).toBe(-1);
  });
});
