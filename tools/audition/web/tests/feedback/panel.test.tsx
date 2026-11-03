import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { answerAt, loadFeedback, resetComposer } from '../../src/feedback/actions';
import { FeedbackPanel } from '../../src/feedback/FeedbackPanel';
import { fb } from '../../src/feedback/store';
import { setLang, t } from '../../src/i18n';
import { reloadSet } from '../../src/listen/actions';
import { getState, setState } from '../../src/state/store';
import { type Call, freshState, PATH, seedApp, stubServer } from './helpers';

vi.mock('../../src/listen/actions', () => ({ loadFeedbackIndex: vi.fn(), reloadSet: vi.fn() }));
vi.mock('../../src/player/engine', () => ({ playhead: () => 1.5 }));

beforeEach(() => {
  freshState();
  seedApp();
  resetComposer();
});

function sentConditions(call: Call | undefined): object {
  return (call?.body as { conditions?: object } | undefined)?.conditions ?? {};
}
const box = () => screen.getByRole('textbox') as HTMLTextAreaElement;
const sendButton = () => document.getElementById('fbSend') as HTMLButtonElement;

describe('the narrowing', () => {
  it('shows the question and numbered choices with "not sure" last', () => {
    render(<FeedbackPanel />);
    expect(document.getElementById('fbQuestion')).toHaveTextContent(t('tree.off.q'));
    const choices = [...document.querySelectorAll('#fbChoices .fb-choice')];
    expect(choices.map((c) => c.querySelector('kbd.fb-key')?.textContent)).toEqual(['1', '2', '3']);
    expect(choices.map((c) => c.querySelector('span')?.textContent)).toEqual([
      t('tree.off.off'),
      t('tree.off.ok'),
      t('fb.notSure'),
    ]);
    expect(choices[2]).toHaveClass('fb-unsure');
  });

  it('words the first question for a page with no comparison', () => {
    setState({ compare: false });
    render(<FeedbackPanel />);
    expect(document.getElementById('fbQuestion')).toHaveTextContent(t('tree.off.qSolo'));
  });

  it('walks to a leaf by clicking, with back and start over', async () => {
    const user = userEvent.setup();
    render(<FeedbackPanel />);
    const back = document.getElementById('fbBack') as HTMLElement;
    const restart = document.getElementById('fbRestart') as HTMLElement;
    expect(back).toHaveAttribute('hidden');
    expect(restart).toHaveAttribute('hidden');
    await user.click(screen.getByRole('button', { name: new RegExp(t('tree.off.off')) }));
    expect(back).not.toHaveAttribute('hidden');
    expect(document.getElementById('fbTrail')).toHaveTextContent(t('tree.off.off'));
    expect(document.getElementById('fbQuestion')).toHaveTextContent(t('tree.grade.q'));
    await user.click(back);
    expect(document.getElementById('fbQuestion')).toHaveTextContent(t('tree.off.q'));
    await user.click(screen.getByRole('button', { name: new RegExp(t('tree.off.ok')) }));
    expect(document.getElementById('fbQuestion')).toHaveAttribute('hidden');
    expect(document.querySelectorAll('#fbChoices .fb-choice')).toHaveLength(0);
    expect(sendButton()).toHaveClass('ready');
    await user.click(restart);
    expect(sendButton()).not.toHaveClass('ready');
    expect(document.getElementById('fbTrail')).toBeEmptyDOMElement();
  });

  it('separates trail steps and re-words them when the language changes', async () => {
    render(<FeedbackPanel />);
    act(() => {
      answerAt(0);
      answerAt(0);
    });
    expect(document.querySelectorAll('#fbTrail .fb-step')).toHaveLength(2);
    expect(document.querySelectorAll('#fbTrail .fb-sep')).toHaveLength(1);
    act(() => setLang('ja'));
    expect(document.getElementById('fbTrail')).toHaveTextContent(t('tree.off.off'));
    expect(document.getElementById('fbTrail')).toHaveTextContent('気になるところがある');
  });
});

