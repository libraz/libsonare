import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  it('rejects clips without channels or a page provider without aborting', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ id: 1, startPpq: 0 }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    expect(() => engine.setClips([{ id: 2, startPpq: 0, channels: [] }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    engine.destroy();
  });

  it('refuses a fractional engine sample rate', () => {
    expect(() => new RealtimeEngine(44100.5, 128)).toThrow(RangeError);
    const engine = new RealtimeEngine(44100, 128);
    expect(() => engine.prepare(48000.25, 128)).toThrow(/whole number of hertz/);
    engine.destroy();
  });

  it('upserts and removes one clip while keeping the rest', () => {
    const engine = new RealtimeEngine(48000, 4);
    const plane = (value: number) => new Float32Array(4).fill(value);
    engine.setClips([
      { id: 1, channels: [plane(0.25)], startPpq: 0 },
      { id: 2, channels: [plane(0.5)], startPpq: 0 },
    ]);
    const firstSample = () => {
      engine.seekSample(0);
      engine.play();
      return engine.process([new Float32Array(4)])[0][0];
    };
    expect(firstSample()).toBeCloseTo(0.75);

    engine.upsertClip({ id: 1, channels: [plane(0.25)], startPpq: 0, gain: 2 });
    expect(engine.clipCount()).toBe(2);
    expect(firstSample()).toBeCloseTo(1.0);

    engine.removeClip(2);
    expect(engine.clipCount()).toBe(1);
    expect(firstSample()).toBeCloseTo(0.5);
    expect(() => engine.removeClip(2)).toThrow();
    expect(() =>
      engine.upsertClip({ id: 1, channels: [plane(0.25)], startPpq: 0, gain: -1 }),
    ).toThrow();
    expect(firstSample()).toBeCloseTo(0.5);
    engine.destroy();
  });

  it('streams paged clip providers and drains page requests', () => {
    const engine = new RealtimeEngine(48000, 8);
    const provider = engine.createClipPageProvider(1, 8, 4);
    provider.supply(0, [new Float32Array([1, 2, 3, 4])]);

    engine.setClips([
      {
        id: 123,
        pageProvider: provider,
        startPpq: 0,
      },
    ]);
    engine.play();
    const first = engine.process([new Float32Array(8)]);
    expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

    const request = engine.popClipPageRequest();
    expect(request).toEqual({ clipId: 123, channel: 0, sample: 4 });
    expect(
      engine.drainTelemetry().some((record) => record.type === 1 && record.value === 123),
    ).toBe(true);

    provider.supply(1, [new Float32Array([5, 6, 7, 8])]);
    engine.seekSample(0);
    const second = engine.process([new Float32Array(8)]);
    expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

    provider.destroy();
    engine.destroy();
  });

  it('rejects paged providers whose metadata exceeds shared limits', () => {
    const engine = new RealtimeEngine(48000, 8);
    expect(() => engine.createClipPageProvider(1, 1_000_000_000_000, 1)).toThrow();
    expect(() => engine.createClipPageProvider(65, 8, 4)).toThrow();
    engine.destroy();
  });

  it('feeds paged clips from raw float32 files', () => {
    const tmpDir = mkdtempSync(join(tmpdir(), 'sonare-paged-'));
    const rawPath = join(tmpDir, 'clip.f32');
    try {
      const interleaved = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
      writeFileSync(rawPath, Buffer.from(interleaved.buffer));

      const engine = new RealtimeEngine(48000, 8);
      const provider = engine.createFileClipPageProvider(rawPath, {
        numChannels: 1,
        numSamples: 8,
        pageFrames: 4,
      });
      provider.supplyPage(0);
      engine.setClips([{ id: 124, pageProvider: provider, startPpq: 0 }]);
      engine.play();
      const first = engine.process([new Float32Array(8)]);
      expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

      const request = engine.popClipPageRequest();
      expect(request).toEqual({ clipId: 124, channel: 0, sample: 4 });
      expect(request && provider.supplyRequest(request)).toBe(true);
      engine.seekSample(0);
      const second = engine.process([new Float32Array(8)]);
      expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

      provider.destroy();
      engine.destroy();
    } finally {
      rmSync(tmpDir, { recursive: true, force: true });
    }
  });

  it('never reuses a clip page provider id after it is destroyed', () => {
    // ids used to be freed-slot indices, so a destroyed provider's id was
    // handed straight back to the next createClipPageProvider call -- a
    // clip bound to the stale id then silently played whichever provider now
    // held that slot instead of being refused. Ids are read relatively here
    // (rather than pinned to literal numbers) because a failed
    // createFileClipPageProvider attempt above also consumes and immediately
    // frees one, and this test must not depend on how many ids came before it.
    const engine = new RealtimeEngine(48000, 8);
    expect(() =>
      engine.createFileClipPageProvider('/definitely/missing/sonare-clip.f32', {
        numChannels: 1,
        numSamples: 8,
        pageFrames: 4,
      }),
    ).toThrow();
    const afterFailedOpen = engine.createClipPageProvider(1, 8, 4);
    afterFailedOpen.destroy();

    const first = engine.createClipPageProvider(1, 8, 4);
    const second = engine.createClipPageProvider(1, 8, 4);
    expect(second.id).toBe(first.id + 1);
    first.destroy();
    const third = engine.createClipPageProvider(1, 8, 4);
    // The control: ids keep climbing rather than backfilling first's freed id.
    expect(third.id).toBe(second.id + 1);
    expect(third.id).not.toBe(first.id);
    second.destroy();
    third.destroy();
    engine.destroy();
  });

  it('refuses a clip bound to a destroyed provider instead of silently playing whatever now holds its old id', () => {
    const engine = new RealtimeEngine(48000, 8);
    const stale = engine.createClipPageProvider(1, 8, 4);
    stale.destroy();
    // Before the fix this reused stale's numeric id, so the assertion below
    // would have silently bound clip 1 to replacement's audio instead of
    // throwing -- the control is that replacement gets a DIFFERENT id.
    const replacement = engine.createClipPageProvider(1, 8, 4);
    expect(replacement.id).not.toBe(stale.id);
    replacement.supply(0, [new Float32Array([9, 9, 9, 9])]);

    expect(() => engine.setClips([{ id: 1, pageProvider: stale, startPpq: 0 }])).toThrow(
      /pageProvider is not a live ClipPageProvider/,
    );

    replacement.destroy();
    engine.destroy();
  });

  it('renders repitch warped clips from anchors', () => {
    const engine = new RealtimeEngine(48000, 4);
    engine.setClips([
      {
        id: 303,
        channels: [new Float32Array([0, 10, 20, 30])],
        startPpq: 0,
        lengthSamples: 4,
        warpMode: 'repitch',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 3, sourceSample: 1.5 },
        ],
      },
    ]);
    engine.play();

    const processed = engine.process([new Float32Array(4)]);
    expect(processed[0][0]).toBeCloseTo(0, 4);
    expect(processed[0][1]).toBeCloseTo(5, 4);
    expect(processed[0][2]).toBeCloseTo(10, 4);
    expect(processed[0][3]).toBeCloseTo(15, 4);
    engine.destroy();

    const badLoopWarpEngine = new RealtimeEngine(48000, 4);
    expect(() =>
      badLoopWarpEngine.setClips([
        {
          id: 3031,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 8,
          loop: true,
          warpMode: 'repitch',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 3, sourceSample: 1.5 },
          ],
        },
      ]),
    ).toThrow();
    badLoopWarpEngine.destroy();

    const badWarpModeEngine = new RealtimeEngine(48000, 4);
    badWarpModeEngine.setClips([
      {
        id: 3032,
        channels: [new Float32Array([0.25, 0.5, 0.75, 1.0])],
        startPpq: 0,
        lengthSamples: 4,
      },
    ]);
    badWarpModeEngine.play();
    expect(() =>
      badWarpModeEngine.setClips([
        {
          id: 3033,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 4,
        },
        {
          id: 3034,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 4,
          warpMode: 'typo' as 'repitch',
        },
      ]),
    ).toThrow(/warpMode must be/);
    expect(badWarpModeEngine.clipCount()).toBe(1);
    badWarpModeEngine.seekSample(0);
    const afterBadWarpMode = badWarpModeEngine.process([new Float32Array(4)]);
    expect(Array.from(afterBadWarpMode[0])).toEqual([0.25, 0.5, 0.75, 1.0]);
    badWarpModeEngine.destroy();

    const tempoEngine = new RealtimeEngine(48000, 8192);
    const tempoSource = new Float32Array(4096);
    for (let i = 0; i < tempoSource.length; i++) {
      tempoSource[i] = Math.sin(i * 0.02);
    }
    tempoEngine.setClips([
      {
        id: 304,
        channels: [tempoSource],
        startPpq: 0,
        lengthSamples: 8192,
        warpMode: 'tempo-sync',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 2048, sourceSample: 1024 },
          { warpSample: 8192, sourceSample: 4096 },
        ],
      },
    ]);
    tempoEngine.play();
    const tempoSynced = tempoEngine.process([new Float32Array(8192)]);
    expect(tempoSynced[0].some((v) => Math.abs(v) > 0.1)).toBe(true);
    tempoEngine.destroy();
  });

  it('renders time-stretch warped clips at the source pitch', () => {
    const sr = 48000;
    const sourceSamples = 12000;
    const outputSamples = 24000;
    const source = new Float32Array(sourceSamples);
    for (let i = 0; i < sourceSamples; i++) {
      source[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sr);
    }
    const engine = new RealtimeEngine(sr, outputSamples);
    engine.setClips([
      {
        id: 305,
        channels: [source],
        startPpq: 0,
        lengthSamples: outputSamples,
        warpMode: 'time-stretch',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: outputSamples, sourceSample: sourceSamples },
        ],
      },
    ]);
    engine.play();
    const out = engine.process([new Float32Array(outputSamples)])[0];

    // Goertzel: a resampling warp at half rate would move the tone to 220 Hz.
    const power = (hz: number): number => {
      const w = (2 * Math.PI * hz) / sr;
      const coeff = 2 * Math.cos(w);
      let s1 = 0;
      let s2 = 0;
      for (let i = 4096; i < outputSamples - 4096; i++) {
        const s0 = out[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const real = s1 - s2 * Math.cos(w);
      const imag = s2 * Math.sin(w);
      return real * real + imag * imag;
    };
    expect(power(440)).toBeGreaterThan(100 * power(220));
    engine.destroy();
  });

  it('reports the clip-page and warp-stretch overflow counters', () => {
    // Both mirror sonare_engine_external_midi_dropped_count, which this file
    // already covers. A fresh engine has dropped nothing, so the assertion is
    // that the counters EXIST and read zero -- the addon method missing
    // entirely is a TypeError, which is what this catches.
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(engine.clipPageRequestOverflowCount()).toBe(0);
      expect(engine.warpStretchOverflowCount()).toBe(0);
      // Positive control on the reader itself: the sibling counter that has
      // always existed reads the same way, so a zero here is the counter
      // answering rather than a stub returning a default.
      expect(engine.externalMidiDroppedCount()).toBe(0);
      expect(typeof engine.clipPageRequestOverflowCount()).toBe('number');
      expect(typeof engine.warpStretchOverflowCount()).toBe('number');
    } finally {
      engine.destroy();
    }
  });

  it('sets and reads the warp voice capacity, rejecting out-of-domain values', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(engine.warpVoiceCapacity()).toBe(8);

      engine.setWarpVoiceCapacity(12);
      expect(engine.warpVoiceCapacity()).toBe(12);

      engine.setWarpVoiceCapacity(0);
      expect(engine.warpVoiceCapacity()).toBe(0);

      // Rejected values leave the previously accepted capacity (0) in place.
      expect(() => engine.setWarpVoiceCapacity(65)).toThrow();
      expect(engine.warpVoiceCapacity()).toBe(0);

      expect(() => engine.setWarpVoiceCapacity(-1)).toThrow();
      expect(engine.warpVoiceCapacity()).toBe(0);

      expect(() => engine.setWarpVoiceCapacity(1.5)).toThrow();
      expect(engine.warpVoiceCapacity()).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('destroy() releases the clip page providers it still owns', () => {
    const pageFrames = 1 << 20;
    const pages = 8;
    const supplied = 2 * pages * pageFrames * 4;
    const page = [new Float32Array(pageFrames), new Float32Array(pageFrames)];

    /** Loads `supplied` bytes of clip pages into a fresh engine. */
    const loadPages = (target: RealtimeEngine): number => {
      const provider = target.createClipPageProvider(2, pageFrames * pages, pageFrames);
      for (let index = 0; index < pages; index++) {
        target.supplyClipPage(provider.id, index, page);
      }
      return provider.id;
    };

    const engine = new RealtimeEngine(48000, 128);
    const second = engine.createClipPageProvider(1, pageFrames, pageFrames);
    const third = engine.createClipPageProvider(1, pageFrames, pageFrames);
    // Explicitly destroying one leaves a nulled slot: the release path has to
    // walk past it rather than free it a second time.
    second.destroy();

    const baselineRss = process.memoryUsage().rss;
    const first = { id: loadPages(engine) };
    const loadedDelta = process.memoryUsage().rss - baselineRss;
    // Positive control: the pages have to be resident for the reuse check below
    // to mean anything.
    expect(loadedDelta).toBeGreaterThan(supplied / 2);

    // No destroyClipPageProvider(first.id) here: destroy() alone must reach it.
    engine.destroy();
    // RSS is measured by reuse rather than by a drop, because the allocator is
    // free to hold freed blocks: loading the same pages again must land inside
    // the memory the first load released rather than stacking a second copy on
    // top of it (which is what the leak measured, at nearly twice this bound).
    const reload = new RealtimeEngine(48000, 128);
    loadPages(reload);
    expect(process.memoryUsage().rss - baselineRss).toBeLessThan(loadedDelta + supplied / 2);
    reload.destroy();

    // The provider table is empty, so neither id resolves any more.
    expect(() => engine.supplyClipPage(first.id, 0, page)).toThrow(TypeError);
    expect(() => engine.supplyClipPage(third.id, 0, page)).toThrow(TypeError);
    // Idempotent: the facade's destroy() is guarded by its own disposed flag, so
    // reach past it to drive the native release a second time directly.
    const native = (engine as unknown as { native: { destroy(): void } }).native;
    expect(() => native.destroy()).not.toThrow();
    expect(() => engine.destroy()).not.toThrow();
    // A provider handed out before the sweep and released after it must not free
    // an already-freed provider either.
    expect(() => third.destroy()).not.toThrow();
    // The process is still usable, not merely free of an escaping exception.
    const after = new RealtimeEngine(48000, 128);
    expect(after.getTransportState().playing).toBe(false);
    after.destroy();
  });
});
