import { beforeAll, describe, expect, it } from 'vitest';
import { createOpfsClipPageProvider, init, RealtimeEngine } from '../dist/index.js';

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
