import { beforeEach, describe, expect, it, vi } from 'vitest';
import * as feedback from '../../src/feedback/actions';
import {
  abstainBlind,
  applyRoute,
  blindTally,
  chooseBlind,
  loadSet,
  pickShown,
  recordBlindResult,
  revealOrReshuffle,
  selectComparison,
  selectTake,
  setBlind,
  setRestartOnSwitch,
  setVersion,
  stepVersion,
  swapRole,
} from '../../src/listen/actions';
import { commitBuffers, playhead, setContextFactory, startAt } from '../../src/player/engine';
import { activeKey, picksKey } from '../../src/state/selectors';
import { getState, setState } from '../../src/state/store';
import { FakeContext, fakeBuffer, serveFetch, wav } from '../audio';
import { comparedManifest, opened, SETS, soloManifest } from '../fixtures';

let ctx: FakeContext;

const tracks = (m = comparedManifest()) =>
  Object.fromEntries(
    m.items
      .flatMap((it) => Object.values(it.tracks))
      .map((p) => [`s/p000-grand/${p}`, { body: wav(48000) }]),
  );

function serveSets(over: Record<string, Parameters<typeof serveFetch>[0][string]> = {}) {
  const solo = soloManifest();
  return serveFetch({
    'sets.json': { body: SETS },
    's/p000-grand/manifest.json': { body: comparedManifest() },
    's/p019-organ/manifest.json': { body: solo },
    'feedback.json': { body: { entries: [], path: '/tmp/log.jsonl' } },
    ...tracks(),
    ...Object.fromEntries(
      Object.values(solo.items[0]?.tracks ?? {}).map((p) => [`s/p019-organ/${p}`, { body: wav() }]),
    ),
    ...over,
  });
}

beforeEach(() => {
  ctx = new FakeContext();
  setContextFactory(() => ctx as unknown as AudioContext);
  setState({ sets: SETS });
  history.replaceState(null, '', '/');
});

const keysOf = () => getState().take?.keys ?? [];
const keysShown = () => getState().display.map((slot) => getState().take?.keys[slot]);

describe('opening a set', () => {
  it('reads the manifest, opens the first take and names it in the address', async () => {
    serveSets();
    await loadSet('p000-grand');
    const s = getState();
    expect(s.setId).toBe('p000-grand');
    expect(s.title).toBe('Grand');
    expect(s.take?.keys).toContain('ref');
    expect(s.canSend).toBe(true);
    expect(keysShown()).toEqual(['model', 'cand-a', 'ref', 'ctx']);
    expect(activeKey(s)).toBe('model');
    expect(location.hash).toBe('#p000-grand/music/model?c=gm_gs_product');
    expect(localStorage.getItem('audition:set')).toBe('p000-grand');
  });

  it('honours the take, version and comparison an address names', async () => {
    serveSets();
    await loadSet('p000-grand', {
      set: 'p000-grand',
      take: 'music',
      ver: 'ref-di',
      cmp: 'instrument_di',
      t: null,
    });
    expect(getState().comparisonId).toBe('instrument_di');
    expect(activeKey(getState())).toBe('ref-di');
  });

  it('turns the comparison controls off on a page with one version per take', async () => {
    serveSets();
    setState({ blind: true });
    await loadSet('p019-organ');
    expect(getState().compare).toBe(false);
    expect(getState().blind).toBe(false);
  });

  it('keeps the page it was on when a manifest fails, and says why', async () => {
    serveSets();
    await loadSet('p000-grand');
    serveSets({ 's/p019-organ/manifest.json': { status: 500 } });
    await loadSet('p019-organ');
    expect(getState().setId).toBe('p000-grand');
    expect(getState().title).toContain('500');
    expect(getState().take).not.toBeNull();
  });

  it('commits nothing from a load a later selection overtook', async () => {
    const net = serveSets({
      's/p000-grand/manifest.json': { body: comparedManifest(), hold: true },
    });
    const slow = loadSet('p000-grand');
    await Promise.resolve();
    const fast = loadSet('p019-organ');
    await fast;
    net.release();
    await slow;
    expect(getState().setId).toBe('p019-organ');
    expect(getState().title).toBe('Organ');
  });

  it('tells the composer which set it is writing about', async () => {
    serveSets();
    const draft = vi.spyOn(feedback, 'switchDraft');
    await loadSet('p019-organ');
    expect(draft).toHaveBeenCalledWith('p019-organ');
  });

  it('offers a note written before the log existed back once', async () => {
    serveSets();
    localStorage.setItem('audition:picks-note:p000-grand', JSON.stringify({ music: 'old words' }));
    const offer = vi.spyOn(feedback, 'offerText');
    await loadSet('p000-grand');
    expect(offer).toHaveBeenCalledWith('old words');
    expect(JSON.parse(localStorage.getItem('audition:picks-note:p000-grand') ?? '{}')).toEqual({});
  });
});

