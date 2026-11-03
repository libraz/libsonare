import { act, render } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { Item } from '../../src/api/schemas';
import { setLang } from '../../src/i18n';
import * as model from '../../src/path/model';
import { PathLine } from '../../src/path/PathLine';
import { playhead } from '../../src/player/engine';
import { getState, setState } from '../../src/state/store';
import { controlFrames, loadTake } from './fakes';

vi.mock('../../src/player/engine');
vi.mock('../../src/path/model', async (orig) => {
  const real = await orig<typeof import('../../src/path/model')>();
  return { ...real, buildPathView: vi.fn(real.buildPathView) };
});

const part = (stages: string[], enabled: boolean[] = []) => ({
  part: 0,
  stages,
  enabled,
  send_tap: 'post_unit',
});

function recorded() {
  loadTake({
    items: [
      {
        id: 't',
        tracks: {},
        evidence: {
          m: {
            status: 'recorded',
            path: {
              events: [
                { kind: 'topology', frame: 0, parts: [part(['saturation.ampSim'])], units: [] },
                {
                  kind: 'topology',
                  frame: 2000,
                  parts: [part(['saturation.ampSim'], [false])],
                  units: [],
                },
              ],
            },
          },
        },
      } as unknown as Item,
    ],
  });
}

const line = (c: HTMLElement) => c.querySelector('#pathLine') as HTMLElement;
const texts = (c: HTMLElement) => [...line(c).children].map((e) => e.textContent);

beforeEach(() => {
  vi.mocked(playhead).mockReturnValue(0);
  vi.mocked(model.buildPathView).mockClear();
});

describe('PathLine', () => {
  it('is an empty #pathLine without a take', () => {
    controlFrames();
    const { container } = render(<PathLine />);
    expect(line(container).className).toBe('path-line');
    expect(line(container).children).toHaveLength(0);
  });

  it('draws the chain with the class each span carries', () => {
    controlFrames();
    recorded();
    const { container } = render(<PathLine />);
    const spans = [...line(container).children];
    expect(spans.map((s) => s.className)).toEqual(['path-head', 'path-chain', 'path-change']);
    expect(texts(container)[1]).toBe('voice → amp → sends');
    expect(line(container).title).toContain('"kind":"topology"');
  });

  it('follows the playhead across a topology boundary', () => {
    const frames = controlFrames();
    recorded();
    const { container } = render(<PathLine />);
    expect(texts(container)[1]).toBe('voice → amp → sends');
    vi.mocked(playhead).mockReturnValue(2);
    act(() => frames.run());
    expect(texts(container)[1]).toBe('voice → amp (bypassed) → sends');
    vi.mocked(playhead).mockReturnValue(0.5);
    act(() => frames.run());
    expect(texts(container)[1]).toBe('voice → amp → sends');
  });

  it('builds the line again only when what it depends on changed', () => {
    const frames = controlFrames();
    recorded();
    render(<PathLine />);
    expect(model.buildPathView).toHaveBeenCalledTimes(1);
    for (const at of [0.1, 0.5, 1.9]) {
      vi.mocked(playhead).mockReturnValue(at);
      act(() => frames.run());
    }
    expect(model.buildPathView).toHaveBeenCalledTimes(1);
    vi.mocked(playhead).mockReturnValue(2.5);
    act(() => frames.run());
    act(() => frames.run());
    expect(model.buildPathView).toHaveBeenCalledTimes(2);
  });

  it('rebuilds for a new epoch, a new sounding version and blind', () => {
    const frames = controlFrames();
    recorded();
    render(<PathLine />);
    const take = getState().take as NonNullable<ReturnType<typeof getState>['take']>;
    act(() => setState({ take: { ...take, epoch: 2 } }));
    act(() => frames.run());
    expect(model.buildPathView).toHaveBeenCalledTimes(2);
    act(() => setState({ versionIndex: 1 }));
    act(() => frames.run());
    expect(model.buildPathView).toHaveBeenCalledTimes(3);
    act(() => setState({ blind: true, blindOrder: [0, 1] }));
    act(() => frames.run());
    expect(model.buildPathView).toHaveBeenCalledTimes(4);
  });

  it('rebuilds in the new language', () => {
    controlFrames();
    recorded();
    const { container } = render(<PathLine />);
    expect(texts(container)[0]).toBe('path');
    act(() => setLang('ja'));
    expect(texts(container)[0]).toBe('経路');
    expect(texts(container)[1]).toBe('音源 → アンプ → センド');
  });

  it('empties when the take goes', () => {
    const frames = controlFrames();
    recorded();
    const { container } = render(<PathLine />);
    act(() => setState({ take: null }));
    act(() => frames.run());
    expect(line(container).children).toHaveLength(0);
  });

  it('stops its loop when unmounted', () => {
    const frames = controlFrames();
    const { unmount } = render(<PathLine />);
    unmount();
    expect(frames.pending()).toBe(0);
  });
});
