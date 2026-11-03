import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BankFile, BankVoiceRow, Provenance } from '../../src/bank/schema';
import {
  currentRow,
  currentShown,
  getBank,
  loadBank,
  resetBank,
  setCursor,
  setQuery,
} from '../../src/bank/store';
import { setState } from '../../src/state/store';
import { bank, setEntry } from './fixtures';

const reply = (body: unknown, ok = true) =>
  vi.fn(async () => new Response(JSON.stringify(body), { status: ok ? 200 : 500 }));

beforeEach(() => resetBank());
afterEach(() => vi.unstubAllGlobals());

describe('loadBank', () => {
  it('fetches bank.json once and parses every row', async () => {
    const fetchMock = reply({ voices: [{ slug: 'a', name: 'Alpha', stage: 0.4 }, { slug: 'b' }] });
    vi.stubGlobal('fetch', fetchMock);
    await loadBank();
    await loadBank();
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock).toHaveBeenCalledWith('bank.json');
    const { voices, loaded } = getBank();
    expect(loaded).toBe(true);
    expect(voices.map((v) => v.slug)).toEqual(['a', 'b']);
    // Absent fields normalise to one empty value.
    expect(voices[1]).toMatchObject({
      name: '',
      stage: 0,
      open_candidates: [],
      kit: false,
      bank: 0,
    });
  });

  it('leaves the bank empty, and loaded, when the body is malformed', async () => {
    vi.stubGlobal('fetch', reply({ voices: [{ slug: 'a', stage: 'high' }] }));
    await loadBank();
    expect(getBank()).toMatchObject({ voices: [], loaded: true });
  });

  it('leaves the bank empty when the server answers an error or the network fails', async () => {
    vi.stubGlobal('fetch', reply({ error: 'no' }, false));
    await loadBank();
    expect(getBank().voices).toEqual([]);
    resetBank();
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => Promise.reject(new Error('offline'))),
    );
    await loadBank();
    expect(getBank()).toMatchObject({ voices: [], loaded: true });
  });
});

describe('the schemas', () => {
  it('reject a row with no slug and a bank whose voices are not a list', () => {
    expect(BankVoiceRow.safeParse({ name: 'x' }).success).toBe(false);
    expect(BankFile.safeParse({ voices: 'none' }).success).toBe(false);
    expect(BankFile.safeParse({}).success).toBe(false);
  });

  it('reject a wrong-typed readout inside a row', () => {
    expect(BankVoiceRow.safeParse({ slug: 'a', open_candidates: 'x' }).success).toBe(false);
    expect(
      BankVoiceRow.safeParse({ slug: 'a', axes: { coverage: { gated: 'many' } } }).success,
    ).toBe(false);
  });

  it('keep fields the page does not read', () => {
    expect(BankVoiceRow.parse({ slug: 'a', extra: 1 })).toHaveProperty('extra', 1);
  });

  it('read a provenance with nothing in it', () => {
    expect(Provenance.safeParse({}).success).toBe(true);
    expect(Provenance.safeParse({ answers: 'x' }).success).toBe(false);
  });
});

describe('the query and the cursor', () => {
  beforeEach(() => {
    resetBank({ voices: bank(), loaded: true });
    setState({ sets: [setEntry('p000')] });
  });

  it('merges a partial query', () => {
    setQuery({ method: 'fm' });
    setQuery({ find: 'band' });
    expect(getBank().query).toMatchObject({ method: 'fm', find: 'band', slot: '' });
    expect(currentShown().map((v) => v.slug)).toEqual(['p002', 'p002b8', 'p002b1']);
  });

  it('holds the cursor on a voice, and falls to the first row when it is filtered out', () => {
    setCursor('p003');
    expect(currentRow()?.slug).toBe('p003');
    setQuery({ method: 'fm' });
    expect(currentRow()?.slug).toBe('p002');
    setQuery({ method: '' });
    expect(currentRow()?.slug).toBe('p003');
  });

  it('has no row when nothing matches', () => {
    setQuery({ find: 'zzz' });
    expect(currentRow()).toBeNull();
  });
});
