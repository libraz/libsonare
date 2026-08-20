import { beforeAll, describe, expect, it } from 'vitest';
// `attachOpfsClipStream` ships in both bundles, and each call below takes it
// from the bundle its other arguments come from. tsup emits every entry as a
// self-contained .d.ts, so ClipPageProvider is declared twice and its private
// field makes the two copies nominally distinct.
import {
  attachOpfsClipStream,
  ClipPageStreamer,
  createOpfsClipPageProvider,
  init,
  RealtimeEngine,
} from '../dist/index.js';
import {
  attachOpfsClipStream as attachWorkletOpfsClipStream,
  init as initWorklet,
  SonareEngine,
  SonareEngineCommandType,
  SonareRealtimeEngineWorkletProcessor,
} from '../dist/worklet.js';

class FakeClipPageWorker {
  listener: ((event: MessageEvent) => void) | null = null;
  terminated = false;
  messages: unknown[] = [];
  framesOverride: number | null = null;

  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listener = listener as (event: MessageEvent) => void;
    }
  }

  removeEventListener(type: string): void {
    if (type === 'message') {
      this.listener = null;
    }
  }

  postMessage(message: {
    type: string;
    requestId: number;
    pageIndex: number;
    numChannels: number;
    numSamples: number;
    pageFrames: number;
  }): void {
    this.messages.push(message);
    const startFrame = message.pageIndex * message.pageFrames;
    const frames =
      this.framesOverride ?? Math.min(message.pageFrames, message.numSamples - startFrame);
    const buffers = Array.from({ length: message.numChannels }, () =>
      typeof SharedArrayBuffer === 'function'
        ? new SharedArrayBuffer(frames * Float32Array.BYTES_PER_ELEMENT)
        : new ArrayBuffer(frames * Float32Array.BYTES_PER_ELEMENT),
    );
    for (let ch = 0; ch < message.numChannels; ++ch) {
      const channel = new Float32Array(buffers[ch]);
      for (let frame = 0; frame < frames; ++frame) {
        channel[frame] = startFrame + frame + 1;
      }
    }
    queueMicrotask(() => {
      this.listener?.({
        data: {
          type: 'sonare:clip-page',
          requestId: message.requestId,
          pageIndex: message.pageIndex,
          ok: true,
          frames,
          channelBuffers: buffers,
        },
      } as MessageEvent);
    });
  }

  terminate(): void {
    this.terminated = true;
  }
}

describe('createOpfsClipPageProvider', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('feeds page requests through a worker-backed provider', async () => {
    const engine = new RealtimeEngine(48000, 8);
    const worker = new FakeClipPageWorker();
    const binding = createOpfsClipPageProvider(engine, {
      path: 'clips/clip.f32',
      numChannels: 1,
      numSamples: 8,
      pageFrames: 4,
      worker: worker as unknown as Worker,
    });

    expect(await binding.supplyPage(0)).toBe(true);
    engine.setClips([{ id: 306, pageProvider: binding.provider, startPpq: 0 }]);
    engine.play();
    const first = engine.process([new Float32Array(8)]);
    expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

    const request = engine.popClipPageRequest();
    expect(request).toEqual({ clipId: 306, channel: 0, sample: 4 });
    expect(request && (await binding.supplyRequest(request))).toBe(true);
    engine.seekSample(0);
    const second = engine.process([new Float32Array(8)]);
    expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

    binding.close();
    engine.destroy();
    expect(worker.terminated).toBe(false);
    expect(worker.messages).toHaveLength(2);
  });

  it('rejects short non-final pages returned by the worker', async () => {
    const engine = new RealtimeEngine(48000, 8);
    const worker = new FakeClipPageWorker();
    worker.framesOverride = 2;
    const binding = createOpfsClipPageProvider(engine, {
      path: 'clips/clip.f32',
      numChannels: 1,
      numSamples: 8,
      pageFrames: 4,
      worker: worker as unknown as Worker,
    });

    expect(await binding.supplyPage(0)).toBe(false);

    binding.close();
    engine.destroy();
  });

  it('serializes concurrent page reads sent to the worker', async () => {
    const engine = new RealtimeEngine(48000, 8);
    const worker = new FakeClipPageWorker();
    const binding = createOpfsClipPageProvider(engine, {
      path: 'clips/clip.f32',
      numChannels: 1,
      numSamples: 8,
      pageFrames: 4,
      worker: worker as unknown as Worker,
    });

    const first = binding.supplyPage(0);
    const second = binding.supplyPage(1);
    await Promise.resolve();
    await Promise.resolve();
    expect(worker.messages).toHaveLength(1);
    expect(await first).toBe(true);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(worker.messages).toHaveLength(2);
    expect(await second).toBe(true);

    binding.close();
    engine.destroy();
  });
});