describe('sending', () => {
  it('sends with the button and shows the reply', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    render(<FeedbackPanel />);
    await user.click(screen.getByRole('button', { name: new RegExp(t('tree.off.ok')) }));
    await user.type(box(), 'fine');
    await user.click(sendButton());
    expect(server.posts()).toHaveLength(1);
    expect(document.getElementById('fbStatus')).toHaveTextContent(t('fb.sent'));
    expect(box().value).toBe('');
    expect(document.getElementById('fbWhere')).toHaveTextContent(t('fb.where', { path: PATH }));
  });

  it('sends on Cmd+Enter and on Ctrl+Enter', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    render(<FeedbackPanel />);
    await user.type(box(), 'one');
    await user.keyboard('{Meta>}{Enter}{/Meta}');
    expect(server.posts()).toHaveLength(1);
    await user.type(box(), 'two');
    await user.keyboard('{Control>}{Enter}{/Control}');
    expect(server.posts()).toHaveLength(2);
    await user.type(box(), 'three');
    await user.keyboard('{Enter}');
    expect(server.posts()).toHaveLength(2);
    expect(box().value).toBe('three\n');
  });

  it('Cmd+Enter with nothing to say asks for something', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    render(<FeedbackPanel />);
    await user.click(box());
    await user.keyboard('{Meta>}{Enter}{/Meta}');
    expect(server.calls).toEqual([]);
    expect(document.getElementById('fbStatus')).toHaveTextContent(t('fb.needSomething'));
    expect(document.getElementById('fbStatus')).toHaveClass('bad');
  });

  it('is disabled, and Cmd+Enter blocked, while sending is off', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    setState({ canSend: false });
    render(<FeedbackPanel />);
    expect(sendButton()).toBeDisabled();
    await user.type(box(), 'text');
    await user.keyboard('{Meta>}{Enter}{/Meta}');
    expect(server.calls).toEqual([]);
    expect(box().value).toBe('text');
    act(() => setState({ canSend: true }));
    expect(sendButton()).toBeEnabled();
  });

  it('labels the button send, or the run label in the run view', () => {
    render(<FeedbackPanel />);
    expect(sendButton()).toHaveTextContent(t('fb.send'));
    expect(sendButton()).toHaveClass('send');
    act(() => setState({ view: 'run' }));
    expect(sendButton()).toHaveTextContent(t('run.keySend'));
  });

  it('the attach box is the app store flag, and un-attaching sends a memo', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    render(<FeedbackPanel />);
    const attach = screen.getByRole('checkbox');
    expect(attach).toBeChecked();
    await user.click(attach);
    expect(getState().attach).toBe(false);
    await user.type(box(), 'memo');
    await user.click(sendButton());
    expect(Object.keys(sentConditions(server.posts()[0])).sort()).toEqual([
      'compared_against',
      'set',
    ]);
  });

  it('a 409 keeps the text, offers the reload and the reason follows the language', async () => {
    const user = userEvent.setup();
    stubServer(() => ({
      status: 409,
      body: { error: 'stale', reason: 'generation', detail: 'd' },
    }));
    render(<FeedbackPanel />);
    const reload = document.getElementById('fbReload') as HTMLElement;
    expect(reload).toHaveAttribute('hidden');
    await user.type(box(), 'keep this');
    await user.click(sendButton());
    expect(box().value).toBe('keep this');
    expect(reload).not.toHaveAttribute('hidden');
    const status = document.getElementById('fbStatus') as HTMLElement;
    expect(status).toHaveTextContent(t('fb.stale', { reason: 'generation' }));
    const english = status.textContent;
    act(() => setLang('ja'));
    expect(status.textContent).toBe(t('fb.stale', { reason: 'generation' }));
    expect(status.textContent).not.toBe(english);
    await user.click(reload);
    expect(reloadSet).toHaveBeenCalledTimes(1);
    expect(reload).toHaveAttribute('hidden');
    expect(status).toBeEmptyDOMElement();
    expect(fb().comment).toBe('keep this');
  });
});

