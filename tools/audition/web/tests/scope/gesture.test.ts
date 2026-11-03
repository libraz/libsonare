import { beforeEach, describe, expect, it, vi } from 'vitest';
import { pause, playhead, seekTo, setRegion, startAt } from '../../src/player/engine';
import { GRAB_PX, MIN_DRAG } from '../../src/scope/constants';
import { Gesture } from '../../src/scope/gesture';
import { getState, setState } from '../../src/state/store';
import { loadTake } from './fakes';

vi.mock('../../src/player/engine');

// A 100 px wide canvas over a 10 s take: one pixel is 0.1 s.
const W = 100;
let g: Gesture;

beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(playhead).mockReturnValue(5);
  loadTake();
  g = new Gesture();
});

describe('click', () => {
  it('seeks', () => {
    g.down(2, W);
    g.up(2);
    expect(seekTo).toHaveBeenCalledWith(2);
    expect(setRegion).not.toHaveBeenCalled();
  });

  it('treats a shaky hand as a click', () => {
    g.down(2, W);
    g.move(2 + MIN_DRAG / 2, W);
    g.up(2.0125);
    expect(seekTo).toHaveBeenCalledWith(2.0125);
    expect(getState().region).toBeNull();
  });

  it('leaves a marked passage when the click is outside it', () => {
    setState({ region: [3, 4] });
    g.down(7, W);
    g.up(7);
    expect(setRegion).toHaveBeenCalledWith(null);
    expect(seekTo).toHaveBeenCalledWith(7);
  });

  it('keeps the passage for a click inside it, edges included', () => {
    setState({ region: [3, 4] });
    for (const at of [3, 3.5, 4]) {
      g.down(at, W);
      g.up(at);
    }
    expect(setRegion).not.toHaveBeenCalled();
    expect(vi.mocked(seekTo).mock.calls.map((c) => c[0])).toEqual([3, 3.5, 4]);
  });
});

describe('drag', () => {
  it('marks a passage and hands it to the engine on release', () => {
    g.down(2, W);
    g.move(3, W);
    expect(getState().region).toEqual([2, 3]);
    expect(setRegion).not.toHaveBeenCalled();
    g.move(4, W);
    g.up(4);
    expect(getState().region).toEqual([2, 4]);
    expect(setRegion).toHaveBeenCalledTimes(1);
    expect(setRegion).toHaveBeenCalledWith([2, 4]);
    expect(seekTo).not.toHaveBeenCalled();
  });

  it('orders a leftward drag', () => {
    g.down(8, W);
    g.move(6, W);
    g.up(6);
    expect(setRegion).toHaveBeenCalledWith([6, 8]);
  });

  it('does not mark until the drag clears the threshold', () => {
    g.down(2, W);
    g.move(2.015, W);
    expect(getState().region).toBeNull();
    g.move(2.03, W);
    expect(getState().region).not.toBeNull();
  });

  it('ends the gesture on release', () => {
    g.down(2, W);
    g.up(2);
    g.up(9);
    g.move(9, W);
    expect(seekTo).toHaveBeenCalledTimes(1);
    expect(getState().region).toBeNull();
  });
});

describe('grab', () => {
  const reach = (GRAB_PX / W) * 10 - 0.001;

  it('catches the playhead within the grab distance only', () => {
    expect(g.near(5 + reach, W)).toBe(true);
    expect(g.near(5 - reach, W)).toBe(true);
    expect(g.near(5 + reach + 0.01, W)).toBe(false);
  });

  it('scales the grab distance with the canvas width', () => {
    expect(g.near(5.5, 1000)).toBe(false);
    expect(g.near(5.05, 1000)).toBe(true);
  });

  it('scrubs silently and resumes where it was left when playing', () => {
    setState({ playing: true });
    g.down(5.5, W);
    expect(pause).toHaveBeenCalledTimes(1);
    expect(seekTo).toHaveBeenLastCalledWith(5.5);
    g.move(7, W);
    expect(seekTo).toHaveBeenLastCalledWith(7);
    vi.mocked(playhead).mockReturnValue(8);
    g.up(8);
    expect(seekTo).toHaveBeenLastCalledWith(8);
    expect(startAt).toHaveBeenCalledWith(8);
    expect(getState().region).toBeNull();
    expect(setRegion).not.toHaveBeenCalled();
  });

  it('stays paused when it was paused', () => {
    g.down(5.2, W);
    g.up(6);
    expect(pause).not.toHaveBeenCalled();
    expect(startAt).not.toHaveBeenCalled();
    expect(seekTo).toHaveBeenLastCalledWith(6);
  });

  it('never marks a passage while scrubbing', () => {
    g.down(5.2, W);
    g.move(9, W);
    expect(getState().region).toBeNull();
  });
});

describe('cursor', () => {
  it('shows a resize cursor near the playhead and nothing elsewhere', () => {
    expect(g.move(5.3, W)).toBe('ew-resize');
    expect(g.move(1, W)).toBe('');
  });

  it('keeps the resize cursor while scrubbing', () => {
    g.down(5.1, W);
    expect(g.move(1, W)).toBe('ew-resize');
  });

  it('shows nothing without a take', () => {
    loadTake({ take: null });
    expect(g.move(5, W)).toBe('');
  });
});

describe('without a take', () => {
  it('does nothing', () => {
    loadTake({ take: null });
    g.down(2, W);
    g.up(2);
    expect(seekTo).not.toHaveBeenCalled();
  });
});
