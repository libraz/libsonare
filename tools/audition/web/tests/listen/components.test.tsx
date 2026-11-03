import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import * as feedback from '../../src/feedback/actions';
import { setLang, t } from '../../src/i18n';
import { displayOrder } from '../../src/listen/display';
import { Takes } from '../../src/listen/Takes';
import { conditionsText, Transport } from '../../src/listen/Transport';
import { NowLine, Versions } from '../../src/listen/Versions';
import { commitBuffers, setContextFactory } from '../../src/player/engine';
import { activeKey } from '../../src/state/selectors';
import { getState, setState } from '../../src/state/store';
import { FakeContext, fakeBuffer } from '../audio';
import { comparedManifest, opened, soloManifest } from '../fixtures';

let ctx: FakeContext;

function open(...args: Parameters<typeof opened>) {
  const s = opened(...args);
  setState({ display: displayOrder(s) });
  commitBuffers(
    Object.fromEntries(
      (s.take?.keys ?? []).map((k) => [k, fakeBuffer(4) as unknown as AudioBuffer]),
    ),
  );
  return getState();
}

beforeEach(() => {
  ctx = new FakeContext();
  setContextFactory(() => ctx as unknown as AudioContext);
});

const pressed = (box: HTMLElement) =>
  within(box)
    .getAllByRole('button')
    .filter((b) => b.getAttribute('aria-pressed') === 'true')
    .map((b) => b.textContent);

describe('the version switch', () => {
  it('shows one block per question, each with its role, numbered in display order', () => {
    open();
    const { container } = render(<Versions />);
    const rows = container.querySelectorAll('.vrow');
    // The comparison row, then model, reference and comparison blocks.
    expect(rows).toHaveLength(4);
    const digits = [...container.querySelectorAll('.segmented button .key')].map(
      (k) => k.textContent,
    );
    expect(digits).toEqual(['1', '2', '3', '4']);
    expect(rows[1]).toHaveClass('role-model');
    expect(within(rows[1] as HTMLElement).getByText('Softer hammer')).toBeInTheDocument();
    expect(within(rows[1] as HTMLElement).getByText(t('ver.tagCandidate'))).toBeInTheDocument();
    expect(within(rows[1] as HTMLElement).getByText(t('ver.hintModel'))).toBeInTheDocument();
  });

  it('marks the comparison in force and switches on a click', async () => {
    open();
    const { container } = render(<Versions />);
    const cmp = container.querySelector('.vrow') as HTMLElement;
    expect(pressed(cmp)[0]).toContain(t('compare.gm_gs_product'));
    await userEvent.click(within(cmp).getByText(t('compare.instrument_di')));
    expect(getState().comparisonId).toBe('instrument_di');
    expect(pressed(container.querySelector('.vrow') as HTMLElement)[0]).toContain(
      t('compare.instrument_di'),
    );
  });

  it('sounds a version when its button is pressed', async () => {
    open();
    render(<Versions />);
    await userEvent.click(screen.getByText('Recording One'));
    expect(activeKey(getState())).toBe('ref');
    expect(getState().playing).toBe(true);
  });

  it('offers "keep this one" only in the block that is sounding, and posts through the composer', async () => {
    open();
    const prefer = vi.spyOn(feedback, 'recordPreference').mockResolvedValue();
    const { container } = render(<Versions />);
    const keep = container.querySelector('[data-block="model|product"]') as HTMLButtonElement;
    expect(keep.hidden).toBe(false);
    await userEvent.click(keep);
    expect(prefer).toHaveBeenCalledTimes(1);
    act(() => setState({ versionIndex: (getState().take?.keys ?? []).indexOf('ref') }));
    expect(keep.hidden).toBe(true);
  });

  it('hides every name behind letters when blind', () => {
    open(comparedManifest(), { blind: true, blindOrder: [3, 0, 2] });
    setState({ display: displayOrder(getState()) });
    const { container } = render(<Versions />);
    const names = [...container.querySelectorAll('.vname')].map((n) => n.textContent);
    expect(names.slice(-3)).toEqual(['A', 'B', 'C']);
    expect(container.textContent).not.toContain('Recording One');
    expect(container.querySelector('[data-block]')).toBeNull();
  });

  it('marks an unmodified render older than the bank as stale', () => {
    open(soloManifest(), { setId: 'p019-organ', versionIndex: 0 });
    setState({ display: displayOrder(getState()) });
    const { container } = render(<Versions />);
    expect(container.querySelector('.vtag.warn')?.textContent).toBe(t('ver.tagStale'));
    // On the key's line, so a tagged button is no taller than its neighbours.
    expect(container.querySelector('.vtag.warn')?.parentElement).toHaveClass('vsub');
  });
});

describe('the banner', () => {
  it('names the role, the title and what the version stands for', () => {
    open(comparedManifest(), { versionIndex: 2 });
    const { container } = render(<NowLine />);
    expect(container.querySelector('#nowRole')).toHaveClass('role-model');
    expect(container.querySelector('.now-title')?.textContent).toBe('Softer hammer');
    expect(container.querySelector('.idstate')?.getAttribute('data-state')).toBe('candidate');
  });

  it('offers the other side, coloured for where it goes, and swaps on a click', async () => {
    open();
    const { container } = render(<NowLine />);
    const swap = container.querySelector('#swapBtn') as HTMLButtonElement;
    expect(swap).toHaveClass('role-reference');
    expect(swap.textContent).toBe(t('now.swap'));
    await userEvent.click(swap);
    expect(activeKey(getState())).toBe('ref');
    expect(container.querySelector('#swapBtn')?.textContent).toBe(t('now.swapBack'));
  });

  it('says only the letter when blind, and offers no swap', () => {
    open(comparedManifest(), { blind: true, blindOrder: [3, 0], versionIndex: 1 });
    setState({ display: displayOrder(getState()) });
    const { container } = render(<NowLine />);
    expect(container.querySelector('.now-what')?.textContent).toBe('B');
    expect((container.querySelector('#swapBtn') as HTMLButtonElement).hidden).toBe(true);
  });
});

