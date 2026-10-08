/**
 * Offline export through the SonareEngine facade: bounce, freeze and the
 * request form of renderOffline all run on the facade's main-thread mirror.
 */

import { RealtimeEngine as RawRealtimeEngine } from '../dist/index.js';
import { describe, expect, it, SonareEngine, setupWorklet } from './_worklet_helpers';

const SR = 48000;
const BLOCK = 128;

type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

function midi1Word(status: number, channel: number, data1: number, data2: number): number {
  return (
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | ((data1 & 0x7f) << 8) | data2
  );
}

/** An engine holding a pad whose note never ends inside any render span. */
function padEngine(): RawRealtimeEngine {
  const engine = new RawRealtimeEngine(SR, BLOCK);
  engine.setBuiltinInstrument({ gain: 0.5 }, 5);
  engine.setMidiClips([
    {
      id: 1,
      trackId: 5,
      destinationId: 5,
      lengthSamples: 1 << 20,
      events: [
        { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
        // A second note after the position the bounce test seeks to.
        { renderFrame: 13000, word0: midi1Word(0x9, 0, 64, 100), wordCount: 1 },
      ],
    },
  ]);
  return engine;
}

const rms = (data: ArrayLike<number>): number => {
  let sum = 0;
  for (let i = 0; i < data.length; i++) {
    sum += data[i] * data[i];
  }
  return Math.sqrt(sum / data.length);
};

function maxAbsDiff(a: ArrayLike<number>, b: ArrayLike<number>): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

/** Builds the facade over `mirror`, recording every message posted to the worklet. */
async function createFacade(
  mirror: RawRealtimeEngine,
  options: { mode?: 'sab' | 'postMessage'; offlineBlockSize?: number } = {},
): Promise<{ engine: SonareEngine; posted: Array<Record<string, unknown>> }> {
  const posted: Array<Record<string, unknown>> = [];
  const port = {
    postMessage: (message: Record<string, unknown>) => posted.push(message),
    onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
  };
  const engine = await SonareEngine.create({ sampleRate: SR } as BaseAudioContext, {
    mode: options.mode ?? 'postMessage',
    offlineEngine: mirror as unknown as OfflineEngineOption,
    offlineChannelCount: 2,
    offlineBlockSize: options.offlineBlockSize ?? BLOCK,
    nodeFactory: () => {
      queueMicrotask(() => port.onmessage?.({ data: { type: 'ready' } } as MessageEvent));
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
  return { engine, posted };
}

describe('SonareEngine offline export', () => {
  setupWorklet();

  it('refuses a block size above the offline block size, on the facade and the raw engine', async () => {
    const { engine } = await createFacade(padEngine());
    try {
      const rejects = async (run: () => Promise<unknown>) => {
        await expect(run()).rejects.toBeInstanceOf(RangeError);
      };
      await rejects(() => engine.renderOffline({ totalFrames: 256, blockSize: BLOCK + 1 }));
      await rejects(() => engine.bounceOffline({ totalFrames: 256, blockSize: BLOCK + 1 }));
      await rejects(() => engine.freezeOffline({ totalFrames: 256, blockSize: BLOCK + 1 }));
      await rejects(() => engine.renderOffline({ totalFrames: 256, blockSize: 0 }));
    } finally {
      engine.destroy();
    }

    const raw = padEngine();
    try {
      const refused = (run: () => unknown) => {
        expect(run).toThrow(expect.objectContaining({ code: 4 }));
      };
      const planes = () => [new Float32Array(256), new Float32Array(256)];
      refused(() => raw.renderOffline(planes(), BLOCK + 1));
      refused(() => raw.renderOffline({ channels: planes(), blockSize: BLOCK + 1 }));
      refused(() => raw.primeOfflineParameters(2, BLOCK + 1));
      refused(() => raw.bounceOffline({ totalFrames: 256, blockSize: BLOCK + 1 }));
      refused(() => raw.freezeOffline({ totalFrames: 256, blockSize: BLOCK + 1 }));
      expect(() => raw.renderOffline(planes(), BLOCK)).not.toThrow();
    } finally {
      raw.destroy();
    }
  });

  it('bounces exactly what the raw engine bounces and restores the mirror position', async () => {
    const frames = 4096;
    const reference = padEngine();
    reference.seekSample(12000);
    reference.applyCommandsDueNowPreservingFuture();
    const expected = reference.bounceOffline({ totalFrames: frames, numChannels: 2 });
    reference.destroy();

    const mirror = padEngine();
    const { engine } = await createFacade(mirror);
    try {
      engine.transport.seekSeconds(12000 / SR);
      const bounced = await engine.bounceOffline({ totalFrames: frames, numChannels: 2 });
      // Non-vacuity: a silent pair would compare equal to anything silent.
      expect(rms(expected.interleaved)).toBeGreaterThan(0);
      expect(bounced.frames).toBe(frames);
      expect(bounced.numChannels).toBe(2);
      expect(maxAbsDiff(bounced.interleaved, expected.interleaved)).toBeLessThan(1e-6);
      expect(mirror.getTransportState().samplePosition).toBe(12000);
    } finally {
      engine.destroy();
    }
  });

  it('renders the request form like the numeric form and restores the position', async () => {
    const frames = 2048;
    const renderWith = async (
      request: number | { totalFrames: number; blockSize?: number },
    ): Promise<Float32Array[]> => {
      const mirror = padEngine();
      const { engine } = await createFacade(mirror);
      try {
        const planes = await engine.renderOffline(request);
        expect(mirror.getTransportState().samplePosition).toBe(0);
        return planes;
      } finally {
        engine.destroy();
      }
    };
    const numeric = await renderWith(frames);
    const request = await renderWith({ totalFrames: frames, blockSize: BLOCK });
    expect(rms(numeric[0])).toBeGreaterThan(0);
    expect(request).toHaveLength(2);
    expect(maxAbsDiff(request[0], numeric[0])).toBeLessThan(1e-6);
    expect(maxAbsDiff(request[1], numeric[1])).toBeLessThan(1e-6);
  });

  it('concatenates chunked renders and rewinds only when the timeline ends', async () => {
    const chunk = 4096;
    const chunks = 3;
    const total = chunk * chunks;

    const continuousMirror = padEngine();
    const continuous = await (async () => {
      const { engine } = await createFacade(continuousMirror);
      try {
        return Array.from((await engine.renderOffline(total))[0]);
      } finally {
        engine.destroy();
      }
    })();
    expect(rms(continuous)).toBeGreaterThan(0);

    const mirror = padEngine();
    const { engine } = await createFacade(mirror);
    try {
      const joined: number[] = [];
      for (let i = 0; i < chunks; i++) {
        const [left] = await engine.renderOffline({
          totalFrames: chunk,
          blockSize: BLOCK,
          finalize: false,
        });
        joined.push(...left);
        // The mirror advances between chunks; rewinding there would re-render chunk 0.
        expect(mirror.getTransportState().samplePosition).toBe(chunk * (i + 1));
      }
      engine.finishOfflineRender();
      expect(mirror.getTransportState().samplePosition).toBe(0);
      expect(maxAbsDiff(joined, continuous)).toBeLessThan(1e-6);
    } finally {
      engine.destroy();
    }
  });

  it('freezes into a single clip held by the facade, the mirror and the worklet', async () => {
    const mirror = new RawRealtimeEngine(SR, BLOCK);
    const { engine, posted } = await createFacade(mirror);
    try {
      const tone = new Float32Array(2048).map((_, i) => 0.25 * Math.sin(i * 0.05));
      engine.setTrackLanes([10]);
      const sourceId = engine.addClip(10, [tone, tone], 0, { id: 11 });
      expect(sourceId).toBe(11);
      posted.length = 0;

      const frozen = await engine.freezeOffline({
        totalFrames: 1024,
        numChannels: 2,
        clipId: 42,
        startPpq: 0,
      });
      expect(frozen).toEqual({ clipId: 42, frames: 1024, numChannels: 2 });
      expect(mirror.clipCount()).toBe(1);
      expect(mirror.getTransportState().samplePosition).toBe(0);

      const delta = posted.find((message) => message.type === 'syncClipsDelta') as
        | { upserts: Array<{ id: number; channels: Float32Array[] }>; removeIds: number[] }
        | undefined;
      expect(delta?.removeIds).toEqual([11]);
      expect(delta?.upserts.map((clip) => clip.id)).toEqual([42]);
      expect(rms(delta?.upserts[0].channels[0] ?? [])).toBeGreaterThan(0);

      // The store follows: removing the old id is now a no-op delta and the
      // frozen clip bounces as the whole timeline.
      const bounced = await engine.bounceOffline({ totalFrames: 1024, numChannels: 2 });
      expect(rms(bounced.interleaved)).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });

  it('withdraws the MIDI a freeze baked, so a frozen MIDI lane plays once', async () => {
    const mirror = new RawRealtimeEngine(SR, BLOCK);
    // A near-instant release, so no tail of the earlier render rings into the replay.
    mirror.setBuiltinInstrument({ gain: 0.5, sustain: 1, releaseMs: 0.01 }, 5);
    const { engine, posted } = await createFacade(mirror);
    try {
      engine.setMidiClips([
        {
          id: 1,
          trackId: 5,
          destinationId: 5,
          lengthSamples: 1 << 20,
          events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }],
        },
      ]);
      const [live] = await engine.renderOffline({ totalFrames: 1024, blockSize: BLOCK });
      expect(rms(live)).toBeGreaterThan(0);
      posted.length = 0;

      await engine.freezeOffline({ totalFrames: 1024, numChannels: 2, clipId: 42, startPpq: 0 });
      // The worklet is told to drop the MIDI clips along with the old clip set.
      const midiSync = posted.find((message) => message.type === 'syncMidiClips') as
        | { clips: unknown[] }
        | undefined;
      expect(midiSync?.clips).toEqual([]);
      const delta = posted.find((message) => message.type === 'syncClipsDelta') as
        | { upserts: Array<{ id: number; channels: Float32Array[] }> }
        | undefined;
      const frozen = delta?.upserts[0].channels[0] ?? new Float32Array(1024);
      expect(rms(frozen)).toBeGreaterThan(0);

      // Replaying the timeline sounds the frozen note alone, not baked plus live.
      const [replay] = await engine.renderOffline({ totalFrames: 1024, blockSize: BLOCK });
      expect(maxAbsDiff(replay, frozen)).toBeLessThan(1e-5);
    } finally {
      engine.destroy();
    }
  });
});

class FakeClipPageWorker {
  listener: ((event: MessageEvent) => void) | null = null;
  pages: number[] = [];
  /** Pages answered as unreadable. */
  failPages = new Set<number>();

  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listener = listener as (event: MessageEvent) => void;
    }
  }

  removeEventListener(): void {
    this.listener = null;
  }

  terminate(): void {}

  postMessage(message: {
    requestId: number;
    pageIndex: number;
    numChannels: number;
    numSamples: number;
    pageFrames: number;
  }): void {
    this.pages.push(message.pageIndex);
    const start = message.pageIndex * message.pageFrames;
    const frames = Math.min(message.pageFrames, message.numSamples - start);
    const buffers = Array.from({ length: message.numChannels }, () => {
      const channel = new Float32Array(frames);
      for (let frame = 0; frame < frames; ++frame) {
        channel[frame] = 0.5;
      }
      return channel.buffer;
    });
    queueMicrotask(() =>
      this.listener?.({
        data: {
          type: 'sonare:clip-page',
          requestId: message.requestId,
          pageIndex: message.pageIndex,
          ok: !this.failPages.has(message.pageIndex),
          frames,
          channelBuffers: buffers,
        },
      } as MessageEvent),
    );
  }
}

describe('SonareEngine offline export of OPFS-streamed clips', () => {
  setupWorklet();

  it('renders every page of a stream clip, and rejects a span with an unreadable page', async () => {
    const pageFrames = 128;
    const numSamples = 4 * pageFrames;
    const worker = new FakeClipPageWorker();
    const mirror = new RawRealtimeEngine(SR, BLOCK);
    const { engine } = await createFacade(mirror, { mode: 'sab' });
    try {
      engine.setTrackLanes([10]);
      const { provider } = await engine.attachOpfsClipStream({
        path: 'clips/clip.f32',
        clipId: 702,
        numChannels: 2,
        numSamples,
        pageFrames,
        primePages: 1,
        worker: worker as unknown as Worker,
      });
      engine.addClip(10, provider, 0, { id: 702 });

      const [left] = await engine.renderOffline({ totalFrames: numSamples });
      for (let page = 0; page < 4; page++) {
        expect(rms(left.slice(page * pageFrames, (page + 1) * pageFrames))).toBeGreaterThan(0);
      }

      worker.failPages.add(2);
      engine.transport.seekSeconds(0);
      await expect(engine.renderOffline({ totalFrames: numSamples })).rejects.toThrow(/page 2/);
    } finally {
      engine.destroy();
    }
  });

  it('pages in the span before rendering and evicts what it supplied', async () => {
    const pageFrames = 128;
    const numSamples = 4 * pageFrames;
    const worker = new FakeClipPageWorker();
    const mirror = new RawRealtimeEngine(SR, BLOCK);
    const { engine, posted } = await createFacade(mirror, { mode: 'sab' });
    try {
      engine.setTrackLanes([10]);
      const { provider } = await engine.attachOpfsClipStream({
        path: 'clips/clip.f32',
        clipId: 701,
        numChannels: 2,
        numSamples,
        pageFrames,
        primePages: 0,
        worker: worker as unknown as Worker,
      });
      engine.addClip(10, provider, 0, { id: 701 });
      expect(worker.pages).toEqual([]);

      const bounced = await engine.bounceOffline({ totalFrames: numSamples, numChannels: 2 });

      expect(worker.pages).toEqual([0, 1, 2, 3]);
      // Every page of the span rendered as audio rather than as silence.
      for (let page = 0; page < 4; page++) {
        const slice = bounced.interleaved.slice(page * pageFrames * 2, (page + 1) * pageFrames * 2);
        expect(rms(slice)).toBeGreaterThan(0);
      }
      const cleared = posted
        .filter((message) => message.type === 'syncClipPageClear')
        .map((message) => message.pageIndex);
      expect(cleared).toEqual([0, 1, 2, 3]);
    } finally {
      engine.destroy();
    }
  });
});
