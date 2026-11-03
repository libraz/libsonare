import { act, fireEvent, render } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { setLang } from '../../src/i18n';
import { bufferOf, playhead, seekTo, setRegion } from '../../src/player/engine';
import { clearScopeCache } from '../../src/scope/compute';
import { Scopes } from '../../src/scope/Scopes';
import { getState, setState } from '../../src/state/store';
import {
  controlFrames,
  loadTake,
  pcm,
  setRoleColours,
  sine,
  sizeCanvas,
  stubCanvas,
} from './fakes';

vi.mock('../../src/player/engine');

let recorder: ReturnType<typeof stubCanvas>;

beforeEach(() => {
  clearScopeCache();
  vi.clearAllMocks();
  recorder = stubCanvas();
  setRoleColours();
  vi.mocked(playhead).mockReturnValue(0);
  vi.mocked(bufferOf).mockImplementation(() => pcm([sine(80, 1)]) as unknown as AudioBuffer);
});

const canvases = (c: HTMLElement) => ({
  wave: c.querySelector('#wave') as HTMLCanvasElement,
  spec: c.querySelector('#spec') as HTMLCanvasElement,
});
const legend = (c: HTMLElement) =>
  [...c.querySelectorAll('#waveLegend .wl')].map((e) => ({
    cls: e.className,
    text: e.textContent,
  }));

describe('markup', () => {
  it('keeps the ids, heights and classes the stylesheet addresses', () => {
    const { container } = render(<Scopes />);
    const { wave, spec } = canvases(container);
    expect(container.firstElementChild?.className).toBe('scopes');
    expect(container.querySelectorAll('figure.pane')).toHaveLength(2);
    expect(wave.getAttribute('height')).toBe('142');
    expect(spec.getAttribute('height')).toBe('212');
    expect(container.querySelector('#waveCaption')).not.toBeNull();
    expect(container.querySelector('#waveLegend.legend-row')).not.toBeNull();
    expect(container.querySelector('figcaption#specCaption.cap')).not.toBeNull();
  });
});

describe('captions', () => {
  it('describes the comparison, or just the waveform when there is none', () => {
    loadTake();
    const { container } = render(<Scopes />);
    const cap = () => container.querySelector('#waveCaption')?.textContent;
    expect(cap()).toBe('waveform — every version drawn in its own colour, the one sounding solid');
    act(() => setState({ compare: false }));
    expect(cap()).toBe('waveform');
  });

  it('follows the language', () => {
    const { container } = render(<Scopes />);
    expect(container.querySelector('#specCaption')?.textContent).toBe(
      'spectrogram of the version sounding — log frequency',
    );
    act(() => setLang('ja'));
    expect(container.querySelector('#specCaption')?.textContent).toBe(
      '鳴っているバージョンのスペクトログラム（周波数は対数）',
    );
    expect(container.querySelector('#waveCaption')?.textContent).toBe(
      '波形 — バージョンごとに色分け、鳴っているものが濃い線',
    );
  });

  it('says a take is decoding, and why one failed', () => {
    const { container } = render(<Scopes />);
    const cap = () => container.querySelector('#specCaption') as HTMLElement;
    act(() => setState({ takeStatus: { state: 'loading' } }));
    expect(cap().textContent).toBe('decoding…');
    expect(cap().className).toContain('loading');
    expect(cap().className).toContain('cap');
    act(() => setState({ takeStatus: { state: 'failed', msg: 'boom' } }));
    expect(cap().textContent).toBe('could not load: boom');
    expect(cap().className).not.toContain('loading');
    act(() => setLang('ja'));
    expect(cap().textContent).toBe('読み込めませんでした: boom');
    act(() => setState({ takeStatus: null }));
    expect(cap().textContent).toContain('スペクトログラム');
  });
});

