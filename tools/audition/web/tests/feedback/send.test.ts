import { beforeEach, describe, expect, it, vi } from 'vitest';
import { NotePost, UndoPost } from '../../src/api/schemas';
import {
  announceSent,
  answerAt,
  chooseOracle,
  loadFeedback,
  offerText,
  onNoteSent,
  recordBlind,
  recordPreference,
  resetComposer,
  send,
  setComment,
  switchDraft,
  takeChanged,
  undo,
} from '../../src/feedback/actions';
import { fb } from '../../src/feedback/store';
import { setLang, t } from '../../src/i18n';
import { loadFeedbackIndex } from '../../src/listen/actions';
import { getState, setState } from '../../src/state/store';
import { deferred, freshState, PATH, SET, seedApp, stubServer } from './helpers';

vi.mock('../../src/listen/actions', () => ({ loadFeedbackIndex: vi.fn(), reloadSet: vi.fn() }));
vi.mock('../../src/player/engine', () => ({ playhead: () => 1.5 }));

const CONDITIONS = {
  set: SET,
  comparison_id: 'cmp1',
  take: 't1',
  take_label: 'Take one',
  version: 'a',
  against: null,
  compared_against: { role: 'reference', version: 'r', label: 'Ref one', chosen: 'last_selected' },
  blind: false,
  playhead: 1.5,
  duration: 4,
  hit: { n: 1, start: 0, into: 1.5, notes: [{ note: 60, velocity: 90 }] },
  region: null,
  match_loudness: false,
  loop: false,
};
const EVALUATION = {
  comparison_id: 'cmp1',
  scope: 'product',
  judged_source: 'a',
  oracle_source: 'r',
  comparison_status: 'matched',
  blind: false,
};

beforeEach(() => {
  freshState();
  seedApp();
  resetComposer();
  vi.mocked(loadFeedbackIndex).mockResolvedValue(undefined);
});