describe('the compared-against row', () => {
  it('flags an absent oracle and offers the take own oracles in the select', () => {
    setState({ lastOracle: null });
    render(<FeedbackPanel />);
    expect(document.getElementById('cmpChip')).toHaveClass('warn');
    expect(document.getElementById('cmpChip')).toHaveTextContent(t('cmp.unplayed'));
    const options = [...(document.getElementById('cmpOverride') as HTMLSelectElement).options];
    expect(options.map((o) => [o.value, o.textContent])).toEqual([
      ['', t('cmp.overrideAuto')],
      ['r', 'reference: Ref one'],
      ['c', 'comparison: Cmp one'],
    ]);
  });

  it('shows the last oracle, and the override changes the store and the next note', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    render(<FeedbackPanel />);
    const chip = document.getElementById('cmpChip') as HTMLElement;
    expect(chip).toHaveClass('role-reference');
    expect(chip).not.toHaveClass('warn');
    expect(chip).toHaveTextContent('Ref one');
    const select = document.getElementById('cmpOverride') as HTMLSelectElement;
    await user.selectOptions(select, 'c');
    expect(getState().oracleOverride).toEqual({ role: 'comparison', key: 'c' });
    expect(chip).toHaveClass('role-comparison');
    expect(chip).toHaveAttribute('title', t('cmp.manualTitle'));
    await user.type(box(), 'x');
    await user.click(sendButton());
    const body = server.posts()[0]?.body as {
      conditions: { compared_against: { chosen: string; version: string } };
    };
    expect(body.conditions.compared_against).toMatchObject({ chosen: 'manual', version: 'c' });
    await user.selectOptions(select, '');
    expect(getState().oracleOverride).toBeNull();
    expect(chip).toHaveAttribute('title', t('cmp.autoTitle'));
  });
});

