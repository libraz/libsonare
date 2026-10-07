import vm from 'node:vm';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  createOpfsClipPageProvider,
  importOpfsClip,
  init,
  opfsClipPageWorkerSource,
  RealtimeEngine,
  SonareError,
  writeOpfsClip,
} from '../dist/index.js';

type FileMap = Map<string, Uint8Array>;

/** In-memory OPFS with exclusive sync access handles, driven by the real worker source. */
class OpfsWorkerHarness {
  readonly files: FileMap = new Map();
  readonly openHandles = new Set<string>();
  terminated = false;
  private listeners = new Set<(event: MessageEvent) => void>();
  private readonly scope: { onmessage?: (event: { data: unknown }) => Promise<void> };

  constructor() {
    const files = this.files;
    const openHandles = this.openHandles;
    const fileHandle = (key: string) => ({
      async createSyncAccessHandle() {
        if (openHandles.has(key)) {
          const error = new Error('locked');
          error.name = 'NoModificationAllowedError';
          throw error;
        }
        openHandles.add(key);
        return {
          read(buffer: Uint8Array, opts: { at: number }) {
            const data = files.get(key) ?? new Uint8Array(0);
            const n = Math.max(0, Math.min(buffer.byteLength, data.byteLength - opts.at));
            buffer.set(data.subarray(opts.at, opts.at + n));
            return n;
          },
          write(buffer: Uint8Array, opts: { at: number }) {
            const data = files.get(key) ?? new Uint8Array(0);
            const grown = new Uint8Array(Math.max(data.byteLength, opts.at + buffer.byteLength));
            grown.set(data);
            grown.set(buffer, opts.at);
            files.set(key, grown);
            return buffer.byteLength;
          },
          truncate(size: number) {
            files.set(key, (files.get(key) ?? new Uint8Array(0)).slice(0, size));
          },
          flush() {},
          close() {
            openHandles.delete(key);
          },
        };
      },
    });
    const dir = (prefix: string): unknown => ({
      async getDirectoryHandle(name: string) {
        return dir(`${prefix}${name}/`);
      },
      async getFileHandle(name: string, opts?: { create?: boolean }) {
        const key = `${prefix}${name}`;
        if (!files.has(key) && !opts?.create) {
          throw new Error('NotFoundError');
        }
        return fileHandle(key);
      },
    });
    const scope: Record<string, unknown> = {
      navigator: { storage: { getDirectory: async () => dir('') } },
      postMessage: (data: unknown) => {
        queueMicrotask(() => {
          for (const listener of this.listeners) {
            listener({ data } as MessageEvent);
          }
        });
      },
    };
    this.scope = scope as typeof this.scope;
    vm.runInNewContext(opfsClipPageWorkerSource, {
      self: scope,
      Promise,
      Map,
      Array,
      Math,
      String,
      Error,
      DataView,
      ArrayBuffer,
      Float32Array,
      Uint8Array,
    });
  }

  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listeners.add(listener as (event: MessageEvent) => void);
    }
  }

  removeEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listeners.delete(listener as (event: MessageEvent) => void);
    }
  }

  postMessage(data: unknown): void {
    void this.scope.onmessage?.({ data });
  }

  terminate(): void {
    this.terminated = true;
  }

  asWorker(): Worker {
    return this as unknown as Worker;
  }
}

function floatsOf(bytes: Uint8Array): Float32Array {
  return new Float32Array(
    bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  );
}

/** Frame-interleaved 16-bit PCM WAV; `samples` holds `channels` values per frame. */
function pcm16Wav(samples: number[], sampleRate: number, channels = 1): ArrayBuffer {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);
  const tag = (offset: number, text: string) => {
    for (let i = 0; i < text.length; ++i) {
      view.setUint8(offset + i, text.charCodeAt(i));
    }
  };
  tag(0, 'RIFF');
  view.setUint32(4, 36 + samples.length * 2, true);
  tag(8, 'WAVE');
  tag(12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, channels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2 * channels, true);
  view.setUint16(32, 2 * channels, true);
  view.setUint16(34, 16, true);
  tag(36, 'data');
  view.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; ++i) {
    view.setInt16(44 + i * 2, samples[i], true);
  }
  return buffer;
}

