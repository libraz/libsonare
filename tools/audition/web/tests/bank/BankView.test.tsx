import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { BankView, bankKey } from '../../src/bank/BankView';
import { getBank, resetBank } from '../../src/bank/store';
import { setLang, tableOf } from '../../src/i18n';
import { setState } from '../../src/state/store';
import { bank, setEntry } from './fixtures';

const en = tableOf('en');

function mount() {
  const onOpen = vi.fn();
  const user = userEvent.setup();
  const utils = render(<BankView onOpen={onOpen} />);
  return { onOpen, user, ...utils };
}

const rows = () =>
  within(screen.getByRole('listbox'))
    .queryAllByRole('option')
    .map((r) => r.id);
const selected = () =>
  within(screen.getByRole('listbox'))
    .getAllByRole('option')
    .find((r) => r.getAttribute('aria-selected') === 'true')?.id;
const inspector = () => document.getElementById('bankInspect') as HTMLElement;

beforeEach(() => {
  resetBank({ voices: bank(), loaded: true });
  setState({
    sets: [
      setEntry('p000', { ships: { state: 'current', rendered: '2026-10-02' } }),
      setEntry('p002', { ships: { state: 'stale', reasons: ['fm moved'] } }),
    ],
  });
});

describe('the table', () => {
  it('lists every voice in address order under its family headings', () => {
    mount();
    expect(rows()).toEqual([
      'v-p000',
      'v-p001',
      'v-p002',
      'v-p002b8',
      'v-p002b1',
      'v-p003',
      'v-k000',
    ]);
    const heads = screen
      .getAllByRole('heading', { level: 2 })
      .filter((h) => h.className === 'bank-group');
    expect(heads.map((h) => h.textContent)).toEqual(['Keys2', 'Strings4', 'Kits1']);
  });

  it('writes each row from the voice it holds', () => {
    mount();
    const row = document.getElementById('v-p002') as HTMLElement;
    expect(within(row).getByText('2')).toHaveClass('addr');
    expect(row.textContent).toContain('Rubber Band');
    expect(row.textContent).toContain('2 unwritten');
    expect(row.querySelector('.stage')?.textContent).toBe('1.0');
    expect(row.querySelector('.engine')?.textContent).toContain('fm');
    expect(row.querySelector('.page-cell')?.textContent).toBe('old');
    expect(row.querySelector('.oracle')?.textContent).toBe('cap-a');
    expect(row.querySelector('.aim')?.textContent).toBe('module');
    expect(
      (document.getElementById('v-p002b8') as HTMLElement).querySelector('.addr')?.textContent,
    ).toBe('2:8');
    expect(
      (document.getElementById('v-k000') as HTMLElement).querySelector('.addr')?.textContent,
    ).toBe('kit 0');
  });

  it('marks a missing capture quietly and an engine not reported', () => {
    mount();
    const anvil = document.getElementById('v-p003') as HTMLElement;
    expect(anvil.querySelector('.engine')?.textContent).toContain(en['bank.notReported']);
    const tin = document.getElementById('v-p001') as HTMLElement;
    expect(tin.querySelector('.oracle')).toHaveClass('absent');
    expect(tin.querySelector('.oracle')?.textContent).toBe(en['tst.uncaptured']);
    const rubber = document.getElementById('v-p002') as HTMLElement;
    expect(rubber.querySelector('.oracle')).toHaveClass('off');
  });

  it('shows the next action only between the oracle step and the first rung', () => {
    mount();
    expect(document.getElementById('v-p002')?.querySelector('.next')).toBeNull();
    expect(document.getElementById('v-p002b8')?.querySelector('.next')?.textContent).toBe(
      'record a listening verdict',
    );
    expect(document.getElementById('v-p001')?.querySelector('.next')).toBeNull();
    expect(document.getElementById('v-k000')?.querySelector('.next')).toBeNull();
  });

  it('carries the heard and claim chips from the ear and the ledger', () => {
    setState({
      feedbackIndex: { p000: { n: 3, worst: 'broken', prefer: 0, last: '', off_reference: 0 } },
    });
    mount();
    expect(document.getElementById('v-p000')?.querySelector('.heard')).toHaveClass('on', 'bad');
    expect(document.getElementById('v-p001')?.querySelector('.heard')).not.toHaveClass('on');
    const kit = document.getElementById('v-k000') as HTMLElement;
    expect([...kit.querySelectorAll('.sg')].map((c) => c.className)).toEqual(['sg good', 'sg bad']);
  });
});