describe('send', () => {
  it('posts an attached note field for field', async () => {
    const server = stubServer();
    answerAt(0);
    answerAt(0);
    answerAt(1);
    answerAt(0);
    answerAt(0);
    setComment('  too hard at the start ');
    expect(await send()).toBe(true);
    const [post] = server.posts();
    expect(post?.url).toBe('feedback');
    expect(post?.body).toEqual({
      schema_version: 2,
      lang: 'en',
      grade: 'acceptable',
      tag: 'tail/long',
      answers: [
        { q: 'off', tag: 'off', said: 'Something bothers me' },
        { q: 'grade', tag: 'acceptable', said: t('tree.grade.acceptable') },
        { q: 'when', tag: 'body', said: t('tree.when.body') },
        { q: 'body', tag: 'tail', said: t('tree.body.tail') },
        { q: 'tail', tag: 'tail/long', said: t('tree.tail.tail/long') },
      ],
      text: 'too hard at the start',
      conditions: CONDITIONS,
      evaluation: EVALUATION,
      evidence: { set_generation: 'gen1', take: 't1', source: 'a' },
    });
    expect(NotePost.safeParse(post?.body).success).toBe(true);
  });

  it('sends the words in the language in force, and says which', async () => {
    const server = stubServer();
    setLang('ja');
    answerAt(1);
    await send();
    expect(server.posts()[0]?.body).toMatchObject({
      lang: 'ja',
      grade: 'ok',
      answers: [{ q: 'off', tag: 'ok', said: 'これでいい' }],
    });
  });

  it('posts an unattached note as a memo naming the voice and the oracle only', async () => {
    const server = stubServer();
    setState({ attach: false });
    answerAt(1);
    await send();
    const [post] = server.posts();
    expect(post?.body).toEqual({
      schema_version: 2,
      lang: 'en',
      grade: 'ok',
      tag: 'ok',
      answers: [{ q: 'off', tag: 'ok', said: 'This is fine as it is' }],
      text: '',
      conditions: {
        set: SET,
        compared_against: {
          role: 'reference',
          version: 'r',
          label: 'Ref one',
          chosen: 'last_selected',
        },
      },
      evaluation: { ...EVALUATION, judged_source: null },
      evidence: null,
    });
    expect(NotePost.safeParse(post?.body).success).toBe(true);
  });

  it('sends a text-only note with an empty grade and tag', async () => {
    const server = stubServer();
    setComment('just a line');
    await send();
    expect(server.posts()[0]?.body).toMatchObject({
      grade: '',
      tag: '',
      answers: [],
      text: 'just a line',
    });
  });

  it('refuses an empty note without posting, saying what is needed', async () => {
    const server = stubServer();
    expect(await send()).toBe(false);
    expect(server.calls).toEqual([]);
    expect(fb().status).toEqual({ key: 'fb.needSomething', bad: true });
  });

  it('does nothing while sending is off', async () => {
    const server = stubServer();
    setState({ canSend: false });
    answerAt(1);
    expect(await send()).toBe(false);
    expect(server.calls).toEqual([]);
  });

  it('clears the box, restarts the questions and says it was sent', async () => {
    stubServer();
    answerAt(1);
    setComment('hello');
    await send();
    expect(fb().comment).toBe('');
    expect(fb().trail).toEqual([]);
    expect(fb().node).toBe('off');
    expect(fb().status).toEqual({ key: 'fb.sent', bad: false });
    expect(fb().entries).toHaveLength(1);
    expect(fb().path).toBe(PATH);
    expect(loadFeedbackIndex).toHaveBeenCalledTimes(1);
  });

  it('sends once when clicked twice', async () => {
    const gate = deferred<undefined>();
    const server = stubServer(async (c) => (c.method === 'POST' ? gate.promise : undefined));
    answerAt(1);
    const first = send();
    const second = send();
    await Promise.resolve();
    gate.resolve(undefined);
    expect(await second).toBe(false);
    expect(await first).toBe(true);
    expect(server.posts()).toHaveLength(1);
  });

  it('announces the set and grade once, and not for a refused note', async () => {
    const heard = vi.fn();
    const off = onNoteSent(heard);
    stubServer();
    answerAt(0);
    answerAt(1);
    answerAt(0);
    await send();
    expect(heard).toHaveBeenCalledTimes(1);
    expect(heard).toHaveBeenCalledWith({ set: SET, grade: 'broken' });

    stubServer(() => ({ status: 500 }));
    answerAt(1);
    await send();
    expect(heard).toHaveBeenCalledTimes(1);
    expect(fb().status).toEqual({ key: 'fb.failed', bad: true, vars: { msg: '500' } });
    off();
    announceSent({ set: SET, grade: 'ok' });
    expect(heard).toHaveBeenCalledTimes(1);
  });
});

describe('a refused or superseded reply', () => {
  it('keeps the text on a 409, offers the reload and re-says the reason in another language', async () => {
    stubServer(() => ({
      status: 409,
      body: { error: 'stale', reason: 'generation', detail: 'd' },
    }));
    answerAt(1);
    setComment('keep me');
    expect(await send()).toBe(false);
    expect(fb().comment).toBe('keep me');
    expect(fb().trail).toHaveLength(1);
    expect(fb().stale).toBe('generation');
    const status = fb().status;
    expect(status).toEqual({ key: 'fb.stale', bad: true, vars: { reason: 'generation' } });
    if (!status) {
      throw new Error('no status');
    }
    const en = t(status.key, status.vars);
    setLang('ja');
    expect(t(status.key, status.vars)).not.toBe(en);
    expect(t(status.key, status.vars)).toContain('generation');
  });

  it('falls back to the status code when a 409 gives no reason', async () => {
    stubServer(() => ({ status: 409, body: { nonsense: true } }));
    answerAt(1);
    await send();
    expect(fb().stale).toBe('409');
  });

  it('a later write goes through after the stale state is cleared', async () => {
    let refuse = true;
    stubServer(() =>
      refuse ? { status: 409, body: { error: 'stale', reason: 'x', detail: 'd' } } : undefined,
    );
    answerAt(1);
    await send();
    refuse = false;
    expect(await send()).toBe(true);
    expect(fb().stale).toBeNull();
  });

  it('commits nothing to the panel when another set was opened meanwhile', async () => {
    const gate = deferred<undefined>();
    const server = stubServer(async (c) => (c.method === 'POST' ? gate.promise : undefined));
    answerAt(1);
    setComment('text for the old set');
    const sending = send();
    await Promise.resolve();
    // The listener opens another set while the note is in flight.
    setState({ setEpoch: 2, setId: 'set-y' });
    switchDraft('set-y');
    gate.resolve(undefined);
    expect(await sending).toBe(true);
    expect(fb().entries).toEqual([]);
    expect(fb().path).toBe('');
    expect(fb().status).toBeNull();
    // The sent text was parked as the old set's draft and is taken off it.
    expect(fb().drafts[SET]).toBe('');
    expect(fb().comment).toBe('');
    expect(fb().trail).toHaveLength(1);
    // The id is remembered under the set it was posted on.
    expect(fb().mine[SET]).toEqual(['e1']);
    expect(fb().mine['set-y']).toBeUndefined();
    expect(server.posts()).toHaveLength(1);
    expect(loadFeedbackIndex).toHaveBeenCalledTimes(1);
  });

  it('says nothing about a failure that arrives after the set changed', async () => {
    const gate = deferred<{ status: number }>();
    stubServer(async (c) => (c.method === 'POST' ? gate.promise : undefined));
    answerAt(1);
    const sending = send();
    await Promise.resolve();
    setState({ setEpoch: 2 });
    gate.resolve({ status: 500 });
    await sending;
    expect(fb().status).toBeNull();
  });
});

