import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { resetBank } from '../../src/bank/store';
import { setLang, tableOf } from '../../src/i18n';
import { getState, setState } from '../../src/state/store';
import { VoicePalette, VoicePickButton } from '../../src/voice/Palette';
import { paletteRows } from '../../src/voice/paletteRows';
import { bank, setEntry } from './fixtures';

const en = tableOf('en');

const sets = () => [
  setEntry('p000', { title: 'Glass Harmonica', group: 'Keys' }),
  setEntry('p001', { title: 'Tin Whistle', group: 'Keys', compare: false }),
  setEntry('p002', { title: 'Rubber Band', group: 'Strings' }),
  setEntry('outside', { title: 'Hand Made', group: '' }),
];

function mount() {
  const onPick = vi.fn();
  const user = userEvent.setup();
  render(
    <>
      <VoicePickButton />
      <VoicePalette onPick={onPick} />
    </>,
  );
  return { onPick, user };
}

const panel = () => document.getElementById('voicePanel') as HTMLElement;
const options = () => within(panel()).queryAllByRole('option');

beforeEach(() => {
  resetBank({ voices: bank(), loaded: true });
  setState({ sets: sets(), setId: 'p001' });
});

describe('paletteRows', () => {
  it('lists the sets in index order and opens a heading where the group changes', () => {
    const rows = paletteRows(sets(), bank(), '');
    expect(rows.map((r) => r.set.id)).toEqual(['p000', 'p001', 'p002', 'outside']);
    expect(rows.map((r) => r.heading)).toEqual(['Keys', '', 'Strings', '']);
  });

  it('joins each set to its bank row, and leaves a set outside the bank bare', () => {
    const rows = paletteRows(sets(), bank(), '');
    expect(rows[0]?.voice?.name).toBe('Glass Harmonica');
    expect(rows[3]?.voice).toBeNull();
  });

  it('searches the id, title, group, name, patch and engine', () => {
    const ids = (q: string) => paletteRows(sets(), bank(), q).map((r) => r.set.id);
    expect(ids('GLASS')).toEqual(['p000']);
    expect(ids('subtractive')).toEqual(['p001']);
    expect(ids('strings')).toEqual(['p002']);
    expect(ids('rubber')).toEqual(['p002']);
    expect(ids('hand made')).toEqual(['outside']);
    expect(ids('nothing here')).toEqual([]);
  });

  it('re-opens a heading for the first row shown in a group', () => {
    const rows = paletteRows(sets(), bank(), 'tin');
    expect(rows.map((r) => r.heading)).toEqual(['Keys']);
  });
});

describe('the pick button', () => {
  it('names the voice being listened to', () => {
    mount();
    const btn = screen.getByRole('button', { name: en['head.voice'] });
    expect(btn).toHaveTextContent('Tin Whistle');
    expect(btn.querySelector('.vp-id')).toHaveTextContent('p001');
    expect(btn.querySelector('.dot')).toHaveClass('classic');
    expect(btn).toHaveAttribute('aria-expanded', 'false');
  });

  it('falls back to the set id for a voice outside the bank', () => {
    setState({ setId: 'outside' });
    mount();
    expect(screen.getByRole('button', { name: en['head.voice'] })).toHaveTextContent('outside');
  });

  it('toggles the palette', async () => {
    const { user } = mount();
    const btn = screen.getByRole('button', { name: en['head.voice'] });
    await user.click(btn);
    expect(getState().paletteOpen).toBe(true);
    expect(btn).toHaveAttribute('aria-expanded', 'true');
    await user.click(btn);
    expect(getState().paletteOpen).toBe(false);
  });
});