describe('moving between takes', () => {
  it('carries the version across by name and drops the marked passage', async () => {
    serveSets();
    await loadSet('p000-grand');
    setVersion(keysOf().indexOf('ref'));
    setState({ region: [1, 2] });
    await selectTake(1);
    expect(getState().take?.keys).toEqual(['model', 'ref']);
    expect(activeKey(getState())).toBe('ref');
    expect(getState().region).toBeNull();
    expect(location.hash).toContain('/single-c4/ref');
  });

  it('refuses a take outside the list', async () => {
    serveSets();
    await loadSet('p000-grand');
    await selectTake(9);
    await selectTake(-1);
    expect(getState().itemIndex).toBe(0);
  });

  it('says why a take could not be decoded', async () => {
    serveSets({ 's/p000-grand/c4/ref.wav': { status: 404 } });
    await loadSet('p000-grand');
    await selectTake(1);
    expect(getState().takeStatus).toEqual({ state: 'failed', msg: expect.stringContaining('404') });
    expect(getState().take).toBeNull();
    expect(getState().canSend).toBe(false);
  });

  it('keeps playing across a take change', async () => {
    serveSets();
    await loadSet('p000-grand');
    startAt(1);
    await selectTake(1);
    expect(getState().playing).toBe(true);
    expect(ctx.live()[0]?.started).toBe(0);
  });
});

describe('versions', () => {
  beforeEach(async () => {
    serveSets();
    await loadSet('p000-grand');
  });

  it('sound when picked, restarting from the top of the passage', () => {
    setState({ region: [1, 3] });
    pickShown(2);
    expect(activeKey(getState())).toBe('ref');
    expect(ctx.live()[0]?.started).toBe(1);
    expect(getState().lastOracle).toEqual({ role: 'reference', key: 'ref' });
  });

  it('keep their place when restart-on-switch is off', () => {
    setRestartOnSwitch(false);
    startAt(2);
    const before = ctx.sources.length;
    pickShown(1);
    expect(ctx.sources.length).toBe(before);
    expect(activeKey(getState())).toBe('cand-a');
  });

  it('step in the order shown and wrap', () => {
    stepVersion(-1);
    expect(activeKey(getState())).toBe('ctx');
    stepVersion(1);
    expect(activeKey(getState())).toBe('model');
  });

  it('ignore a slot that is not there', () => {
    setVersion(99);
    setVersion(1.5);
    expect(activeKey(getState())).toBe('model');
  });

  it('swap to the other side and back to the version last chosen there', () => {
    pickShown(1);
    swapRole();
    expect(activeKey(getState())).toBe('ref');
    swapRole();
    expect(activeKey(getState())).toBe('cand-a');
  });

  it('move to another comparison and come back to the version left there', () => {
    pickShown(1);
    selectComparison('instrument_di');
    expect(activeKey(getState())).toBe('model-di');
    expect(keysShown()).toEqual(['model-di', 'ref-di', 'ctx']);
    expect(getState().lastOracle).toBeNull();
    selectComparison('gm_gs_product');
    expect(activeKey(getState())).toBe('cand-a');
  });

  it('follow an address arriving at an open page', async () => {
    await applyRoute({ set: 'p000-grand', take: 'single-c4', ver: 'ref', cmp: '', t: null });
    expect(getState().itemIndex).toBe(1);
    expect(activeKey(getState())).toBe('ref');
    await applyRoute({ set: 'nowhere', take: '', ver: '', cmp: '', t: null });
    expect(getState().setId).toBe('p000-grand');
    expect(location.hash).toBe('#p000-grand/single-c4/ref?c=gm_gs_product');
  });
});