describe('writeOpfsClip', () => {
  beforeAll(async () => {
    await init();
  });

  it('writes headerless interleaved little-endian float32', async () => {
    const harness = new OpfsWorkerHarness();
    const left = Float32Array.from([0.1, 0.2, 0.3]);
    const right = Float32Array.from([-0.1, -0.2, -0.3]);
    const result = await writeOpfsClip('clips/a.f32', [left, right], {
      worker: harness.asWorker(),
    });
    expect(result).toEqual({ path: 'clips/a.f32', numChannels: 2, numSamples: 3 });
    const bytes = harness.files.get('clips/a.f32') as Uint8Array;
    expect(bytes.byteLength).toBe(3 * 2 * 4);
    expect(Array.from(floatsOf(bytes))).toEqual([
      left[0],
      right[0],
      left[1],
      right[1],
      left[2],
      right[2],
    ]);
    expect(harness.terminated).toBe(false);
  });

  it('replaces an existing longer file', async () => {
    const harness = new OpfsWorkerHarness();
    await writeOpfsClip('a.f32', [new Float32Array(100).fill(1)], { worker: harness.asWorker() });
    await writeOpfsClip('a.f32', [new Float32Array(4).fill(2)], { worker: harness.asWorker() });
    expect((harness.files.get('a.f32') as Uint8Array).byteLength).toBe(16);
  });

  it('splits a large clip into several chunks without changing the bytes', async () => {
    const harness = new OpfsWorkerHarness();
    const frames = 700_000;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; ++i) {
      left[i] = i;
      right[i] = -i;
    }
    const writes: number[] = [];
    const worker = harness.asWorker();
    const post = worker.postMessage.bind(worker);
    worker.postMessage = (message: unknown) => {
      writes.push((message as { at: number }).at);
      post(message);
    };
    await writeOpfsClip('big.f32', [left, right], { worker });
    expect(writes.length).toBeGreaterThan(1);
    const out = floatsOf(harness.files.get('big.f32') as Uint8Array);
    expect(out.length).toBe(frames * 2);
    for (const i of [0, 1, 524_287, 524_288, frames - 1]) {
      expect(out[i * 2]).toBe(i);
      expect(out[i * 2 + 1]).toBe(-i);
    }
  });

  it('reads back through the existing OPFS page reader with identical samples', async () => {
    const harness = new OpfsWorkerHarness();
    const channels = [
      Float32Array.from({ length: 10 }, (_, i) => i * 0.5),
      Float32Array.from({ length: 10 }, (_, i) => -i * 0.25),
    ];
    await writeOpfsClip('clips/rt.f32', channels, { worker: harness.asWorker() });
    const engine = new RealtimeEngine(48000, 8);
    const supplied: Float32Array[][] = [];
    const binding = createOpfsClipPageProvider(engine, {
      path: 'clips/rt.f32',
      numChannels: 2,
      numSamples: 10,
      pageFrames: 4,
      worker: harness.asWorker(),
      onPageSupplied: (_page, pageChannels) => supplied.push(pageChannels.map((c) => c.slice())),
    });
    try {
      for (let page = 0; page < 3; ++page) {
        expect(await binding.supplyPage(page)).toBe(true);
      }
      for (let page = 0; page < 3; ++page) {
        for (let ch = 0; ch < 2; ++ch) {
          const expected = channels[ch].slice(page * 4, page * 4 + 4);
          expect(Array.from(supplied[page][ch])).toEqual(Array.from(expected));
        }
      }
    } finally {
      binding.close();
      engine.destroy();
    }
  });

  it('refuses a path an open page provider holds, and accepts it after close', async () => {
    const harness = new OpfsWorkerHarness();
    await writeOpfsClip('held.f32', [new Float32Array(8)], { worker: harness.asWorker() });
    const engine = new RealtimeEngine(48000, 8);
    const binding = createOpfsClipPageProvider(engine, {
      path: '/held.f32',
      numChannels: 1,
      numSamples: 8,
      pageFrames: 4,
      worker: harness.asWorker(),
    });
    try {
      const error = await writeOpfsClip('held.f32', [new Float32Array(8)], {
        worker: harness.asWorker(),
      }).catch((e) => e);
      expect(error).toBeInstanceOf(SonareError);
      expect(error.codeName).toBe('InvalidState');
      binding.close();
      await expect(
        writeOpfsClip('held.f32', [new Float32Array(8)], { worker: harness.asWorker() }),
      ).resolves.toMatchObject({ numSamples: 8 });
    } finally {
      binding.close();
      engine.destroy();
    }
  });

  it('maps a handle held by a reader mid-read to InvalidState', async () => {
    const harness = new OpfsWorkerHarness();
    harness.openHandles.add('busy.f32');
    const error = await writeOpfsClip('busy.f32', [new Float32Array(4)], {
      worker: harness.asWorker(),
    }).catch((e) => e);
    expect(error).toBeInstanceOf(SonareError);
    expect(error.codeName).toBe('InvalidState');
  });

  it('terminates a worker it created itself only', async () => {
    const harness = new OpfsWorkerHarness();
    await writeOpfsClip('own.f32', [new Float32Array(2)], { worker: harness.asWorker() });
    expect(harness.terminated).toBe(false);
  });

  it('validates channels and path', async () => {
    const harness = new OpfsWorkerHarness();
    const options = { worker: harness.asWorker() };
    await expect(writeOpfsClip('x', [], options)).rejects.toBeInstanceOf(RangeError);
    await expect(writeOpfsClip('x', [new Float32Array(0)], options)).rejects.toBeInstanceOf(
      RangeError,
    );
    await expect(
      writeOpfsClip('x', [new Float32Array(2), new Float32Array(3)], options),
    ).rejects.toBeInstanceOf(RangeError);
    await expect(
      writeOpfsClip('x', [[1, 2]] as unknown as Float32Array[], options),
    ).rejects.toBeInstanceOf(TypeError);
    await expect(
      writeOpfsClip('x', new Float32Array(2) as unknown as Float32Array[], options),
    ).rejects.toBeInstanceOf(TypeError);
    await expect(
      writeOpfsClip(5 as unknown as string, [new Float32Array(2)], options),
    ).rejects.toBeInstanceOf(TypeError);
    expect(harness.files.size).toBe(0);
  });
});

