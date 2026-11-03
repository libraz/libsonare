import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { onKey } from '../../src/app/keys';
import * as feedback from '../../src/feedback/actions';
import { t } from '../../src/i18n';
import { setContextFactory } from '../../src/player/engine';
import {
  advance,
  modelKey,
  PASSED_KEY,
  POLL_MS,
  pending,
  preferredTake,
  resetRun,
  runKey,
  runStore,
  startRun,
  stopRun,
} from '../../src/run/run';
import { activeKey } from '../../src/state/selectors';
import { getState, setState } from '../../src/state/store';
import { FakeContext, serveFetch, wav } from '../audio';
import { comparedManifest, opened, SETS, soloManifest, step } from '../fixtures';

let ctx: FakeContext;

const manifestWithTakes = () =>
  comparedManifest({
    items: [
      ...comparedManifest().items,
      { id: 'groove', tracks: { model: 'g/model.wav' } },
    ] as ReturnType<typeof comparedManifest>['items'],
  });

function serveRun(queue: ReturnType<typeof step>[], over: Parameters<typeof serveFetch>[0] = {}) {
  const m = manifestWithTakes();
  const files = Object.fromEntries(
    m.items
      .flatMap((it) => Object.values(it.tracks))
      .map((p) => [`s/p000-grand/${p}`, { body: wav() }]),
  );
  const solo = soloManifest();
  return serveFetch({
    'queue.json': { body: queue },
    render: { body: queue },
    'sets.json': { body: SETS },
    's/p000-grand/manifest.json': { body: m },
    's/p019-organ/manifest.json': { body: solo },
    's/p019-organ/music/model.wav': { body: wav() },
    'feedback.json': { body: { entries: [], path: '' } },
    ...files,
    ...over,
  });
}

beforeEach(() => {
  ctx = new FakeContext();
  setContextFactory(() => ctx as unknown as AudioContext);
  sessionStorage.clear();
  resetRun();
});

afterEach(() => {
  stopRun();
  vi.useRealTimers();
});

const press = (key: string) => new KeyboardEvent('keydown', { key, cancelable: true });

describe('the order', () => {
  it('skips voices passed in this tab', () => {
    sessionStorage.setItem(PASSED_KEY, JSON.stringify(['a']));
    expect(pending([step({ set: 'a' }), step({ set: 'b' })]).map((s) => s.set)).toEqual(['b']);
  });

  it('asks on music, and on the groove for a kit', () => {
    expect(preferredTake(step())).toBe('music');
    expect(preferredTake(step({ kit: true }))).toBe('groove');
  });

  it("sounds the selected comparison's model side, else the first model", () => {
    expect(modelKey(opened())).toBe('model');
    expect(modelKey(opened(comparedManifest(), { comparisonId: 'instrument_di' }))).toBe(
      'model-di',
    );
    expect(modelKey(opened(soloManifest()))).toBe('model');
  });
});