describe('blind', () => {
  beforeEach(async () => {
    serveSets();
    await loadSet('p000-grand');
  });

  it("draws the comparison's sides and keeps the sounding version through the switch", () => {
    pickShown(2);
    setBlind(true);
    const s = getState();
    expect(s.blind).toBe(true);
    expect(s.blindOrder.map((i) => s.take?.keys[i]).sort()).toEqual(['cand-a', 'model', 'ref']);
    expect(activeKey(s)).toBe('ref');
    expect(location.hash).toBe('#p000-grand/music?c=gm_gs_product');
    setBlind(false);
    expect(activeKey(getState())).toBe('ref');
  });

  it('is refused where the comparison is not matched', () => {
    selectComparison('instrument_di');
    setBlind(true);
    expect(getState().blind).toBe(false);
  });

  it('records a choice and a could-not-tell per take, and tallies them apart', () => {
    setBlind(true);
    chooseBlind();
    const pick = getState().picks.music;
    expect(pick?.key).toBe(activeKey(getState()));
    expect(pick?.candidates.sort()).toEqual(['cand-a', 'model', 'ref']);
    expect(pick?.comparison_id).toBe('gm_gs_product');
    expect(JSON.parse(localStorage.getItem(picksKey(getState())) ?? '{}').music.key).toBe(
      pick?.key,
    );
    setState({ itemIndex: 1 });
    abstainBlind();
    const tally = blindTally(getState());
    expect(tally.n).toBe(2);
    expect(tally.unsure).toEqual(['single-c4']);
    expect(Object.keys(tally.picks)).toEqual(['music']);
  });

  it('reveals a pick, then draws again', () => {
    setBlind(true);
    chooseBlind();
    revealOrReshuffle();
    expect(getState().picks.music?.revealed).toBe(true);
    const random = vi.spyOn(Math, 'random').mockReturnValue(0);
    revealOrReshuffle();
    expect(random).toHaveBeenCalled();
  });

  it('withholds the tally until the run is recorded, and posts every answer as it was given', async () => {
    setBlind(true);
    chooseBlind();
    const record = vi.spyOn(feedback, 'recordBlind').mockResolvedValue(true);
    expect(getState().blindRevealed).toBe(false);
    await recordBlindResult();
    expect(record).toHaveBeenCalledTimes(1);
    const [, picks, unsure, answers] = record.mock.calls[0] ?? [];
    expect(Object.keys(picks ?? {})).toEqual(['music']);
    expect(unsure).toEqual([]);
    expect(answers?.[0]).toMatchObject({
      take: 'music',
      abstained: false,
      comparison_id: 'gm_gs_product',
    });
    expect(getState().blindRevealed).toBe(true);
  });

  it('stays withheld when the log refused the run', async () => {
    setBlind(true);
    chooseBlind();
    vi.spyOn(feedback, 'recordBlind').mockResolvedValue(false);
    await recordBlindResult();
    expect(getState().blindRevealed).toBe(false);
  });
});

describe('the playhead across a set change', () => {
  it('goes back to the top', async () => {
    serveSets();
    await loadSet('p000-grand');
    commitBuffers({ model: fakeBuffer(4) as unknown as AudioBuffer });
    await loadSet('p019-organ');
    expect(playhead()).toBe(0);
  });
});

describe('a solo page', () => {
  it('opens with nothing to swap to', async () => {
    serveSets();
    await loadSet('p019-organ');
    swapRole();
    expect(activeKey(getState())).toBe('model');
    expect(opened(soloManifest()).compare).toBe(false);
  });
});