describe('importOpfsClip', () => {
  beforeAll(async () => {
    await init();
  });

  const pcm = [0, 8192, 16384, -8192, -16384, 0, 4096, -4096];
  const expected = pcm.map((v) => v / 32768);

  it('imports an ArrayBuffer and returns the decoded sample rate', async () => {
    const harness = new OpfsWorkerHarness();
    const result = await importOpfsClip('i/a.f32', pcm16Wav(pcm, 22050), {
      worker: harness.asWorker(),
    });
    expect(result).toEqual({ path: 'i/a.f32', numChannels: 1, numSamples: 8, sampleRate: 22050 });
    const out = floatsOf(harness.files.get('i/a.f32') as Uint8Array);
    for (let i = 0; i < expected.length; ++i) {
      expect(out[i]).toBeCloseTo(expected[i], 3);
    }
  });

  it('imports a Blob', async () => {
    const harness = new OpfsWorkerHarness();
    const result = await importOpfsClip('i/b.f32', new Blob([pcm16Wav(pcm, 16000)]), {
      worker: harness.asWorker(),
    });
    expect(result).toMatchObject({ numChannels: 1, numSamples: 8, sampleRate: 16000 });
    expect(floatsOf(harness.files.get('i/b.f32') as Uint8Array).length).toBe(8);
  });

  it('imports encoded stereo keeping both channels', async () => {
    const harness = new OpfsWorkerHarness();
    const frames = [
      [8192, -8192],
      [16384, 4096],
      [0, -16384],
    ];
    const result = await importOpfsClip('i/s.f32', pcm16Wav(frames.flat(), 32000, 2), {
      worker: harness.asWorker(),
    });
    expect(result).toEqual({ path: 'i/s.f32', numChannels: 2, numSamples: 3, sampleRate: 32000 });
    const out = floatsOf(harness.files.get('i/s.f32') as Uint8Array);
    expect(out.length).toBe(6);
    frames.flat().forEach((v, i) => {
      expect(out[i]).toBeCloseTo(v / 32768, 4);
    });
  });

  it('imports an AudioBuffer keeping every channel', async () => {
    const harness = new OpfsWorkerHarness();
    const left = Float32Array.from([1, 2, 3]);
    const right = Float32Array.from([4, 5, 6]);
    const buffer = {
      numberOfChannels: 2,
      sampleRate: 44100,
      length: 3,
      getChannelData: (ch: number) => (ch === 0 ? left : right),
    } as unknown as AudioBuffer;
    const result = await importOpfsClip('i/c.f32', buffer, { worker: harness.asWorker() });
    expect(result).toEqual({ path: 'i/c.f32', numChannels: 2, numSamples: 3, sampleRate: 44100 });
    expect(Array.from(floatsOf(harness.files.get('i/c.f32') as Uint8Array))).toEqual([
      1, 4, 2, 5, 3, 6,
    ]);
  });

  it('rejects a source of another kind', async () => {
    const harness = new OpfsWorkerHarness();
    await expect(
      importOpfsClip('i/d.f32', 'nope' as unknown as ArrayBuffer, { worker: harness.asWorker() }),
    ).rejects.toBeInstanceOf(TypeError);
  });
});
