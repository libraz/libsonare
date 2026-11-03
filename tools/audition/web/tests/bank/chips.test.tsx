import { render } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { tableOf } from '../../src/i18n';
import { setState } from '../../src/state/store';
import { HeardChip, MethodDot, ShipChip, SignoffChip, StageBar } from '../../src/voice/chips';
import { voice } from './fixtures';

const en = tableOf('en');
const row = (worst: string, over: Record<string, unknown> = {}) => ({
  n: 2,
  worst,
  prefer: 0,
  last: '',
  off_reference: 0,
  ...over,
});

describe('MethodDot', () => {
  it('takes the method class from the engine', () => {
    expect(render(<MethodDot engine="fm" />).container.firstElementChild).toHaveClass('dot', 'fm');
    expect(render(<MethodDot engine="reed" />).container.firstElementChild).toHaveClass('physical');
    expect(render(<MethodDot engine={undefined} />).container.firstElementChild).toHaveClass(
      'classic',
    );
  });
});

describe('StageBar', () => {
  const segs = (stage: number) => {
    const { container } = render(<StageBar stage={stage} />);
    return {
      on: container.querySelectorAll('.seg.on').length,
      all: container.querySelectorAll('.seg').length,
      color: (container.firstElementChild as HTMLElement).style.getPropertyValue('--sc'),
    };
  };

  it('fills one segment per fifth, in the rung colour', () => {
    expect(segs(0)).toEqual({ on: 0, all: 5, color: 'var(--s0)' });
    expect(segs(0.6)).toEqual({ on: 3, all: 5, color: 'var(--s3)' });
    expect(segs(1)).toEqual({ on: 5, all: 5, color: 'var(--s5)' });
  });
});

describe('HeardChip', () => {
  const chip = (id = 'a') =>
    render(<HeardChip id={id} />).container.firstElementChild as HTMLElement;

  it('is empty where nothing has been said', () => {
    const el = chip();
    expect(el).toHaveClass('heard');
    expect(el).not.toHaveClass('on');
    expect(el.textContent).toBe('');
  });

  it.each([
    ['broken', 'bad'],
    ['wrong-instrument', 'bad'],
    ['acceptable', 'mid'],
    ['off', 'mid'],
    ['ok', 'good'],
  ])('shows the count and tones a worst verdict of %s as %s', (worst, tone) => {
    setState({ feedbackIndex: { a: row(worst) } });
    const el = chip();
    expect(el).toHaveClass('heard', 'on', tone);
    expect(el.textContent).toBe('2');
  });

  it('has no tone for a verdict it does not know', () => {
    setState({ feedbackIndex: { a: row('unsure') } });
    expect(chip().className).toBe('heard on');
  });

  it('says in words what was heard', () => {
    setState({ feedbackIndex: { a: row('wrong-instrument', { prefer: 3 }) } });
    expect(chip().title).toBe(
      `2 listening notes · ${en['grade.wrongInstrument']} · 3 put forward to keep`,
    );
  });

  it('reads another voice’s chip from its own id', () => {
    setState({ feedbackIndex: { a: row('ok') } });
    expect(chip('b')).not.toHaveClass('on');
  });
});

describe('SignoffChip', () => {
  const chips = (v: ReturnType<typeof voice> | null) =>
    [...render(<SignoffChip voice={v} />).container.querySelectorAll('.sg')] as HTMLElement[];

  it('is empty without a voice or without claims', () => {
    expect(chips(null)).toHaveLength(0);
    expect(chips(voice())).toHaveLength(0);
  });

  it('shows each claim in the tone of its state, with the meaning on hover', () => {
    const got = chips(
      voice({ axes: { structure: { state: 'current' }, music: { state: 'stale' } } }),
    );
    expect(got.map((c) => [c.textContent, c.className])).toEqual([
      [en['pick.structure'], 'sg good'],
      [en['pick.music'], 'sg bad'],
    ]);
    expect(got[0]?.title).toBe(`${en['pick.structure.long']} — ${en['pick.current']}`);
  });

  it('tones an unverified claim as middling and shows only the claim made', () => {
    const got = chips(voice({ axes: { music: { state: 'unverified' } } }));
    expect(got.map((c) => c.className)).toEqual(['sg mid']);
  });
});

describe('ShipChip', () => {
  it('carries the state and the reasons', () => {
    const { container } = render(<ShipChip ships={{ state: 'stale', reasons: ['moved'] }} />);
    const el = container.firstElementChild as HTMLElement;
    expect(el).toHaveClass('idstate');
    expect(el).toHaveAttribute('data-state', 'stale');
    expect(el.textContent).toBe(en['ship.stale']);
    expect(el.title).toBe('moved');
  });

  it('uses the short word and the no-record state', () => {
    expect(render(<ShipChip ships={{ state: 'stale' }} short />).container.textContent).toBe(
      en['ship.short.stale'],
    );
    const none = render(<ShipChip ships={null} />).container.firstElementChild as HTMLElement;
    expect(none).toHaveAttribute('data-state', 'none');
    expect(none.title).toBe(en['ship.none.long']);
  });
});