describe('the transport', () => {
  it('plays and pauses, and says which it will do', async () => {
    open();
    render(<Transport />);
    const play = screen.getByRole('button', { name: new RegExp(t('transport.play')) });
    await userEvent.click(play);
    expect(getState().playing).toBe(true);
    expect(play.textContent).toContain(t('transport.pause'));
  });

  it('shows a marked passage with its way out', async () => {
    open(comparedManifest(), { region: [1.2, 3.4] });
    const { container } = render(<Transport />);
    expect(container.querySelector('#regionSpan')?.textContent).toBe('1.20–3.40 s');
    await userEvent.click(container.querySelector('#clearRegion') as HTMLElement);
    expect(getState().region).toBeNull();
    expect((container.querySelector('#passage') as HTMLElement).hidden).toBe(true);
  });

  it('reads the level of the sounding version, and the gain once matched', () => {
    open(comparedManifest(), { matchRms: true });
    const { container } = render(<Transport />);
    expect(container.querySelector('#levels')?.textContent).toMatch(/-20\.0/);
    expect(container.querySelector('#levels')?.textContent).toContain(
      t('level.gain', { db: '10.9' }).slice(0, 4),
    );
  });

  it('withholds the blind tally until recorded, then names it', async () => {
    open(comparedManifest(), { blind: true, blindOrder: [0, 3], versionIndex: 0 });
    const { container } = render(<Transport />);
    expect(container.querySelector('#blindScore')?.textContent).toBe(t('blind.pickEach'));
    expect((container.querySelector('#blindRecord') as HTMLButtonElement).hidden).toBe(true);
    await userEvent.click(container.querySelector('#blindPick') as HTMLElement);
    expect(container.querySelector('#blindScore')?.textContent).toBe(
      t('blind.decided', { count: 1 }),
    );
    expect(container.querySelector('#blindPick')?.getAttribute('aria-pressed')).toBe('true');
    vi.spyOn(feedback, 'recordBlind').mockResolvedValue(true);
    await userEvent.click(container.querySelector('#blindRecord') as HTMLElement);
    expect(container.querySelector('#blindScore')?.textContent).toContain('library 1');
  });

  it('says why blind mode is refused here', () => {
    open(comparedManifest(), { comparisonId: 'instrument_di' });
    const { container } = render(<Transport />);
    expect(container.querySelector('#blindScore')?.textContent).toBe(
      t('blind.unmatched', { status: t('compare.status.context_only') }),
    );
  });

  it('names the file being heard, and nothing of it when blind', () => {
    open();
    const { container, unmount } = render(<Transport />);
    expect(container.querySelector('#identPath')?.textContent).toBe('s/p000-grand/music/model.wav');
    expect([...container.querySelectorAll('#crumbs span.now')].map((n) => n.textContent)).toEqual([
      'model',
    ]);
    unmount();
    open(comparedManifest(), { blind: true, blindOrder: [0, 3], versionIndex: 0 });
    const blind = render(<Transport />);
    expect(blind.container.querySelector('#identPath')?.textContent).toBe('');
    expect(blind.container.textContent).not.toContain('model.wav');
  });

  it('copies the conditions the note carries', async () => {
    open(comparedManifest(), { region: [0.5, 1.5] });
    const text = conditionsText(0.51, 'http://h', '/');
    expect(text).toContain('set:      p000-grand');
    expect(text).toContain('hit:      #2 — note 64 v90, struck at 0.50 s (0.01 s in)');
    expect(text).toContain('region:   0.50–1.50 s');
    expect(text).toMatch(/http:\/\/h\/\?t=0\.51#p000-grand\/music\/model\?c=gm_gs_product$/);
    const write = vi.fn(async () => {});
    Object.defineProperty(navigator, 'clipboard', {
      value: { writeText: write },
      configurable: true,
    });
    render(<Transport />);
    await userEvent.click(screen.getByText(t('now.copy')));
    expect(write).toHaveBeenCalledWith(expect.stringContaining('set:      p000-grand'));
    expect(screen.getByText(t('now.copied'))).toBeInTheDocument();
  });

  it('renders in Japanese with no raw keys', () => {
    open();
    setLang('ja');
    const { container } = render(
      <>
        <Transport />
        <Versions />
        <NowLine />
      </>,
    );
    expect(container.textContent).toContain(t('transport.play'));
    expect(container.textContent).not.toMatch(/\b(ver|now|transport|role|compare)\.[a-z]/);
  });
});

describe('the take list', () => {
  it('lists every take, marks the open one and the ones holding a reference', async () => {
    open();
    const { container } = render(<Takes />);
    const buttons = container.querySelectorAll('nav#takes button');
    expect(buttons).toHaveLength(2);
    expect(buttons[0]?.getAttribute('aria-current')).toBe('true');
    expect(container.querySelectorAll('.take-ref')).toHaveLength(2);
    expect(container.querySelector('.group')).not.toBeNull();
    expect(buttons[1]?.querySelector('.sub')?.textContent).toBe('single-c4');
  });
});

describe('the header voice picker', () => {
  it('sits in the listen-only block the bank view hides', async () => {
    const { VoicePickButton } = await import('../../src/voice/Palette');
    open();
    const { container } = render(<VoicePickButton />);
    const pick = container.querySelector('#voicePick');
    expect(pick?.closest('.pick.listen-only')).not.toBeNull();
    expect(container.querySelector('.pick-lab')?.textContent).toBe(t('head.voice'));
  });
});