describe('undo', () => {
  it('names the last entry this page posted for the set', async () => {
    const server = stubServer();
    answerAt(1);
    await send();
    answerAt(1);
    await send();
    expect(fb().mine[SET]).toEqual(['e1', 'e2']);
    await undo();
    const body = server.posts().at(-1)?.body;
    expect(body).toEqual({ op: 'undo', set: SET, id: 'e2' });
    expect(UndoPost.safeParse(body).success).toBe(true);
    expect(fb().mine[SET]).toEqual(['e1']);
    expect(fb().entries.map((e) => e.id)).toEqual(['e1']);
    expect(fb().status).toEqual({ key: 'fb.undone', bad: false });
  });

  it('never reaches a note this page did not post', async () => {
    const server = stubServer();
    server.log = [{ id: 'old', at: '2026-01-01T00:00:00Z' }];
    await undo();
    expect(server.calls).toEqual([]);
  });

  it('keeps ids per set', async () => {
    const server = stubServer();
    answerAt(1);
    await send();
    setState({ setId: 'set-y', setEpoch: 2 });
    await undo();
    expect(server.posts()).toHaveLength(1);
  });

  it('on a 404 drops the id, re-reads the log and says it was already gone', async () => {
    const server = stubServer((c) =>
      c.body && (c.body as { op?: string }).op === 'undo' ? { status: 404 } : undefined,
    );
    answerAt(1);
    await send();
    server.log = [];
    await undo();
    expect(fb().mine[SET]).toEqual([]);
    expect(fb().entries).toEqual([]);
    expect(fb().status).toEqual({ key: 'fb.undoGone', bad: true });
    expect(server.calls.at(-1)?.url).toBe(`feedback.json?set=${SET}`);
  });
});

describe('blind tally', () => {
  it('posts the picks, the unseparated takes and each answer as given', async () => {
    const server = stubServer();
    const answers = [
      {
        take: 't1',
        candidates: ['a', 'r'],
        picked: 'a',
        abstained: false,
        comparison_id: 'cmp1',
        set_generation: 'gen1',
        answered_at: '2026-01-02T03:00:00Z',
      },
    ];
    expect(await recordBlind('2 of 3 chose A', { t1: 'a' }, ['t2'], answers)).toBe(true);
    const [post] = server.posts();
    expect(post?.body).toEqual({
      schema_version: 2,
      lang: 'en',
      grade: '',
      tag: 'blind',
      answers: [],
      text: '2 of 3 chose A',
      conditions: {
        set: SET,
        comparison_id: 'cmp1',
        blind: true,
        picks: { t1: 'a' },
        unseparated: ['t2'],
      },
      evaluation: { ...EVALUATION, judged_source: null },
      evidence: { set_generation: 'gen1', take: 't1', source: null },
      blind_answers: answers,
    });
    expect(NotePost.safeParse(post?.body).success).toBe(true);
  });
});

