import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { init, StreamAnalyzer } from '../../dist/index.js';
import {
  init as initWorklet,
  registerSonareStreamAnalyzerWorkletProcessor,
  SonareStreamAnalyzerNode,
  SonareStreamAnalyzerWorkletProcessor,
} from '../../dist/worklet.js';

const QUANTUM = 128;

interface Posted {
  message: Record<string, unknown>;
  transfer?: unknown[];
}

function fakePort() {
  const posted: Posted[] = [];
  return {
    posted,
    port: {
      postMessage: (message: unknown, transfer?: unknown[]) => {
        posted.push({ message: message as Record<string, unknown>, transfer });
      },
    },
  };
}

/** Records each chunk as it is posted, since the processor reuses its message object. */
function chunkRecorder() {
  const { posted, port } = fakePort();
  const chunks: Array<{
    samples: Float32Array;
    startSample: number;
    discontinuity: boolean;
    transfer?: unknown[];
    message: unknown;
    firstSample: number;
  }> = [];
  const recording = {
    postMessage: (message: unknown, transfer?: unknown[]) => {
      port.postMessage(message, transfer);
      const m = message as Record<string, unknown>;
      if (m.type === 'chunk') {
        chunks.push({
          samples: m.samples as Float32Array,
          startSample: m.startSample as number,
          discontinuity: m.discontinuity as boolean,
          transfer: transfer?.slice(),
          firstSample: (m.samples as Float32Array)[0],
          message,
        });
      }
    },
  };
  return { posted, chunks, port: recording };
}

function quantum(channels: number[], frames = QUANTUM): Float32Array[][] {
  return [channels.map((value) => new Float32Array(frames).fill(value))];
}

function feed(processor: SonareStreamAnalyzerWorkletProcessor, quanta: number, value = 0.5) {
  for (let i = 0; i < quanta; i++) {
    expect(processor.process(quantum([value]), [])).toBe(true);
  }
}

describe('SonareStreamAnalyzerWorkletProcessor', () => {
  it('announces itself and posts a chunk once chunkFrames samples are in', () => {
    const { posted, chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 256 }, port);
    expect(posted[0].message).toEqual({ type: 'ready' });
    feed(processor, 1);
    expect(chunks).toHaveLength(0);
    feed(processor, 3);
    expect(chunks).toHaveLength(2);
    expect(chunks.map((c) => c.startSample)).toEqual([0, 256]);
    expect(chunks.every((c) => c.samples.length === 256 && !c.discontinuity)).toBe(true);
    expect(chunks[0].transfer).toEqual([chunks[0].samples.buffer]);
  });

  it('defaults to 4096-sample chunks and refuses a chunk shorter than a render quantum', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({}, port);
    feed(processor, 31);
    expect(chunks).toHaveLength(0);
    feed(processor, 1);
    expect(chunks[0].samples).toHaveLength(4096);
    expect(() => new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 64 }, port)).toThrow(
      RangeError,
    );
    expect(() => new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 300.5 }, port)).toThrow(
      /chunkFrames must be an integer of at least 128/,
    );
  });

  it('averages every input channel, not just the first', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    processor.process(quantum([1, 3]), []);
    processor.process(quantum([0, 0, 0.6]), []);
    processor.process(quantum([-0.5]), []);
    expect(chunks[0].samples[0]).toBeCloseTo(2, 6);
    expect(chunks[0].samples[127]).toBeCloseTo(2, 6);
    expect(chunks[1].samples[0]).toBeCloseTo(0.2, 6);
    expect(chunks[2].samples[0]).toBeCloseTo(-0.5, 6);
  });

  it('keeps samples contiguous when a chunk ends inside a render quantum', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 192 }, port);
    let n = 0;
    for (let q = 0; q < 6; q++) {
      const block = new Float32Array(QUANTUM);
      for (let i = 0; i < QUANTUM; i++) {
        block[i] = n++;
      }
      processor.process([[block]], []);
    }
    expect(chunks).toHaveLength(4);
    const joined = chunks.flatMap((c) => Array.from(c.samples));
    expect(joined).toEqual(Array.from({ length: 4 * 192 }, (_, i) => i));
    expect(chunks.map((c) => c.startSample)).toEqual([0, 192, 384, 576]);
  });

  it('posts through one reused message object', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    feed(processor, 3);
    expect(chunks[0].message).toBe(chunks[1].message);
  });

  it('drops a chunk when the pool is empty and flags the discontinuity on the next one', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    feed(processor, 4);
    expect(chunks).toHaveLength(4);
    // Nothing returned yet: this chunk has no buffer to fill.
    feed(processor, 1);
    expect(chunks).toHaveLength(4);
    // The next chunk is filled once a buffer has come back.
    processor.receiveMessage({ type: 'recycle', samples: chunks[0].samples });
    feed(processor, 1);
    expect(chunks).toHaveLength(4);
    processor.receiveMessage({ type: 'recycle', samples: chunks[1].samples });
    feed(processor, 1);
    expect(chunks).toHaveLength(5);
    expect(chunks[4].startSample).toBe(6 * 128);
    expect(chunks[4].discontinuity).toBe(true);
    expect(chunks[4].samples).toBe(chunks[0].samples);
    feed(processor, 1);
    expect(chunks[5].discontinuity).toBe(false);
    expect(chunks[5].startSample).toBe(7 * 128);
  });

  it('recycles a returned buffer so the stream continues without a gap', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    for (let i = 0; i < 20; i++) {
      feed(processor, 1, i);
      processor.receiveMessage({ type: 'recycle', samples: chunks[i].samples });
    }
    expect(chunks).toHaveLength(20);
    expect(chunks.every((c) => !c.discontinuity)).toBe(true);
    expect(chunks.map((c) => c.firstSample)).toEqual(Array.from({ length: 20 }, (_, i) => i));
  });

  it('ignores a recycled buffer of the wrong size', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    feed(processor, 4);
    processor.receiveMessage({ type: 'recycle', samples: new Float32Array(64) });
    feed(processor, 2);
    expect(chunks).toHaveLength(4);
  });

  it('leaves a quantum with no connected input alone and stops after destroy', () => {
    const { chunks, port } = chunkRecorder();
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 }, port);
    expect(processor.process([[]], [])).toBe(true);
    feed(processor, 1);
    expect(chunks).toHaveLength(1);
    processor.receiveMessage({ type: 'destroy' });
    expect(processor.process(quantum([1]), [])).toBe(false);
    expect(chunks).toHaveLength(1);
  });
});