describe('the log', () => {
  const entries = [
    {
      id: 'old',
      at: '2026-01-02T03:04:05Z',
      grade: 'ok',
      tag: 'ok',
      text: 'older words',
      answers: [{ q: 'off', tag: 'ok', said: 'fine as it is' }],
      conditions: {
        version: 'a',
        take: 't1',
        compared_against: { role: 'reference', version: 'r', label: 'Ref one', chosen: 'manual' },
      },
    },
    {
      id: 'new',
      at: '2026-02-03T04:05:06Z',
      grade: 'broken',
      tag: 'broken',
      text: 'newer words',
      answers: [
        { q: 'off', tag: 'off', said: 'bothers' },
        { q: 'grade', tag: 'wrong-instrument', said: 'sounds like something else' },
      ],
      conditions: { blind: true, compared_against: null },
    },
    { id: 'legacy', at: 'not a date', grade: 'off/unsure', tag: 'off/unsure', conditions: {} },
  ];

  async function shown() {
    const server = stubServer();
    server.log = entries;
    render(<FeedbackPanel />);
    await act(() => loadFeedback());
  }

  it('lists the newest first', async () => {
    await shown();
    const rows = [...document.querySelectorAll('#fbRecent .fb-entry')];
    expect(rows).toHaveLength(3);
    expect(rows.map((r) => r.querySelector('.fb-text')?.textContent ?? '')).toEqual([
      '',
      'newer words',
      'older words',
    ]);
  });

  it('draws grade, version, oracle, take, said trail, text, tag and time', async () => {
    await shown();
    const row = [...document.querySelectorAll('#fbRecent .fb-entry')][2] as HTMLElement;
    expect(row.querySelector('.fb-grade')).toHaveClass('good');
    expect(row.querySelector('.fb-grade')).toHaveTextContent(t('grade.ok'));
    const who = row.querySelector('.fb-who') as HTMLElement;
    expect(who).toHaveClass('role-model');
    expect(who).toHaveAttribute('title', 'Alpha render');
    expect(row.querySelector('.fb-cmp')).toHaveClass('role-reference');
    expect(row.querySelector('.fb-cmp')).toHaveTextContent('Ref one');
    expect(row.querySelector('.fb-cmp')).toHaveAttribute('title', t('role.reference'));
    expect(row.querySelector('.fb-take')).toHaveTextContent('t1');
    expect(row.querySelector('.fb-said')).toHaveTextContent('fine as it is');
    expect(row.querySelector('code.fb-tag')).toHaveTextContent('ok');
    const when = new Date('2026-01-02T03:04:05Z').toLocaleString('en', {
      month: 'short',
      day: 'numeric',
      hour: '2-digit',
      minute: '2-digit',
    });
    expect(row.querySelector('.fb-when')).toHaveTextContent(when);
  });

  it('shows a hidden-version chip for a blind note, an unplayed oracle and a joined trail', async () => {
    await shown();
    const row = [...document.querySelectorAll('#fbRecent .fb-entry')][1] as HTMLElement;
    expect(row.querySelector('.fb-grade')).toHaveClass('bad');
    expect(row.querySelector('.fb-who')).toHaveTextContent(t('now.hidden'));
    expect(row.querySelector('.fb-cmp')).toHaveTextContent(t('cmp.unplayed'));
    expect(row.querySelector('.fb-said')?.textContent).toBe(
      'bothers  ›  sounds like something else',
    );
  });

  it('marks a note that predates the oracle field as unknown and keeps an unparseable time', async () => {
    await shown();
    const row = [...document.querySelectorAll('#fbRecent .fb-entry')][0] as HTMLElement;
    expect(row.querySelector('.fb-cmp')).toHaveTextContent(t('cmp.unknown'));
    expect(row.querySelector('.fb-cmp')).toHaveAttribute('title', t('cmp.unknownTitle'));
    expect(row.querySelector('.fb-when')).toHaveTextContent('not a date');
    expect(row.querySelector('.fb-grade')).toHaveTextContent(t('grade.unsure'));
  });

  it('says nothing yet for an empty log and shows where the log lives', async () => {
    stubServer();
    render(<FeedbackPanel />);
    expect(screen.getByText(t('fb.none'))).toBeInTheDocument();
    await act(() => loadFeedback());
    expect(document.getElementById('fbWhere')).toHaveTextContent(PATH);
  });

  it('undo is enabled only for this page own note and takes it back', async () => {
    const user = userEvent.setup();
    const server = stubServer();
    server.log = entries;
    render(<FeedbackPanel />);
    await act(() => loadFeedback());
    const undo = document.getElementById('fbUndo') as HTMLButtonElement;
    expect(undo).toBeDisabled();
    await user.click(screen.getByRole('button', { name: new RegExp(t('tree.off.ok')) }));
    await user.click(sendButton());
    expect(undo).toBeEnabled();
    expect(document.querySelectorAll('#fbRecent .fb-entry')).toHaveLength(4);
    await user.click(undo);
    expect(server.posts().at(-1)?.body).toEqual({ op: 'undo', set: 'set-x', id: 'e1' });
    expect(document.querySelectorAll('#fbRecent .fb-entry')).toHaveLength(3);
    expect(undo).toBeDisabled();
    expect(document.getElementById('fbStatus')).toHaveTextContent(t('fb.undone'));
  });

  it.each(['en', 'ja'] as const)('shows no raw keys in %s', async (lang) => {
    setLang(lang);
    await shown();
    act(() => {
      answerAt(0);
      answerAt(0);
    });
    const panel = document.querySelector('.fb') as HTMLElement;
    expect(within(panel).queryByText(/\b(fb|cmp|tree|grade|role|run|now)\.[a-zA-Z]/)).toBeNull();
    expect(panel.textContent).not.toMatch(/\b(fb|cmp|tree|grade|role|now)\.[a-zA-Z]/);
    expect(document.getElementById('fbQuestion')?.textContent).toBeTruthy();
    expect(panel).toHaveTextContent(t('fb.title'));
    expect(panel).toHaveTextContent(t('fb.intro'));
  });
});