describe('a run', () => {
  it('opens the first voice on its musical take, the library side looping and attached', async () => {
    serveRun([step(), step({ set: 'p019-organ', name: 'Organ', program: 19 })]);
    await startRun();
    const s = getState();
    expect(runStore.getState().step?.set).toBe('p000-grand');
    expect(runStore.getState().total).toBe(2);
    expect(s.setId).toBe('p000-grand');
    expect(s.items[s.itemIndex]?.id).toBe('music');
    expect(activeKey(s)).toBe('model');
    expect(s.loop).toBe(true);
    expect(s.attach).toBe(true);
    expect(s.playing).toBe(true);
  });

  it('hears every voice under the same conditions, whatever the listening surface had set', async () => {
    serveRun([step()]);
    setState({ blind: true, matchRms: true, restartOnSwitch: false, region: [0.1, 0.2] });
    await startRun();
    const s = getState();
    expect(s.blind).toBe(false);
    expect(s.matchRms).toBe(false);
    expect(s.restartOnSwitch).toBe(true);
    expect(s.region).toBeNull();
    expect(activeKey(s)).toBe('model');
  });

  it('stays silent when restored without a gesture', async () => {
    serveRun([step()]);
    await startRun({ autoplay: false });
    expect(getState().playing).toBe(false);
  });

  it('asks for the page to be rendered, waits, and plays once it is ready', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    let page: 'render' | 'rendering' | 'ready' = 'render';
    const net = serveRun([], {
      'queue.json': () => ({ body: [step({ page })] }),
      render: () => {
        page = 'rendering';
        return { body: [step({ page })] };
      },
    });
    await startRun();
    expect(
      net.calls.some(
        (c) => c.url === 'render' && JSON.parse(String(c.init?.body)).sets[0] === 'p000-grand',
      ),
    ).toBe(true);
    expect(runStore.getState().wait).toBe('rendering');
    page = 'ready';
    await vi.advanceTimersByTimeAsync(POLL_MS);
    expect(runStore.getState().wait).toBeNull();
    expect(getState().setId).toBe('p000-grand');
  });

  it('shows a failed render with its reason and stops polling', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    const net = serveRun([step({ page: 'failed', error: 'exit 3', log: '/tmp/p0.log' })]);
    await startRun();
    expect(runStore.getState().wait).toBe('failed');
    expect(runStore.getState().error).toBe('exit 3');
    const before = net.calls.length;
    await vi.advanceTimersByTimeAsync(POLL_MS * 3);
    expect(net.calls.length).toBe(before);
  });

  it('renders the next voices ahead while this one is heard', async () => {
    const net = serveRun([
      step(),
      step({ set: 'b', page: 'render' }),
      step({ set: 'c', page: 'render' }),
      step({ set: 'd', page: 'render' }),
    ]);
    await startRun();
    await vi.waitFor(() => expect(net.calls.some((c) => c.url === 'render')).toBe(true));
    const asked = net.calls
      .filter((c) => c.url === 'render')
      .map((c) => JSON.parse(String(c.init?.body)).sets);
    expect(asked).toEqual([['b', 'c']]);
  });

  it('moves on when a note about this voice is sent, and remembers it passed', async () => {
    serveRun([step(), step({ set: 'p019-organ', name: 'Organ', program: 19 })]);
    await startRun();
    feedback.announceSent({ set: 'p000-grand', grade: 'ok' });
    await vi.waitFor(() => expect(getState().setId).toBe('p019-organ'));
    expect(JSON.parse(sessionStorage.getItem(PASSED_KEY) ?? '[]')).toEqual(['p000-grand']);
  });

  it('ignores a note about another voice', async () => {
    serveRun([step(), step({ set: 'p019-organ' })]);
    await startRun();
    feedback.announceSent({ set: 'p019-organ', grade: 'ok' });
    await Promise.resolve();
    expect(runStore.getState().step?.set).toBe('p000-grand');
  });

  it('says it is done when nothing is left', async () => {
    serveRun([step()]);
    await startRun();
    await advance();
    expect(runStore.getState().step).toBeNull();
    expect(runStore.getState().wait).toBe('done');
    expect(getState().playing).toBe(false);
  });

  it('says why when the queue cannot be read', async () => {
    serveRun([], { 'queue.json': { status: 500 } });
    await startRun();
    expect(runStore.getState().wait).toBe('failed');
    expect(runStore.getState().error).toBe(t('run.noQueue', { msg: '500' }));
  });

  it('stops listening for notes once stopped', async () => {
    serveRun([step(), step({ set: 'p019-organ' })]);
    await startRun();
    stopRun();
    feedback.announceSent({ set: 'p000-grand', grade: 'ok' });
    await Promise.resolve();
    expect(sessionStorage.getItem(PASSED_KEY)).toBeNull();
  });
});

describe('the keys', () => {
  it('answer with the digits, send with enter, skip with n — only while a voice is up', async () => {
    const answerAt = vi.fn(() => true);
    const send = vi.fn(async () => true);
    expect(runKey(press('1'), { answerAt, send })).toBe(false);
    serveRun([step(), step({ set: 'p019-organ' })]);
    await startRun();
    const one = press('1');
    expect(runKey(one, { answerAt, send })).toBe(true);
    expect(one.defaultPrevented).toBe(true);
    expect(runKey(press('3'), { answerAt, send })).toBe(true);
    expect(answerAt.mock.calls).toEqual([[0], [2]]);
    expect(runKey(press('4'), { answerAt, send })).toBe(false);
    expect(runKey(press('Enter'), { answerAt, send })).toBe(true);
    expect(send).toHaveBeenCalledTimes(1);
    expect(runKey(press('Tab'), { answerAt, send })).toBe(false);
    expect(runKey(press('n'), { answerAt, send })).toBe(true);
    await vi.waitFor(() => expect(runStore.getState().step?.set).toBe('p019-organ'));
  });
});

describe('the keys on the run view', () => {
  const fire = (key: string) => {
    const ev = press(key);
    onKey(ev);
    return ev;
  };

  it('take play and the side swap, and nothing the run decided', async () => {
    serveRun([step(), step({ set: 'p019-organ' })]);
    await startRun();
    setState({ view: 'run' });
    const before = getState();
    for (const key of ['v', 'b', 'm', 's', 'r', 'l', 'ArrowDown', 'ArrowRight', 'Home', '4']) {
      expect(fire(key).defaultPrevented, key).toBe(false);
    }
    const after = getState();
    expect(after.paletteOpen).toBe(false);
    expect(after.blind).toBe(before.blind);
    expect(after.matchRms).toBe(before.matchRms);
    expect(after.restartOnSwitch).toBe(before.restartOnSwitch);
    expect(after.loop).toBe(true);
    expect(after.itemIndex).toBe(before.itemIndex);
    expect(activeKey(after)).toBe('model');
    fire('Tab');
    expect(activeKey(getState())).toBe('ref');
    expect(getState().playing).toBe(true);
    fire(' ');
    expect(getState().playing).toBe(false);
  });
});
