import { beforeEach, describe, expect, it } from 'vitest';
import {
  applyGains,
  atEnd,
  bufferRms,
  commitBuffers,
  MIN_SPAN,
  pause,
  playhead,
  rewind,
  seekTo,
  setContextFactory,
  setLoop,
  setRegion,
  span,
  startAt,
  stop,
  targetGain,
  togglePlay,
  wavRate,
} from '../../src/player/engine';
import { getState, setState } from '../../src/state/store';
import { FakeContext, fakeBuffer, wav } from '../audio';
import { opened } from '../fixtures';

let ctx: FakeContext;

beforeEach(() => {
  ctx = new FakeContext();
  setContextFactory(() => ctx as unknown as AudioContext);
  const s = opened();
  commitBuffers(
    Object.fromEntries(
      (s.take?.keys ?? []).map((k) => [k, fakeBuffer(4) as unknown as AudioBuffer]),
    ),
  );
});

describe('reading a file', () => {
  it('reads the rate a WAVE file was written at, and 0 for anything else', () => {
    expect(wavRate(wav(44100))).toBe(44100);
    expect(wavRate(wav(96000))).toBe(96000);
    expect(wavRate(new ArrayBuffer(8))).toBe(0);
    expect(wavRate(new TextEncoder().encode('RIFFxxxxAVI LIST').buffer)).toBe(0);
  });

  it('measures the level of a buffer', () => {
    expect(bufferRms(fakeBuffer(1, 1000, 0.5) as unknown as AudioBuffer)).toBeCloseTo(0.5, 6);
    expect(bufferRms(fakeBuffer(0, 1000, 0.5) as unknown as AudioBuffer)).toBe(0);
  });
});

describe('the passage', () => {
  it('is the whole take until a region is marked, and ignores a region too short to be one', () => {
    expect(span()).toEqual([0, 4]);
    setState({ region: [1, 2] });
    expect(span()).toEqual([1, 2]);
    setState({ region: [1, 1 + MIN_SPAN / 2] });
    expect(span()).toEqual([0, 4]);
  });
});

describe('playing', () => {
  it('starts every version at once with only the sounding one audible', () => {
    startAt(1);
    expect(ctx.live()).toHaveLength(getState().take?.keys.length ?? -1);
    expect(ctx.live().every((s) => s.started === 1)).toBe(true);
    const audible = ctx.gains.slice(1).filter((g) => g.gain.value > 0);
    expect(audible).toHaveLength(1);
    expect(getState().playing).toBe(true);
    expect(ctx.resumed).toBe(1);
  });

  it('clamps the start into the passage', () => {
    setState({ region: [1, 2] });
    startAt(0.2);
    expect(ctx.live()[0]?.started).toBe(1);
    startAt(3);
    expect(ctx.live()[0]?.started).toBeCloseTo(1.999, 6);
  });

  it('follows the audio clock, and wraps inside the passage under loop', () => {
    startAt(0);
    ctx.currentTime = 1.25;
    expect(playhead()).toBeCloseTo(1.25);
    setState({ region: [1, 2] });
    setLoop(true);
    // Re-anchored at 1.25 s: 0.75 s later it reaches the passage end and wraps to its top.
    ctx.currentTime = 2.0;
    expect(playhead()).toBeCloseTo(1.0);
    ctx.currentTime = 2.5;
    expect(playhead()).toBeCloseTo(1.5);
    expect(ctx.live().every((s) => s.loop)).toBe(true);
  });

  it('stops at the end of a pass and goes back to its top, never under loop', () => {
    setState({ region: [1, 2] });
    startAt(1);
    ctx.currentTime = 0.999;
    expect(atEnd()).toBe(true);
    stop();
    expect(getState().playing).toBe(false);
    expect(playhead()).toBe(1);
    setLoop(true);
    startAt(1);
    ctx.currentTime = 5;
    expect(atEnd()).toBe(false);
  });

  it('pauses where it is and resumes from there', () => {
    startAt(0);
    ctx.currentTime = 0.75;
    pause();
    expect(playhead()).toBeCloseTo(0.75);
    togglePlay();
    expect(ctx.live()[0]?.started).toBeCloseTo(0.75);
  });

  it('plays from the top when parked at the far end', () => {
    seekTo(4);
    togglePlay();
    expect(ctx.live()[0]?.started).toBe(0);
  });

  it('seeks while playing by restarting, and while paused by parking', () => {
    seekTo(2);
    expect(ctx.live()).toHaveLength(0);
    expect(playhead()).toBe(2);
    startAt(0);
    seekTo(3);
    expect(ctx.live()[0]?.started).toBe(3);
    rewind();
    expect(ctx.live()[0]?.started).toBe(0);
  });
});

describe('a region marked while playing', () => {
  it('keeps the playhead when it is inside the new passage', () => {
    startAt(1.5);
    const before = ctx.sources.length;
    setRegion([1, 3]);
    expect(ctx.sources.length).toBe(before);
    expect(ctx.live().every((s) => s.loopStart === 1 && s.loopEnd === 3)).toBe(true);
  });

  it('starts the passage over when the playhead is outside it', () => {
    startAt(0.2);
    setRegion([2, 3]);
    expect(ctx.live()[0]?.started).toBe(2);
  });

  it('parks a paused playhead at the top of a passage it is outside', () => {
    seekTo(0.2);
    setRegion([2, 3]);
    expect(playhead()).toBe(2);
    setRegion(null);
    expect(getState().region).toBeNull();
  });
});

describe('levels', () => {
  it('match every version to the mean level when asked, capped at ×8', () => {
    expect(targetGain('model')).toBe(1);
    setState({ matchRms: true });
    const take = getState().take;
    if (!take) {
      throw new Error('no take');
    }
    const mean = take.keys.reduce((a, k) => a + (take.rms[k] ?? 0), 0) / take.keys.length;
    expect(targetGain('model')).toBeCloseTo(mean / (take.rms.model ?? 1));
    setState({ take: { ...take, rms: { ...take.rms, model: 1e-6 } } });
    expect(targetGain('model')).toBe(8);
  });

  it('ramp rather than jump on a switch while playing', () => {
    startAt(0);
    setState({ versionIndex: 1 });
    applyGains(false);
    const ramps = ctx.gains.flatMap((g) => g.gain.events.filter((e) => e[0] === 'ramp'));
    expect(ramps.length).toBeGreaterThan(0);
  });
});