describe('the masthead', () => {
  it('counts the bank', () => {
    mount();
    const fig = (label: string) => screen.getByRole('button', { name: new RegExp(label) });
    expect(fig(`^7${en['bank.voices']}`)).toBeDisabled();
    expect(fig(`^3${en['bank.withOracle']}`)).toBeEnabled();
    expect(fig(`^2${en['bank.unwritten']}`)).toHaveClass('warn');
    expect(fig(`^2${en['bank.rendered']}`)).toBeEnabled();
    expect(fig(`^1${en['bank.pagesStale']}`)).toHaveClass('warn');
  });

  it('turns a figure into a filter and back', async () => {
    const { user } = mount();
    const stale = screen.getByRole('button', { name: new RegExp(en['bank.pagesStale']) });
    await user.click(stale);
    expect(rows()).toEqual(['v-p002']);
    await user.click(stale);
    expect(rows()).toHaveLength(7);
  });

  it('keeps a figure with nothing behind it inert', () => {
    resetBank({ voices: bank().filter((v) => v.slug === 'p001'), loaded: true });
    mount();
    expect(screen.getByRole('button', { name: new RegExp(en['bank.unwritten']) })).toBeDisabled();
  });

  it('filters by stage from the distribution keys, which count their rung', async () => {
    const { user } = mount();
    const keys = screen.getByRole('group', { name: en['bank.colStage'] });
    const untouched = within(keys).getByRole('button', { name: new RegExp(en['stage.0']) });
    expect(untouched).toHaveTextContent('1');
    await user.click(within(keys).getByRole('button', { name: new RegExp(en['stage.2']) }));
    expect(rows()).toEqual(['v-p002b8', 'v-p002b1']);
    expect(within(keys).getByRole('button', { name: new RegExp(en['stage.2']) })).toHaveAttribute(
      'aria-pressed',
      'true',
    );
    await user.click(within(keys).getByRole('button', { name: new RegExp(en['stage.2']) }));
    expect(rows()).toHaveLength(7);
  });
});

describe('the toolbar', () => {
  it('filters by slot and by method, each exclusive', async () => {
    const { user } = mount();
    const slot = screen.getByRole('group', { name: en['bank.slot'] });
    await user.click(within(slot).getByRole('button', { name: en['slot.short.kit'] }));
    expect(rows()).toEqual(['v-k000']);
    expect(within(slot).getByRole('button', { name: en['slot.short.kit'] })).toHaveAttribute(
      'aria-pressed',
      'true',
    );
    expect(within(slot).getByRole('button', { name: en['slot.short.gm'] })).toHaveAttribute(
      'aria-pressed',
      'false',
    );
    await user.click(
      within(slot).getAllByRole('button', { name: en['bank.any'] })[0] as HTMLElement,
    );
    const method = screen.getByRole('group', { name: en['bank.method'] });
    await user.click(within(method).getByRole('button', { name: en['bank.fm'] }));
    expect(rows()).toEqual(['v-p002', 'v-p002b8', 'v-p002b1']);
  });

  it('toggles the filter chips', async () => {
    const { user } = mount();
    const chip = screen.getByRole('button', { name: en['bank.fNeedsOracle'] });
    await user.click(chip);
    expect(chip).toHaveAttribute('aria-pressed', 'true');
    expect(rows()).toEqual(['v-p001', 'v-p002b8', 'v-p002b1', 'v-k000']);
    await user.click(screen.getByRole('button', { name: en['bank.fHasOracle'] }));
    expect(chip).toHaveAttribute('aria-pressed', 'false');
    expect(rows()).toEqual(['v-p000', 'v-p002', 'v-p003']);
  });

  it('finds as you type and counts what is shown', async () => {
    const { user } = mount();
    expect(document.getElementById('bankCount')).toHaveTextContent('7 / 7');
    await user.type(screen.getByRole('searchbox', { name: en['bank.find'] }), 'rubber');
    expect(rows()).toEqual(['v-p002', 'v-p002b8', 'v-p002b1']);
    expect(document.getElementById('bankCount')).toHaveTextContent('3 / 7');
    await user.type(screen.getByRole('searchbox'), 'zzz');
    expect(screen.getByText(en['bank.nothingMatches'])).toBeInTheDocument();
    expect(inspector()).toHaveTextContent(en['bank.selectOne']);
  });

  it('sorts from the select, and drops the family headings off the address order', async () => {
    const { user } = mount();
    await user.selectOptions(screen.getByRole('combobox', { name: en['bank.sort'] }), 'stage');
    expect(rows()[0]).toBe('v-p002');
    expect(document.querySelector('.bank-group')).toBeNull();
    await user.selectOptions(screen.getByRole('combobox', { name: en['bank.sort'] }), 'name');
    expect(rows()[0]).toBe('v-p003');
    await user.selectOptions(screen.getByRole('combobox', { name: en['bank.sort'] }), 'address');
    expect(rows()[0]).toBe('v-p000');
    expect(document.querySelectorAll('.bank-group')).toHaveLength(3);
  });

  it('keeps its choices when the view is left and entered again', async () => {
    const { user, unmount } = mount();
    await user.type(screen.getByRole('searchbox'), 'tin');
    unmount();
    mount();
    expect(rows()).toEqual(['v-p001']);
    expect(getBank().query.find).toBe('tin');
  });
});