describe('legend', () => {
  it('shows each role once, in its colour class', () => {
    loadTake({ display: [0, 1] });
    const { container } = render(<Scopes />);
    expect(legend(container)).toEqual([
      { cls: 'wl role-model', text: 'libsonare' },
      { cls: 'wl role-reference', text: 'reference' },
    ]);
  });

  it('does not repeat a role', () => {
    loadTake({
      display: [0, 1, 2],
      take: { id: 't', keys: ['m', 'm', 'r'], rms: {}, rates: {}, duration: 10, epoch: 1 },
    });
    const { container } = render(<Scopes />);
    expect(legend(container).map((l) => l.text)).toEqual(['libsonare', 'reference']);
  });

  it('follows the order the versions are shown in', () => {
    loadTake({ display: [1, 0] });
    const { container } = render(<Scopes />);
    expect(legend(container).map((l) => l.text)).toEqual(['reference', 'libsonare']);
  });

  it('names a version with no role as versions', () => {
    loadTake({
      display: [0],
      take: { id: 't', keys: ['zz'], rms: {}, rates: {}, duration: 10, epoch: 1 },
    });
    const { container } = render(<Scopes />);
    expect(legend(container)).toEqual([{ cls: 'wl role-other', text: 'versions' }]);
  });

  it('is empty when blind, when solo and without a take', () => {
    const { container } = render(<Scopes />);
    expect(legend(container)).toEqual([]);
    act(() => loadTake({ blind: true, blindOrder: [0, 1] }));
    expect(legend(container)).toEqual([]);
    act(() => loadTake({ compare: false }));
    expect(legend(container)).toEqual([]);
    act(() => loadTake());
    expect(legend(container)).toHaveLength(2);
    act(() => setState({ blind: true, blindOrder: [0, 1] }));
    expect(legend(container)).toEqual([]);
  });

  it('follows the language', () => {
    loadTake();
    const { container } = render(<Scopes />);
    act(() => setLang('ja'));
    expect(legend(container).map((l) => l.text)).toEqual(['libsonare', 'リファレンス']);
  });
});

describe('frame loop', () => {
  it('repaints each frame with the playhead where the engine says', () => {
    loadTake();
    const frames = controlFrames();
    const { container } = render(<Scopes />);
    const { wave } = canvases(container);
    sizeCanvas(wave, 100, 40);
    sizeCanvas(canvases(container).spec, 100, 40);
    const heads = () =>
      recorder(wave)
        .of('lineTo')
        .filter((c) => c.stroke === 'rgba(248,246,242,0.92)' && c.args[1] === 40)
        .map((c) => c.args[0]);
    vi.mocked(playhead).mockReturnValue(2);
    frames.run();
    expect(heads()).toEqual([20]);
    vi.mocked(playhead).mockReturnValue(6);
    frames.run();
    expect(heads()).toEqual([20, 60]);
    expect(frames.pending()).toBe(1);
  });

  it('draws nothing while no take is loaded', () => {
    const frames = controlFrames();
    const { container } = render(<Scopes />);
    sizeCanvas(canvases(container).wave, 100, 40);
    frames.run();
    expect(recorder(canvases(container).wave).calls).toHaveLength(0);
  });

  it('stops when unmounted', () => {
    const frames = controlFrames();
    const { unmount } = render(<Scopes />);
    expect(frames.pending()).toBe(1);
    unmount();
    expect(frames.pending()).toBe(0);
    expect(frames.cancelled()).toBe(1);
  });
});

describe('pointer wiring', () => {
  const setup = () => {
    loadTake();
    const { container } = render(<Scopes />);
    const { wave, spec } = canvases(container);
    sizeCanvas(wave, 100, 40, 10);
    sizeCanvas(spec, 100, 40, 10);
    return { wave, spec };
  };
  const at = (cv: HTMLElement, type: 'pointerDown' | 'pointerMove' | 'pointerUp', x: number) =>
    fireEvent[type](cv, { clientX: 10 + x, pointerId: 1 });

  it('seeks on a click on either picture', () => {
    const { wave, spec } = setup();
    at(wave, 'pointerDown', 20);
    at(wave, 'pointerUp', 20);
    expect(seekTo).toHaveBeenLastCalledWith(2);
    at(spec, 'pointerDown', 70);
    at(spec, 'pointerUp', 70);
    expect(seekTo).toHaveBeenLastCalledWith(7);
  });

  it('marks a passage by dragging', () => {
    const { spec } = setup();
    at(spec, 'pointerDown', 20);
    at(spec, 'pointerMove', 40);
    expect(getState().region).toEqual([2, 4]);
    at(spec, 'pointerUp', 40);
    expect(setRegion).toHaveBeenCalledWith([2, 4]);
  });

  it('grabs the playhead and scrubs', () => {
    vi.mocked(playhead).mockReturnValue(5);
    const { wave } = setup();
    at(wave, 'pointerMove', 52);
    expect(wave.style.cursor).toBe('ew-resize');
    at(wave, 'pointerDown', 52);
    at(wave, 'pointerMove', 80);
    expect(seekTo).toHaveBeenLastCalledWith(8);
    expect(getState().region).toBeNull();
    at(wave, 'pointerMove', 10);
    at(wave, 'pointerUp', 10);
    expect(seekTo).toHaveBeenLastCalledWith(1);
  });

  it('ignores the pointer without a take', () => {
    const { container } = render(<Scopes />);
    const { wave } = canvases(container);
    sizeCanvas(wave, 100, 40);
    fireEvent.pointerDown(wave, { clientX: 20, pointerId: 1 });
    fireEvent.pointerUp(wave, { clientX: 20, pointerId: 1 });
    expect(seekTo).not.toHaveBeenCalled();
  });
});