describe('registerSonareStreamAnalyzerWorkletProcessor', () => {
  const scope = globalThis as unknown as Record<string, unknown>;

  afterEach(() => {
    scope.AudioWorkletProcessor = undefined;
    scope.registerProcessor = undefined;
  });

  it('throws outside an AudioWorkletGlobalScope', () => {
    expect(() => registerSonareStreamAnalyzerWorkletProcessor()).toThrow(
      'AudioWorkletProcessor is not available in this context.',
    );
  });

  it('registers a processor that taps the input and routes port messages', () => {
    const registered: Record<string, new (options?: unknown) => { process: unknown }> = {};
    const posted: Array<Record<string, unknown>> = [];
    let listener: ((event: { data: unknown }) => void) | undefined;
    scope.AudioWorkletProcessor = class {
      port = {
        postMessage: (message: unknown) => posted.push({ ...(message as object) }),
        addEventListener: (_type: string, callback: (event: { data: unknown }) => void) => {
          listener = callback;
        },
      };
    };
    scope.registerProcessor = (name: string, ctor: never) => {
      registered[name] = ctor;
    };
    registerSonareStreamAnalyzerWorkletProcessor();
    const Ctor = registered['sonare-stream-analyzer-processor'];
    const instance = new Ctor({ processorOptions: { chunkFrames: 128 } }) as {
      process: (inputs: Float32Array[][], outputs: Float32Array[][]) => boolean;
    };
    expect(posted[0]).toEqual({ type: 'ready' });
    expect(instance.process(quantum([0.25]), [])).toBe(true);
    expect(posted.map((m) => m.type)).toEqual(['ready', 'chunk']);
    listener?.({ data: { type: 'unknown' } });
    listener?.({ data: { type: 'destroy' } });
    expect(instance.process(quantum([0.25]), [])).toBe(false);
  });
});

