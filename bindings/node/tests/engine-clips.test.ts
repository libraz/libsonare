import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { ClipPageProvider, ErrorCode, FileClipPageProvider, RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  it('rejects clips without channels or a page provider without aborting', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClips([{ id: 1, startPpq: 0 }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    expect(() => engine.setClips([{ id: 2, startPpq: 0, channels: [] }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    engine.destroy();
  });

  it('refuses a fractional engine sample rate', () => {
    expect(() => new RealtimeEngine({ sampleRate: 44100.5, maxBlockSize: 128 })).toThrow(
      RangeError,
    );
    const engine = new RealtimeEngine({ sampleRate: 44100, maxBlockSize: 128 });
    expect(() => engine.prepare({ sampleRate: 48000.25, maxBlockSize: 128 })).toThrow(
      /sampleRate must be an integer in \[8000, 384000\]/,
    );
    engine.destroy();
  });

  it('upserts and removes one clip while keeping the rest', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 4 });
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
    expect(engine.clipCount).toBe(2);
    expect(firstSample()).toBeCloseTo(1.0);

    engine.removeClip(2);
    expect(engine.clipCount).toBe(1);
    expect(firstSample()).toBeCloseTo(0.5);
    expect(() => engine.removeClip(2)).toThrow();
    expect(() =>
      engine.upsertClip({ id: 1, channels: [plane(0.25)], startPpq: 0, gain: -1 }),
    ).toThrow();
    expect(firstSample()).toBeCloseTo(0.5);
    engine.destroy();
  });

  it('streams paged clip providers and drains page requests', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 8 });
    const provider = new ClipPageProvider({ numChannels: 1, numSamples: 8, pageFrames: 4 });
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
      engine.drainTelemetry().some((record) => record.type === 'error' && record.value === 123),
    ).toBe(true);

    provider.supply(1, [new Float32Array([5, 6, 7, 8])]);
    engine.seekSample(0);
    const second = engine.process([new Float32Array(8)]);
    expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

    provider.destroy();
    engine.destroy();
  });

  it('rejects paged providers whose metadata exceeds shared limits', () => {
    expect(
      () => new ClipPageProvider({ numChannels: 1, numSamples: 1_000_000_000_000, pageFrames: 1 }),
    ).toThrow();
    expect(() => new ClipPageProvider({ numChannels: 65, numSamples: 8, pageFrames: 4 })).toThrow();
  });

  it('feeds paged clips from raw float32 files', () => {
    const tmpDir = mkdtempSync(join(tmpdir(), 'sonare-paged-'));
    const rawPath = join(tmpDir, 'clip.f32');
    try {
      const interleaved = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
      writeFileSync(rawPath, Buffer.from(interleaved.buffer));

      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 8 });
      const provider = new FileClipPageProvider({
        path: rawPath,
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

  it('keeps a scheduled clip playing after its provider is destroyed', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 4 });
    const provider = new ClipPageProvider({ numChannels: 1, numSamples: 4, pageFrames: 4 });
    provider.supply(0, [new Float32Array([1, 2, 3, 4])]);
    engine.setClips([{ id: 5, pageProvider: provider, startPpq: 0 }]);
    provider.destroy();
    engine.play();
    expect(Array.from(engine.process([new Float32Array(4)])[0])).toEqual([1, 2, 3, 4]);
    engine.destroy();
  });

  describe('ClipPageProvider', () => {
    const dims = { numChannels: 1, numSamples: 8, pageFrames: 4 };

    it('refuses an unknown option key, naming it', () => {
      expect(() => new ClipPageProvider({ ...dims, notAField: 1 } as never)).toThrow(TypeError);
      expect(() => new ClipPageProvider({ ...dims, notAField: 1 } as never)).toThrow(/notAField/);
    });

    it('refuses a positional call', () => {
      expect(
        () => new (ClipPageProvider as unknown as new (...args: number[]) => unknown)(2, 1000, 256),
      ).toThrow(TypeError);
    });

    it('refuses non-positive dimensions with a RangeError', () => {
      expect(() => new ClipPageProvider({ ...dims, numChannels: 0 })).toThrow(RangeError);
      expect(() => new ClipPageProvider({ ...dims, numSamples: 0 })).toThrow(RangeError);
      expect(() => new ClipPageProvider({ ...dims, pageFrames: 0 })).toThrow(RangeError);
    });

    it('refuses supply and clear after destroy, and destroys idempotently', () => {
      const provider = new ClipPageProvider(dims);
      provider.destroy();
      expect(() => provider.destroy()).not.toThrow();
      const invalidState = expect.objectContaining({ code: ErrorCode.InvalidState });
      expect(() => provider.supply(0, [new Float32Array(4)])).toThrow(invalidState);
      expect(() => provider.clear(0)).toThrow(invalidState);
    });

    it('is refused by setClips unless it is a live ClipPageProvider', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 8 });
      try {
        expect(() => engine.setClips([{ id: 1, pageProvider: 3 as never, startPpq: 0 }])).toThrow(
          TypeError,
        );
        expect(() => engine.setClips([{ id: 1, pageProvider: 3 as never, startPpq: 0 }])).toThrow(
          /must be a ClipPageProvider instance/,
        );
        const stale = new ClipPageProvider(dims);
        stale.destroy();
        const refusal = expect.objectContaining({ code: ErrorCode.InvalidState });
        expect(() => engine.setClips([{ id: 1, pageProvider: stale, startPpq: 0 }])).toThrow(
          refusal,
        );
        expect(() => engine.setClips([{ id: 1, pageProvider: stale, startPpq: 0 }])).toThrow(
          /has been destroyed/,
        );
      } finally {
        engine.destroy();
      }
    });
  });

  describe('FileClipPageProvider', () => {
    it('refuses an unknown option key, naming it', () => {
      const options = { path: '/x', numChannels: 1, numSamples: 8, pageFrames: 4, notAField: 1 };
      expect(() => new FileClipPageProvider(options as never)).toThrow(TypeError);
      expect(() => new FileClipPageProvider(options as never)).toThrow(/notAField/);
    });

    it('refuses a missing file without leaking its native provider', () => {
      expect(
        () =>
          new FileClipPageProvider({
            path: '/definitely/missing/sonare-clip.f32',
            numChannels: 1,
            numSamples: 8,
            pageFrames: 4,
          }),
      ).toThrow();
    });
  });

  it('renders repitch warped clips from anchors', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 4 });
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

    const badLoopWarpEngine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 4 });
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

    const badWarpModeEngine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 4 });
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
    expect(badWarpModeEngine.clipCount).toBe(1);
    badWarpModeEngine.seekSample(0);
    const afterBadWarpMode = badWarpModeEngine.process([new Float32Array(4)]);
    expect(Array.from(afterBadWarpMode[0])).toEqual([0.25, 0.5, 0.75, 1.0]);
    badWarpModeEngine.destroy();

    const tempoEngine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 8192 });
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
        warpMode: 'tempoSync',
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
    const engine = new RealtimeEngine({ sampleRate: sr, maxBlockSize: outputSamples });
    engine.setClips([
      {
        id: 305,
        channels: [source],
        startPpq: 0,
        lengthSamples: outputSamples,
        warpMode: 'timeStretch',
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(engine.clipPageRequestOverflowCount).toBe(0);
      expect(engine.warpStretchOverflowCount).toBe(0);
      // Positive control on the reader itself: the sibling counter that has
      // always existed reads the same way, so a zero here is the counter
      // answering rather than a stub returning a default.
      expect(engine.externalMidiDroppedCount).toBe(0);
      expect(typeof engine.externalMidiDroppedCount).toBe('number');
      expect(Number.isSafeInteger(engine.externalMidiDroppedCount)).toBe(true);
      expect(typeof engine.clipPageRequestOverflowCount).toBe('number');
      expect(typeof engine.warpStretchOverflowCount).toBe('number');
    } finally {
      engine.destroy();
    }
  });

  it('sets and reads the warp voice capacity, rejecting out-of-domain values', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(engine.warpVoiceCapacity).toBe(8);

      engine.setWarpVoiceCapacity(12);
      expect(engine.warpVoiceCapacity).toBe(12);

      engine.setWarpVoiceCapacity(0);
      expect(engine.warpVoiceCapacity).toBe(0);

      // Rejected values leave the previously accepted capacity (0) in place.
      expect(() => engine.setWarpVoiceCapacity(65)).toThrow(RangeError);
      expect(() => engine.setWarpVoiceCapacity(65)).toThrow(/voices/);
      expect(engine.warpVoiceCapacity).toBe(0);

      expect(() => engine.setWarpVoiceCapacity(-1)).toThrow(RangeError);
      expect(engine.warpVoiceCapacity).toBe(0);

      expect(() => engine.setWarpVoiceCapacity(1.5)).toThrow(RangeError);
      expect(engine.warpVoiceCapacity).toBe(0);
    } finally {
      engine.destroy();
    }
  });
});