describe('the cursor and the keyboard', () => {
  it('starts on the first row and follows a click, which also focuses the list', async () => {
    const { user } = mount();
    expect(selected()).toBe('v-p000');
    await user.click(document.getElementById('v-p002') as HTMLElement);
    expect(selected()).toBe('v-p002');
    expect(screen.getByRole('listbox')).toHaveFocus();
    expect(screen.getByRole('listbox')).toHaveAttribute('aria-activedescendant', 'v-p002');
    expect(document.getElementById('v-p002')).toHaveAttribute('aria-current', 'true');
    expect(document.getElementById('v-p000')).not.toHaveAttribute('aria-current');
  });

  it('moves with the arrows and J K, wrapping at the ends', async () => {
    const { user } = mount();
    screen.getByRole('listbox').focus();
    await user.keyboard('{ArrowDown}');
    expect(selected()).toBe('v-p001');
    await user.keyboard('j');
    expect(selected()).toBe('v-p002');
    await user.keyboard('k{ArrowUp}');
    expect(selected()).toBe('v-p000');
    await user.keyboard('{ArrowUp}');
    expect(selected()).toBe('v-k000');
    await user.keyboard('{ArrowDown}');
    expect(selected()).toBe('v-p000');
  });

  it('jumps to the ends with Home and End', async () => {
    const { user } = mount();
    screen.getByRole('listbox').focus();
    await user.keyboard('{End}');
    expect(selected()).toBe('v-k000');
    await user.keyboard('{Home}');
    expect(selected()).toBe('v-p000');
  });

  it('moves only within what is shown', async () => {
    const { user } = mount();
    await user.type(screen.getByRole('searchbox'), 'rubber');
    screen.getByRole('listbox').focus();
    await user.keyboard('{End}');
    expect(selected()).toBe('v-p002b1');
    await user.keyboard('{ArrowDown}');
    expect(selected()).toBe('v-p002');
  });

  it('opens the voice on Enter when it has a page, and does nothing when it has none', async () => {
    const { user, onOpen } = mount();
    screen.getByRole('listbox').focus();
    await user.keyboard('{Enter}');
    expect(onOpen).toHaveBeenCalledWith('p000');
    onOpen.mockClear();
    await user.keyboard('j{Enter}');
    expect(onOpen).not.toHaveBeenCalled();
  });

  it('opens on a double click, and not for a row with no page', async () => {
    const { user, onOpen } = mount();
    await user.dblClick(document.getElementById('v-p002') as HTMLElement);
    expect(onOpen).toHaveBeenCalledWith('p002');
    onOpen.mockClear();
    await user.dblClick(document.getElementById('v-p001') as HTMLElement);
    expect(onOpen).not.toHaveBeenCalled();
  });

  it('focuses the find box on / without typing the slash', async () => {
    const { user } = mount();
    screen.getByRole('listbox').focus();
    await user.keyboard('/');
    expect(screen.getByRole('searchbox')).toHaveFocus();
    expect(screen.getByRole('searchbox')).toHaveValue('');
  });

  it('leaves letters to the find box while it has focus', async () => {
    const { user } = mount();
    await user.type(screen.getByRole('searchbox'), 'jk');
    expect(screen.getByRole('searchbox')).toHaveValue('jk');
    expect(getBank().cursor).toBeNull();
  });

  it('answers bankKey for a caller that owns the listener, once per key', () => {
    mount();
    const press = (key: string) => {
      const ev = new KeyboardEvent('keydown', { key, cancelable: true, bubbles: true });
      let first = false;
      act(() => {
        first = bankKey(ev);
      });
      return [first, bankKey(ev)] as const;
    };
    expect(press('ArrowDown')).toEqual([true, false]);
    expect(selected()).toBe('v-p001');
    expect(press('x')).toEqual([false, false]);
    expect(press('Enter')[0]).toBe(true);
  });

  it('does not take a key with a modifier', () => {
    mount();
    const ev = new KeyboardEvent('keydown', { key: 'ArrowDown', metaKey: true, cancelable: true });
    expect(bankKey(ev)).toBe(false);
    expect(selected()).toBe('v-p000');
  });
});