describe('SonareStreamAnalyzerNode', () => {
  const CONFIG = { nFft: 512, hopLength: 256, nMels: 16, sampleRate: 22050 };
  const CHUNK = 1024;

  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  function fakeNode() {
    const posted: Posted[] = [];
    const node = {
      port: {
        postMessage: (message: unknown, transfer?: unknown[]) => {
          posted.push({ message: message as Record<string, unknown>, transfer });
        },
        onmessage: undefined as ((event: { data: unknown }) => void) | undefined | null,
      },
      disconnect: vi.fn(),
    };
    return { node, posted };
  }

  async function createNode(options: Record<string, unknown> = {}) {
    const { node, posted } = fakeNode();
    let captured: { name: string; options: AudioWorkletNodeOptions } | undefined;
    const analyzerNode = await SonareStreamAnalyzerNode.create(
      { sampleRate: 22050 } as BaseAudioContext,
      {
        config: CONFIG,
        chunkFrames: CHUNK,
        nodeFactory: (_ctx, name, nodeOptions) => {
          captured = { name, options: nodeOptions };
          return node as unknown as AudioWorkletNode;
        },
        ...options,
      },
    );
    const send = (samples: Float32Array, startSample: number, discontinuity = false) =>
      node.port.onmessage?.({ data: { type: 'chunk', samples, startSample, discontinuity } });
    return {
      analyzerNode,
      node,
      posted,
      send,
      captured: captured as { name: string; options: AudioWorkletNodeOptions },
    };
  }
  it('refuses an analyzer sampleRate that differs from the AudioContext before creating a node', async () => {
    const factory = vi.fn();
    await expect(
      SonareStreamAnalyzerNode.create({ sampleRate: 48000 } as BaseAudioContext, {
        config: CONFIG,
        chunkFrames: CHUNK,
        nodeFactory: factory as never,
      }),
    ).rejects.toThrow(RangeError);
    expect(factory).not.toHaveBeenCalled();
  });

  function signal(length: number, seed = 1): Float32Array {
    const out = new Float32Array(length);
    let state = seed;
    for (let i = 0; i < length; i++) {
      state = (state * 1664525 + 1013904223) >>> 0;
      out[i] = 0.3 * Math.sin(i * 0.05) + 0.1 * (state / 0xffffffff - 0.5);
    }
    return out;
  }

  function collect(frames: Array<{ rmsEnergy: Float32Array; timestamps: Float32Array }>) {
    return {
      rms: frames.flatMap((f) => Array.from(f.rmsEnergy)),
      timestamps: frames.flatMap((f) => Array.from(f.timestamps)),
    };
  }

  it('creates a zero-output sink that is not remixed by the browser', async () => {
    const { captured } = await createNode();
    expect(captured.name).toBe('sonare-stream-analyzer-processor');
    expect(captured.options).toMatchObject({
      numberOfInputs: 1,
      numberOfOutputs: 0,
      channelCountMode: 'max',
      channelInterpretation: 'discrete',
      processorOptions: { chunkFrames: CHUNK },
    });
    const custom = await createNode({ processorName: 'custom', chunkFrames: undefined });
    expect(custom.captured.name).toBe('custom');
    expect(custom.captured.options.processorOptions).toEqual({ chunkFrames: 4096 });
  });

  it('refuses an invalid option before building a node', async () => {
    await expect(createNode({ chunkFrames: 10 })).rejects.toThrow(RangeError);
    await expect(createNode({ config: { outputFormat: 1 } })).rejects.toThrow(TypeError);
  });

  it('resolves ready when the processor announces itself', async () => {
    const { analyzerNode, node } = await createNode();
    let resolved = false;
    analyzerNode.ready.then(() => {
      resolved = true;
    });
    await Promise.resolve();
    expect(resolved).toBe(false);
    node.port.onmessage?.({ data: { type: 'ready' } });
    await analyzerNode.ready;
    expect(resolved).toBe(true);
  });

  it('delivers the frames a direct StreamAnalyzer produces over the same samples', async () => {
    const { analyzerNode, send } = await createNode();
    const seen: Array<{ rmsEnergy: Float32Array; timestamps: Float32Array; mel: Float32Array }> =
      [];
    analyzerNode.onFrames((frames) => seen.push(frames));
    const samples = signal(CHUNK * 5);
    for (let i = 0; i < 5; i++) {
      send(samples.slice(i * CHUNK, (i + 1) * CHUNK), i * CHUNK);
    }

    const direct = new StreamAnalyzer(CONFIG);
    direct.process(samples);
    const expected = direct.readFrames(direct.availableFrames());
    direct.destroy();

    expect(expected.nFrames).toBeGreaterThan(10);
    const got = collect(seen);
    expect(got.rms).toEqual(Array.from(expected.rmsEnergy));
    expect(got.timestamps).toEqual(Array.from(expected.timestamps));
    expect(seen.flatMap((f) => Array.from(f.mel))).toEqual(Array.from(expected.mel));
  });

  it('returns every consumed chunk buffer to the processor', async () => {
    const { analyzerNode, posted, send } = await createNode();
    const chunk = signal(CHUNK);
    send(chunk, 0);
    expect(posted).toHaveLength(1);
    expect(posted[0].message).toMatchObject({ type: 'recycle', samples: chunk });
    expect(posted[0].transfer).toEqual([chunk.buffer]);
    analyzerNode.destroy();
  });

  it('resets the analyzer offset on a gap or a flagged discontinuity instead of splicing', async () => {
    const { analyzerNode, send } = await createNode();
    const seen: Array<{ rmsEnergy: Float32Array; timestamps: Float32Array }> = [];
    analyzerNode.onFrames((frames) => seen.push(frames));
    const a = signal(CHUNK * 2, 3);
    const b = signal(CHUNK, 4);
    const c = signal(CHUNK, 5);
    send(a.slice(0, CHUNK), 0);
    send(a.slice(CHUNK), CHUNK);
    // A dropped chunk: the offset jumps.
    expect(() => send(b, CHUNK * 5)).not.toThrow();
    // Contiguous offset, but flagged as restarted.
    send(c, CHUNK * 6, true);

    const direct = new StreamAnalyzer(CONFIG);
    const expected: Array<{ rmsEnergy: Float32Array; timestamps: Float32Array }> = [];
    const drain = () => expected.push(direct.readFrames(direct.availableFrames()));
    direct.processWithOffset(a.slice(0, CHUNK), 0);
    direct.processWithOffset(a.slice(CHUNK), CHUNK);
    drain();
    direct.reset(CHUNK * 5);
    direct.processWithOffset(b, CHUNK * 5);
    drain();
    direct.reset(CHUNK * 6);
    direct.processWithOffset(c, CHUNK * 6);
    drain();
    direct.destroy();

    expect(collect(seen)).toEqual(collect(expected));
    const gapFrames = seen.at(-2) as { timestamps: Float32Array };
    expect(gapFrames.timestamps[0]).toBeGreaterThanOrEqual((CHUNK * 5) / 22050 - 0.05);
  });

  it('stops delivering to a listener once it unsubscribes', async () => {
    const { analyzerNode, send } = await createNode();
    const first = vi.fn();
    const second = vi.fn();
    const stopFirst = analyzerNode.onFrames(first);
    analyzerNode.onFrames(second);
    send(signal(CHUNK), 0);
    expect(first).toHaveBeenCalledTimes(1);
    stopFirst();
    send(signal(CHUNK, 2), CHUNK);
    expect(first).toHaveBeenCalledTimes(1);
    expect(second).toHaveBeenCalledTimes(2);
    const stats = vi.fn();
    const stopStats = analyzerNode.onStats(stats);
    stopStats();
    send(signal(CHUNK, 3), CHUNK * 2);
    expect(stats).not.toHaveBeenCalled();
  });

  it('delivers statistics only when the estimate was re-estimated', async () => {
    const { analyzerNode, send } = await createNode({
      config: { ...CONFIG, keyUpdateIntervalSec: 0.2, bpmUpdateIntervalSec: 0.2 },
    });
    const delivered: boolean[] = [];
    analyzerNode.onStats((stats) => delivered.push(stats.estimate.updated));
    const samples = signal(22050 * 2);
    const chunks = Math.floor(samples.length / CHUNK);
    for (let i = 0; i < chunks; i++) {
      send(samples.slice(i * CHUNK, (i + 1) * CHUNK), i * CHUNK);
    }
    expect(delivered.length).toBeGreaterThan(0);
    expect(delivered.length).toBeLessThan(chunks);
    expect(delivered.every(Boolean)).toBe(true);
  });

  it('destroys idempotently, silences listeners and ignores late chunks', async () => {
    const { analyzerNode, node, posted, send } = await createNode();
    const frames = vi.fn();
    analyzerNode.onFrames(frames);
    analyzerNode.destroy();
    analyzerNode.destroy();
    expect(posted.filter((p) => p.message.type === 'destroy')).toHaveLength(1);
    expect(node.disconnect).toHaveBeenCalledTimes(1);
    expect(node.port.onmessage).toBeNull();
    send(signal(CHUNK), 0);
    expect(frames).not.toHaveBeenCalled();
  });
});