describe('preference', () => {
  it('posts the sounding version as the one to keep, with the box text', async () => {
    const server = stubServer();
    setState({ versionIndex: 1 });
    setComment(' this one ');
    await recordPreference();
    const [post] = server.posts();
    expect(post?.body).toEqual({
      schema_version: 2,
      lang: 'en',
      grade: '',
      tag: 'prefer',
      answers: [],
      text: 'this one',
      conditions: { ...CONDITIONS, version: 'r', against: 'a' },
      evaluation: { ...EVALUATION, judged_source: 'r' },
      evidence: { set_generation: 'gen1', take: 't1', source: 'r' },
    });
    expect(NotePost.safeParse(post?.body).success).toBe(true);
    expect(fb().comment).toBe('');
  });

  it('does nothing without a take', async () => {
    const server = stubServer();
    setState({ take: null });
    await recordPreference();
    expect(server.calls).toEqual([]);
  });
});

describe('drafts and offered text', () => {
  it('keeps the box per set', () => {
    switchDraft('s1');
    setComment('about one');
    switchDraft('s2');
    expect(fb().comment).toBe('');
    setComment('about two');
    switchDraft('s1');
    expect(fb().comment).toBe('about one');
    switchDraft('s1');
    expect(fb().comment).toBe('about one');
    switchDraft('s2');
    expect(fb().comment).toBe('about two');
  });

  it('offerText fills an empty or blank box only', () => {
    offerText('earlier note');
    expect(fb().comment).toBe('earlier note');
    offerText('another');
    expect(fb().comment).toBe('earlier note');
    setComment('   ');
    offerText('now');
    expect(fb().comment).toBe('now');
  });
});

describe('loading the log', () => {
  it('reads the entries and where they live, then restarts the questions', async () => {
    const server = stubServer();
    server.log = [{ id: 'old', at: '2026-01-01T00:00:00Z', text: 'before' }];
    answerAt(1);
    await loadFeedback();
    expect(fb().entries.map((e) => e.id)).toEqual(['old']);
    expect(fb().path).toBe(PATH);
    expect(fb().trail).toEqual([]);
    expect(fb().node).toBe('off');
  });

  it('leaves the composer up when the server has no endpoint', async () => {
    stubServer(() => ({ status: 404 }));
    await loadFeedback();
    expect(fb().entries).toEqual([]);
    expect(fb().node).toBe('off');
  });

  it('commits nothing for a set that was left meanwhile', async () => {
    const gate = deferred<undefined>();
    const server = stubServer(async () => gate.promise);
    server.log = [{ id: 'old' }];
    const loading = loadFeedback();
    setState({ setEpoch: 5 });
    gate.resolve(undefined);
    await loading;
    expect(fb().entries).toEqual([]);
  });

  it('clears the last reply', async () => {
    stubServer();
    await send();
    expect(fb().status).not.toBeNull();
    await loadFeedback();
    expect(fb().status).toBeNull();
  });
});

describe('the compared-against oracle', () => {
  it('a chosen override rides on the note as manual', async () => {
    const server = stubServer();
    chooseOracle('c');
    expect(getState().oracleOverride).toEqual({ role: 'comparison', key: 'c' });
    answerAt(1);
    await send();
    const body = server.posts()[0]?.body as {
      conditions: { compared_against: unknown };
      evaluation: unknown;
    };
    expect(body.conditions.compared_against).toEqual({
      role: 'comparison',
      version: 'c',
      label: 'Cmp one',
      chosen: 'manual',
    });
    expect(body.evaluation).toMatchObject({ oracle_source: 'c' });
    chooseOracle('');
    expect(getState().oracleOverride).toBeNull();
  });

  it('ignores a key the take does not offer', () => {
    chooseOracle('nope');
    expect(getState().oracleOverride).toBeNull();
  });

  it('a new take drops an oracle it does not hold', () => {
    chooseOracle('c');
    setState({ take: { id: 't2', keys: ['a', 'r'], rms: {}, rates: {}, duration: 1, epoch: 2 } });
    takeChanged();
    expect(getState().oracleOverride).toBeNull();
    expect(getState().lastOracle).toEqual({ role: 'reference', key: 'r' });
    setState({ take: { id: 't3', keys: ['a'], rms: {}, rates: {}, duration: 1, epoch: 3 } });
    takeChanged();
    expect(getState().lastOracle).toBeNull();
  });
});
