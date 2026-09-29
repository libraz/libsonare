import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, type PlaybackRendererConfig } from '../dist/index.js';
import {
  createSonarePlaybackNode,
  init as initWorklet,
  registerSonarePlaybackWorkletProcessor,
  SonarePlaybackWorkletProcessor,
} from '../dist/worklet.js';

const QUANTUM = 128;
const SPEAKERS_5_1_AUTO: PlaybackRendererConfig = {
  input: { layout: 'auto' },
  target: { kind: 'speakers', layout: '5.1' },
};

function hrtfBuffer(): ArrayBuffer {
  const bytes = readFileSync(new URL('../dist/hrtf/default.shrf', import.meta.url));
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function noiseBlock(channels: number, seed: number): Float32Array[] {
  let state = seed >>> 0;
  return Array.from({ length: channels }, () => {
    const plane = new Float32Array(QUANTUM);
    for (let i = 0; i < QUANTUM; i++) {
      state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
      plane[i] = 0.1 * (state / 2 ** 31 - 1);
    }
    return plane;
  });
}

function outputBlock(channels: number): Float32Array[] {
  return Array.from({ length: channels }, () => new Float32Array(QUANTUM));
}

interface Posted {
  type: string;
  [key: string]: unknown;
}

function capturePort(): { postMessage: (message: unknown) => void; messages: Posted[] } {
  const messages: Posted[] = [];
  return { messages, postMessage: (message: unknown) => messages.push(message as Posted) };
}

function requestDiagnostics(
  processor: InstanceType<typeof SonarePlaybackWorkletProcessor>,
  port: ReturnType<typeof capturePort>,
): Record<string, unknown> {
  processor.receiveMessage({ type: 'diagnostics' });
  const reply = port.messages.at(-1);
  expect(reply?.type).toBe('diagnostics');
  return reply?.diagnostics as Record<string, unknown>;
}

describe('SonarePlaybackWorkletProcessor', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('keeps its output shape while the input channel count changes', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor(
      { config: SPEAKERS_5_1_AUTO, sampleRate: 48000 },
      port,
    );
    try {
      const sequence = [2, 6, 0, 6, 2];
      let seed = 1;
      let heard = false;
      for (const channels of sequence) {
        for (let block = 0; block < 12; block++) {
          const output = outputBlock(6);
          expect(processor.process([noiseBlock(channels, seed++)], [output])).toBe(true);
          expect(output).toHaveLength(6);
          for (const plane of output) {
            expect(plane.every(Number.isFinite)).toBe(true);
            heard ||= plane.some((v) => v !== 0);
          }
        }
      }
      expect(heard).toBe(true);
      const diagnostics = requestDiagnostics(processor, port);
      // 2 -> 6 and 6 -> 2; the empty blocks render silence on the 5.1 layout.
      expect(diagnostics.layout_switches).toBe(2);
      expect(diagnostics.unsupported_input_blocks).toBe(0);
      expect(Object.getPrototypeOf(diagnostics)).toBe(Object.prototype);
      expect(structuredClone(diagnostics)).toEqual(diagnostics);
    } finally {
      processor.destroy();
    }
  });

  it('renders silence correctly as the very first block, before any real block establishes a layout', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor(
      { config: SPEAKERS_5_1_AUTO, sampleRate: 48000 },
      port,
    );
    try {
      const output = outputBlock(6);
      // An empty inputs[0] (no channels connected yet) maps to
      // processPreparedSilence -- exercised here as the processor's very
      // first call, before any processPrepared call has cached an input
      // channel count, so it must fall back to the same default the C ABI
      // documents (2 channels) rather than reading uninitialized state.
      expect(processor.process([[]], [output])).toBe(true);
      for (const plane of output) {
        expect(plane.every((v) => v === 0)).toBe(true);
      }
      const diagnostics = requestDiagnostics(processor, port);
      expect(diagnostics.layout_switches).toBe(0);
    } finally {
      processor.destroy();
    }
  });

  it('renders an unsupported block as silence without shifting the timeline', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor(
      { config: SPEAKERS_5_1_AUTO, sampleRate: 48000 },
      port,
    );
    try {
      const latency = (requestDiagnostics(processor, port).latency as { samples: number }).samples;
      const impulseBlock = 4;
      const impulseOffset = 17;
      const blocks = impulseBlock + Math.ceil(latency / QUANTUM) + 3;
      const center: number[] = [];
      for (let block = 0; block < blocks; block++) {
        const input = block === 2 ? noiseBlock(3, 9) : outputBlock(6);
        if (block === impulseBlock) {
          input[2][impulseOffset] = 0.5;
        }
        const output = outputBlock(6);
        processor.process([input], [output]);
        center.push(...output[2]);
      }
      const diagnostics = requestDiagnostics(processor, port);
      expect(diagnostics.unsupported_input_blocks).toBe(1);
      expect(diagnostics.layout_switches).toBe(1);
      const peak = Math.max(...center.map(Math.abs));
      const rise = center.findIndex((v) => Math.abs(v) >= peak * 0.01);
      expect(
        Math.abs(rise - (impulseBlock * QUANTUM + impulseOffset + latency)),
      ).toBeLessThanOrEqual(1);
    } finally {
      processor.destroy();
    }
  });

  it('renders binaural output from HRTF bytes and answers its control messages', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor({ config: {}, hrtf: hrtfBuffer() }, port);
    try {
      let heard = false;
      for (let block = 0; block < 16; block++) {
        processor.receiveMessage({ type: 'orientation', yaw: block * 3, pitch: 0, roll: 0 });
        const output = outputBlock(2);
        processor.process([noiseBlock(6, block + 1)], [output]);
        for (const plane of output) {
          expect(plane.every(Number.isFinite)).toBe(true);
          heard ||= plane.some((v) => v !== 0);
        }
      }
      expect(heard).toBe(true);

      processor.receiveMessage({
        type: 'config',
        config: JSON.stringify({ night_mode: { amount: 1 } }),
      });
      expect(port.messages.filter((m) => m.type === 'error')).toHaveLength(0);
      processor.receiveMessage({
        type: 'config',
        config: JSON.stringify({ target: { kind: 'speakers', layout: '5.1' } }),
      });
      const error = port.messages.at(-1);
      expect(error?.type).toBe('error');
      expect(error?.request).toBe('config');
      expect(String(error?.message)).toMatch(/requires a new renderer/);

      processor.receiveMessage({ type: 'reset' });
      const diagnostics = requestDiagnostics(processor, port);
      expect(diagnostics.active_input_layout).toBe('5.1');
    } finally {
      processor.destroy();
    }
    expect(processor.process([noiseBlock(2, 1)], [outputBlock(2)])).toBe(false);
  });

  it('refuses a headphones target without HRTF bytes', () => {
    expect(() => new SonarePlaybackWorkletProcessor({ config: {} })).toThrow(/hrtf required/);
  });

  it('creates a node whose input follows its source without browser remixing', () => {
    const calls: Array<{ name: string; options: AudioWorkletNodeOptions }> = [];
    const nodeFactory = (
      _context: BaseAudioContext,
      name: string,
      options: AudioWorkletNodeOptions,
    ) => {
      calls.push({ name, options });
      return {} as AudioWorkletNode;
    };
    const context = { sampleRate: 44100 } as BaseAudioContext;
    createSonarePlaybackNode(context, { config: SPEAKERS_5_1_AUTO, nodeFactory });
    const hrtf = hrtfBuffer();
    createSonarePlaybackNode(context, { hrtf, nodeFactory, processorName: 'custom' });

    expect(calls[0].name).toBe('sonare-playback-processor');
    expect(calls[0].options).toMatchObject({
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [6],
      channelCountMode: 'max',
      channelInterpretation: 'discrete',
    });
    expect(calls[0].options.processorOptions.sampleRate).toBe(44100);
    expect(typeof calls[0].options.processorOptions.config).toBe('string');
    expect(calls[1].name).toBe('custom');
    expect(calls[1].options.outputChannelCount).toEqual([2]);
    expect(calls[1].options.processorOptions.hrtf).toBe(hrtf);
  });

  it('registers a processor that routes port messages', () => {
    const registered: Record<
      string,
      new (
        options?: unknown,
      ) => { process: typeof SonarePlaybackWorkletProcessor.prototype.process }
    > = {};
    const posted: unknown[] = [];
    const scope = globalThis as unknown as Record<string, unknown>;
    let listener: ((event: { data: unknown }) => void) | undefined;
    scope.AudioWorkletProcessor = class {
      port = {
        postMessage: (message: unknown) => posted.push(message),
        addEventListener: (_type: string, callback: (event: { data: unknown }) => void) => {
          listener = callback;
        },
      };
    };
    scope.registerProcessor = (name: string, ctor: never) => {
      registered[name] = ctor;
    };
    try {
      registerSonarePlaybackWorkletProcessor();
      const Ctor = registered['sonare-playback-processor'];
      const instance = new Ctor({
        processorOptions: { config: SPEAKERS_5_1_AUTO, sampleRate: 48000 },
      });
      expect(instance.process([noiseBlock(2, 3)], [outputBlock(6)])).toBe(true);
      listener?.({ data: { type: 'diagnostics' } });
      listener?.({ data: { type: 'unknown' } });
      expect(posted).toHaveLength(1);
      expect((posted[0] as Posted).type).toBe('diagnostics');
      listener?.({ data: { type: 'destroy' } });
    } finally {
      delete scope.AudioWorkletProcessor;
      delete scope.registerProcessor;
    }
  });
});