describe('the inspector', () => {
  it('shows the legend with nothing selected', () => {
    resetBank({ voices: [], loaded: true });
    mount();
    expect(screen.getByText(en['bank.notGenerated'])).toBeInTheDocument();
    expect(inspector()).toHaveTextContent(en['bank.ladder']);
    expect(inspector().querySelectorAll('.rung.done')).toHaveLength(6);
    expect(inspector().querySelectorAll('.legend > div')).toHaveLength(3);
  });

  it('follows the cursor', async () => {
    const { user } = mount();
    expect(within(inspector()).getByRole('heading')).toHaveTextContent('Glass Harmonica');
    await user.click(document.getElementById('v-p001') as HTMLElement);
    expect(within(inspector()).getByRole('heading')).toHaveTextContent('Tin Whistle');
    expect(inspector()).toHaveTextContent(en['bank.notCaptured']);
    expect(inspector()).toHaveTextContent('capture a reference');
  });

  it('holds the selection on the voice when a filter keeps it on screen', async () => {
    const { user } = mount();
    await user.click(document.getElementById('v-p002b1') as HTMLElement);
    await user.click(screen.getByRole('button', { name: en['bank.fNeedsOracle'] }));
    expect(selected()).toBe('v-p002b1');
    expect(within(inspector()).getByRole('heading')).toHaveTextContent('Rubber Band');
    expect(inspector()).toHaveTextContent('2:1');
  });

  it('marks the rung the voice is on and what the earlier ones were', () => {
    mount();
    const rungs = [...inspector().querySelectorAll('.ladder .rung')].map((r) => r.className);
    expect(rungs).toEqual(['rung done', 'rung done', 'rung done', 'rung here', 'rung', 'rung']);
    expect(inspector().querySelector('.rung.here .mark')?.textContent).toBe('▸');
  });

  it('writes coverage, gaps and agreement for a measured voice', async () => {
    const { user } = mount();
    await user.click(document.getElementById('v-p003') as HTMLElement);
    const text = inspector().textContent ?? '';
    expect(text).toContain('4 of 6 gated');
    expect(text).toContain('1 excused: stretch');
    expect(text).toContain('decay, tnr');
    expect(text).toContain('3 of 5 inside the spread');
    expect(text).toContain('centroid 2×');
    expect(text).toContain('2 timbres, 40 rows, gate green');
    expect(inspector().querySelector('dd.warn')).not.toBeNull();
  });

  it('lists unwritten settings and offers to listen only where a page exists', async () => {
    const { user } = mount();
    await user.click(document.getElementById('v-p002') as HTMLElement);
    expect(inspector()).toHaveTextContent('rubber.tension, rubber.damp');
    await user.click(screen.getByRole('button', { name: en['bank.listen'] }));
    await user.click(document.getElementById('v-p001') as HTMLElement);
    expect(screen.queryByRole('button', { name: en['bank.listen'] })).toBeNull();
  });

  it('hands the voice to the caller from the listen button', async () => {
    const { user, onOpen } = mount();
    await user.click(screen.getByRole('button', { name: en['bank.listen'] }));
    expect(onOpen).toHaveBeenCalledWith('p000');
  });

  it('says what ships and what the page was rendered as, for a voice with a page', () => {
    resetBank({
      voices: [
        {
          ...bank()[0],
          ships: { unit: 'modal', version: 4, date: '2026-10-09' },
        } as ReturnType<typeof bank>[number],
      ],
      loaded: true,
    });
    mount();
    expect(inspector()).toHaveTextContent('modal v4 (2026-10-09)');
    expect(inspector()).toHaveTextContent('rendered 2026-10-02');
  });
});

describe('both languages', () => {
  it('render without a raw key and change with the language', async () => {
    const { user } = mount();
    await user.click(document.getElementById('v-p003') as HTMLElement);
    const en1 = document.getElementById('bank')?.textContent ?? '';
    expect(en1).toContain(en['bank.colProg']);
    setLang('ja');
    const ja1 = await screen.findByText(tableOf('ja')['bank.colProg']);
    expect(ja1).toBeInTheDocument();
    const text = document.getElementById('bank')?.textContent ?? '';
    expect(text).not.toMatch(/\b(bank|stage|slot|method|tgt|tst|ship|id|pick|keys)\.[a-zA-Z0-9]+/);
    expect(text).toContain(tableOf('ja')['stage.3']);
    expect(text).toContain(tableOf('ja')['bank.sortStage']);
    expect(screen.getByRole('listbox')).toHaveAttribute('aria-label', tableOf('ja')['bank.voices']);
  });
});