describe('worklet attachOpfsClipStream', () => {
  it('primes, pulls, and bounds pages through the realtime-safe SAB AudioWorklet bridge', async () => {
    const blockSize = 128;
    const worker = new FakeClipPageWorker();
    let processor: SonareRealtimeEngineWorkletProcessor | undefined;
    const port = {
      postMessage: (message: unknown, transfer?: Transferable[]) => {
        // The worker deliberately returns SharedArrayBuffer-backed pages. A
        // browser rejects SABs in a transfer list, so this bridge must send
        // them clone-only instead of silently leaving the page unavailable.
        expect(
          transfer?.some(
            (entry) =>
              typeof SharedArrayBuffer === 'function' && entry instanceof SharedArrayBuffer,
          ) ?? false,
        ).toBe(false);
        const type =
          typeof message === 'object' && message !== null
            ? (message as { type?: unknown }).type
            : undefined;
        if (typeof type === 'number') {
          processor?.receiveCommand(
            message as Parameters<SonareRealtimeEngineWorkletProcessor['receiveCommand']>[0],
          );
        } else {
          processor?.receiveSync(
            message as Parameters<SonareRealtimeEngineWorkletProcessor['receiveSync']>[0],
          );
        }
      },
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    };
    const context = { sampleRate: 48000 } as BaseAudioContext;
    const engine = await SonareEngine.create(context, {
      mode: 'sab',
      offlineChannelCount: 1,
      blockSize,
      channelCount: 1,
      nodeFactory: (_context, _name, options) => {
        processor = new SonareRealtimeEngineWorkletProcessor(
          options.processorOptions as ConstructorParameters<
            typeof SonareRealtimeEngineWorkletProcessor
          >[0],
          {
            postMessage: (message) => port.onmessage?.({ data: message } as MessageEvent<unknown>),
          },
        );
        // A browser worklet initializes asynchronously. Mirror that timing so
        // the node has installed its port handler before the ready signal.
        queueMicrotask(() => port.onmessage?.({ data: { type: 'ready' } } as MessageEvent));
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    });
    try {
      if (!processor) {
        throw new Error('expected a worklet processor');
      }
      engine.setTrackLanes([10]);
      const { provider } = await attachWorkletOpfsClipStream(engine, {
        path: 'clips/clip.f32',
        clipId: 701,
        numChannels: 1,
        numSamples: 8,
        pageFrames: 4,
        worker: worker as unknown as Worker,
      });
      engine.addClip(10, provider, 0, { id: 701 });
      engine.transport.play();

      const first = new Float32Array(blockSize);
      const missing = new Float32Array(blockSize);
      expect(processor.process([[]], [[first]])).toBe(true);
      expect(processor.process([[]], [[missing]])).toBe(true);
      expect(Array.from(first.slice(0, 4))).toEqual([1, 2, 3, 4]);
      expect(first.slice(4).every((sample) => sample === 0)).toBe(true);
      expect(missing.every((sample) => sample === 0)).toBe(true);

      // The main thread polls the SAB ring, pumps the same streamer used by
      // direct engines, and the fake OPFS worker supplies only page 1.
      await new Promise((resolve) => setTimeout(resolve, 16));
      expect(
        worker.messages.map((message) => (message as { pageIndex: number }).pageIndex),
      ).toEqual([0, 1]);

      processor.receiveCommand({
        type: SonareEngineCommandType.TransportSeekSample,
        sampleTime: -1,
        argInt: 0,
      });
      const replayedFirst = new Float32Array(blockSize);
      expect(processor.process([[]], [[replayedFirst]])).toBe(true);
      expect(Array.from(replayedFirst.slice(0, 8))).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
      expect(replayedFirst.slice(8).every((sample) => sample === 0)).toBe(true);
    } finally {
      engine.destroy();
      processor?.destroy();
    }
  });

  it('rejects OPFS streaming on the explicitly non-realtime-safe postMessage fallback', async () => {
    const port = {
      postMessage: () => undefined,
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    };
    const engine = await SonareEngine.create({ sampleRate: 48000 } as BaseAudioContext, {
      mode: 'postMessage',
      nodeFactory: () => {
        queueMicrotask(() => port.onmessage?.({ data: { type: 'ready' } } as MessageEvent));
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    });
    try {
      await expect(
        attachWorkletOpfsClipStream(engine, {
          path: 'clips/clip.f32',
          clipId: 702,
          numChannels: 1,
          numSamples: 8,
          pageFrames: 4,
          worker: new FakeClipPageWorker() as unknown as Worker,
        }),
      ).rejects.toThrow(/postMessage fallback is not realtime-safe/);
    } finally {
      engine.destroy();
    }
  });

  it('bounds pending worklet page misses while OPFS service is stalled', async () => {
    const port = {
      postMessage: () => undefined,
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    };
    const context = { sampleRate: 48000 } as BaseAudioContext;
    const engine = await SonareEngine.create(context, {
      mode: 'postMessage',
      nodeFactory: () => {
        queueMicrotask(() => port.onmessage?.({ data: { type: 'ready' } } as MessageEvent));
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    });
    try {
      for (let clipId = 0; clipId < 1024; ++clipId) {
        port.onmessage?.({
          data: { type: 'clipPageRequest', requests: [{ clipId, pageIndex: clipId }] },
        } as MessageEvent<unknown>);
      }
      const pending = (
        engine as unknown as { workletClipPageRequests: Map<number, { pageIndex: number }> }
      ).workletClipPageRequests;
      expect(pending.size).toBeLessThanOrEqual(256);
      expect(pending.get(1023)).toEqual({ clipId: 1023, pageIndex: 1023 });
      expect(pending.has(0)).toBe(false);
    } finally {
      engine.destroy();
    }
  });
});

describe('attachOpfsClipStream', () => {
  beforeAll(async () => {
    await init();
  });

  it('wires a streaming clip in one call and feeds it within the window', async () => {
    const engine = new RealtimeEngine(48000, 8);
    const worker = new FakeClipPageWorker();
    // Default window retains page 0 behind the frontier, so a seek back replays it.
    const streamer = new ClipPageStreamer(engine);
    const { provider } = await attachOpfsClipStream(streamer, engine, {
      path: 'clips/clip.f32',
      clipId: 400,
      numChannels: 1,
      numSamples: 8,
      pageFrames: 4,
      worker: worker as unknown as Worker,
    });

    engine.setClips([{ id: 400, pageProvider: provider, startPpq: 0 }]);
    engine.play();
    // Page 0 was primed by attachOpfsClipStream, so the first block plays it.
    const first = engine.process([new Float32Array(8)]);
    expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

    // The miss for page 1 is drained and serviced by the streamer's pump.
    await streamer.pump();
    engine.seekSample(0);
    const second = engine.process([new Float32Array(8)]);
    expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

    streamer.close();
    engine.destroy();
  });
});
