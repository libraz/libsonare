import { describe, expect, it } from 'vitest';
import { ClipPageStreamer } from '../dist/index.js';

interface ClipPageRequest {
  clipId: number;
  channel: number;
  sample: number;
}

/** Engine stand-in that replays a fixed queue of page-miss requests. */
function fakeEngine(requests: ClipPageRequest[]) {
  const queue = [...requests];
  return {
    popClipPageRequest: () => queue.shift() ?? null,
    enqueue: (...more: ClipPageRequest[]) => queue.push(...more),
  };
}

/** Binding stand-in recording every supply / clear / close. */
function fakeBinding(options: { failPages?: Set<number> } = {}) {
  const supplied: number[] = [];
  const cleared: number[] = [];
  let closed = false;
  const binding = {
    supplied,
    cleared,
    get closed() {
      return closed;
    },
    provider: {
      clear: (pageIndex: number) => {
        cleared.push(pageIndex);
      },
    },
    supplyPage: (pageIndex: number) => {
      supplied.push(pageIndex);
      return Promise.resolve(!options.failPages?.has(pageIndex));
    },
    supplyRequest: () => Promise.resolve(true),
    close: () => {
      closed = true;
    },
  };
  return binding;
}

describe('ClipPageStreamer', () => {
  it('fetches the missed page plus its read-ahead window', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 12 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 2, retainBehindPages: 1 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();

    // sample 12 / pageFrames 4 = page 3; window [3-1, 3+2] = [2,5].
    expect([...binding.supplied].sort((a, b) => a - b)).toEqual([2, 3, 4, 5]);
    expect(binding.cleared).toEqual([]);
  });

  it('evicts pages that fall outside the window as the frontier advances', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 0 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 1, retainBehindPages: 1 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump(); // page 0 -> window [0,1]
    expect([...binding.supplied].sort((a, b) => a - b)).toEqual([0, 1]);

    engine.enqueue({ clipId: 1, channel: 0, sample: 40 }); // page 10 -> window [9,11]
    await streamer.pump();

    // Pages 0 and 1 are now far behind the frontier and must be evicted, so the
    // resident set stays bounded to the window size regardless of how far
    // playback has advanced.
    expect([...binding.cleared].sort((a, b) => a - b)).toEqual([0, 1]);
    expect([...binding.supplied].sort((a, b) => a - b)).toEqual([0, 1, 9, 10, 11]);
    const resident = new Set(binding.supplied);
    for (const page of binding.cleared) {
      resident.delete(page);
    }
    expect(resident.size).toBeLessThanOrEqual(1 + 1 + 1); // behind + frontier + ahead
  });

  it('collapses a forward run of misses across channels to the latest frontier', async () => {
    const engine = fakeEngine([
      { clipId: 1, channel: 0, sample: 4 },
      { clipId: 1, channel: 1, sample: 4 },
      { clipId: 1, channel: 0, sample: 8 },
    ]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 0, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();

    // Furthest sample 8 -> page 2; zero-width window supplies only page 2 once.
    expect(binding.supplied).toEqual([2]);
  });

  it('services a backward seek that follows a stale high-page miss in the same pump', async () => {
    const engine = fakeEngine([
      { clipId: 1, channel: 0, sample: 400 }, // stale page 100
      { clipId: 1, channel: 0, sample: 0 }, // seek/loop page 0 is newer
    ]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 0, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();

    expect(binding.supplied).toEqual([0]);
  });

  it('advances generation and resets resident pages on a backward seek', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 400 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 0, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();
    engine.enqueue({ clipId: 1, channel: 0, sample: 0 });
    await streamer.pump();

    expect(binding.supplied).toEqual([100, 0]);
    expect(binding.cleared).toContain(100);
  });

  it('explicit reset refetches pages in a new playback generation', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 0 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 0, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();
    streamer.resetSource(1);
    engine.enqueue({ clipId: 1, channel: 0, sample: 0 });
    await streamer.pump();

    expect(binding.supplied).toEqual([0, 0]);
    expect(binding.cleared).toContain(0);
  });

  it('clears a stale fetch that settles after a generation reset', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 40 }]);
    const supplied: number[] = [];
    const cleared: number[] = [];
    let resolveFetch: ((ok: boolean) => void) | undefined;
    const binding = {
      provider: { clear: (page: number) => cleared.push(page) },
      supplyPage: (page: number) => {
        supplied.push(page);
        return new Promise<boolean>((resolve) => {
          resolveFetch = resolve;
        });
      },
      close: () => undefined,
    };
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 0, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    const pumping = streamer.pump();
    await Promise.resolve();
    streamer.resetSource(1);
    expect(resolveFetch).toBeDefined();
    resolveFetch?.(true);
    await pumping;

    expect(supplied).toEqual([10]);
    // Once during reset for the eager resident marker and once after the stale
    // supply settles, in case it installed the page after that first clear.
    expect(cleared.filter((page) => page === 10)).toHaveLength(2);
  });

  it('honors pages already resident before registration', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 0 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 1, retainBehindPages: 0 });
    streamer.addSource(
      { clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 },
      [0],
    );

    await streamer.pump();

    // Page 0 was primed, so only the read-ahead page 1 is fetched.
    expect(binding.supplied).toEqual([1]);
  });

  it('drops a page from the resident set when its fetch reports a miss', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 0 }]);
    const binding = fakeBinding({ failPages: new Set([1]) });
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 1, retainBehindPages: 0 });
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();
    expect([...binding.supplied].sort((a, b) => a - b)).toEqual([0, 1]);

    // Page 1 failed, so a later miss at the same position retries it.
    engine.enqueue({ clipId: 1, channel: 0, sample: 0 });
    await streamer.pump();
    expect(binding.supplied.filter((p) => p === 1)).toHaveLength(2);
  });

  it('clamps the window to the final page of the clip', async () => {
    const engine = fakeEngine([{ clipId: 1, channel: 0, sample: 20 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine, { readAheadPages: 5, retainBehindPages: 0 });
    // 24 samples / 4 = 6 pages (0..5).
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 24 });

    await streamer.pump();

    // sample 20 -> page 5 (the last); read-ahead must not exceed it.
    expect(binding.supplied).toEqual([5]);
  });

  it('ignores requests for unregistered clips', async () => {
    const engine = fakeEngine([{ clipId: 99, channel: 0, sample: 0 }]);
    const binding = fakeBinding();
    const streamer = new ClipPageStreamer(engine);
    streamer.addSource({ clipId: 1, binding: binding as never, pageFrames: 4, numSamples: 4000 });

    await streamer.pump();
    expect(binding.supplied).toEqual([]);
  });

  it('closes every registered binding on close', () => {
    const engine = fakeEngine([]);
    const a = fakeBinding();
    const b = fakeBinding();
    const streamer = new ClipPageStreamer(engine);
    streamer.addSource({ clipId: 1, binding: a as never, pageFrames: 4, numSamples: 400 });
    streamer.addSource({ clipId: 2, binding: b as never, pageFrames: 4, numSamples: 400 });

    streamer.close();
    expect(a.closed).toBe(true);
    expect(b.closed).toBe(true);
  });
});