describe('the palette', () => {
  it('stays hidden, and empty, until opened', () => {
    mount();
    expect(panel()).toHaveAttribute('hidden');
    expect(options()).toHaveLength(0);
  });

  it('opens with every voice, focus in the find box, and the current voice selected', async () => {
    const { user } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    expect(panel()).not.toHaveAttribute('hidden');
    expect(screen.getByRole('searchbox', { name: en['pick.find'] })).toHaveFocus();
    expect(options()).toHaveLength(4);
    expect(options().filter((o) => o.getAttribute('aria-selected') === 'true')).toHaveLength(1);
    expect(options()[1]).toHaveAttribute('aria-selected', 'true');
    expect(panel().querySelector('.mono')).toHaveTextContent('4');
    expect(screen.getByText(en['pick.legend'])).toBeInTheDocument();
  });

  it('draws the heading and the three readouts on each row', async () => {
    setState({
      feedbackIndex: { p000: { n: 2, worst: 'ok', prefer: 1, last: '', off_reference: 0 } },
    });
    const { user } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    expect([...panel().querySelectorAll('.palette-group')].map((g) => g.textContent)).toEqual([
      'Keys',
      'Strings',
    ]);
    const first = options()[0] as HTMLElement;
    expect(first.querySelector('.addr')?.textContent).toBe('0');
    expect(first.querySelector('.who')?.textContent).toBe('Glass Harmonica  glass');
    expect(first.querySelector('.stage')?.textContent).toBe('0.6');
    expect(first.querySelectorAll('.bar .seg.on')).toHaveLength(3);
    expect(first.querySelector('.heard')).toHaveClass('on', 'good');
    expect(first.querySelector('.heard')?.getAttribute('title')).toContain('2 listening notes');
    expect(first.querySelector('.solo')?.textContent).toBe('');
    expect((options()[1] as HTMLElement).querySelector('.solo')?.textContent).toBe(
      en['pick.playOnly'],
    );
    const outside = options()[3] as HTMLElement;
    expect(outside.querySelector('.who')?.textContent).toBe('Hand Made');
    expect(outside.querySelector('.addr')?.textContent).toBe('');
  });

  it('filters as you type and counts what is left', async () => {
    const { user } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.keyboard('rubber');
    expect(options().map((o) => o.querySelector('.who')?.textContent)).toEqual([
      'Rubber Band  rubber',
    ]);
    expect(panel().querySelector('.mono')).toHaveTextContent('1 / 4');
    await user.clear(screen.getByRole('searchbox'));
    await user.keyboard('qqq');
    expect(options()).toHaveLength(0);
    expect(screen.getByText(en['bank.nothingMatches'])).toBeInTheDocument();
  });

  it('starts from an empty filter every time it opens', async () => {
    const { user } = mount();
    const btn = screen.getByRole('button', { name: en['head.voice'] });
    await user.click(btn);
    await user.keyboard('tin');
    await user.click(btn);
    await user.click(btn);
    expect(screen.getByRole('searchbox')).toHaveValue('');
    expect(options()).toHaveLength(4);
  });

  it('walks the rows with the arrows and comes back to the find box', async () => {
    const { user } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.keyboard('{ArrowDown}');
    expect(options()[0]).toHaveFocus();
    await user.keyboard('{ArrowDown}{ArrowDown}');
    expect(options()[2]).toHaveFocus();
    await user.keyboard('{ArrowDown}{ArrowDown}{ArrowDown}');
    expect(options()[3]).toHaveFocus();
    await user.keyboard('{ArrowUp}');
    expect(options()[2]).toHaveFocus();
    await user.keyboard('{ArrowUp}{ArrowUp}');
    expect(options()[0]).toHaveFocus();
    await user.keyboard('{ArrowUp}');
    expect(screen.getByRole('searchbox')).toHaveFocus();
  });

  it('picks the first match on Enter from the find box', async () => {
    const { user, onPick } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.keyboard('band{Enter}');
    expect(onPick).toHaveBeenCalledWith('p002');
    expect(getState().paletteOpen).toBe(false);
  });

  it('picks the focused row on Enter, once', async () => {
    const { user, onPick } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.keyboard('{ArrowDown}{ArrowDown}{Enter}');
    expect(onPick).toHaveBeenCalledTimes(1);
    expect(onPick).toHaveBeenCalledWith('p001');
  });

  it('picks with the mouse and closes', async () => {
    const { user, onPick } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.click(options()[0] as HTMLElement);
    expect(onPick).toHaveBeenCalledWith('p000');
    expect(getState().paletteOpen).toBe(false);
    expect(panel()).toHaveAttribute('hidden');
  });

  it('closes on Escape and gives focus back to the button', async () => {
    const { user, onPick } = mount();
    const btn = screen.getByRole('button', { name: en['head.voice'] });
    await user.click(btn);
    await user.keyboard('{Escape}');
    expect(getState().paletteOpen).toBe(false);
    expect(btn).toHaveFocus();
    expect(onPick).not.toHaveBeenCalled();
  });

  it('closes on a click away, but not on a click inside it', async () => {
    const { user } = mount();
    await user.click(screen.getByRole('button', { name: en['head.voice'] }));
    await user.click(screen.getByRole('searchbox'));
    expect(getState().paletteOpen).toBe(true);
    await user.click(document.body);
    expect(getState().paletteOpen).toBe(false);
  });

  it('answers to the store, so a shell can open it', async () => {
    mount();
    setState({ paletteOpen: true });
    expect(await screen.findByRole('searchbox')).toBeInTheDocument();
    expect(options()).toHaveLength(4);
  });
});

describe('both languages', () => {
  it('render the chrome in the language in force, with no raw key', async () => {
    const { user } = mount();
    setLang('ja');
    await user.click(await screen.findByRole('button', { name: tableOf('ja')['head.voice'] }));
    expect(screen.getByRole('searchbox', { name: tableOf('ja')['pick.find'] })).toHaveAttribute(
      'placeholder',
      tableOf('ja')['pick.findPlaceholder'],
    );
    expect(screen.getByText(tableOf('ja')['pick.legend'])).toBeInTheDocument();
    expect(panel().textContent).not.toMatch(/\b(pick|head|bank|stage)\.[a-zA-Z]+/);
    expect((options()[1] as HTMLElement).querySelector('.solo')?.textContent).toBe(
      tableOf('ja')['pick.playOnly'],
    );
  });
});
