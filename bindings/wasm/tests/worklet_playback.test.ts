import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, type PlaybackRendererConfig } from '../dist/index.js';
import {
  init as initWorklet,
  registerSonarePlaybackWorkletProcessor,
  SonarePlaybackNode,
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

/** A stand-in AudioWorkletNode recording what the wrapper posts. */
function fakeNode(posted: unknown[] = []) {
  return {
    port: {
      onmessage: null as ((event: MessageEvent) => void) | null,
      postMessage: (message: unknown) => posted.push(message),
    },
    disconnect: () => undefined,
  } as unknown as AudioWorkletNode & {
    port: { onmessage: ((event: MessageEvent) => void) | null };
  };
}

describe('SonarePlaybackWorkletProcessor', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('keeps its output shape while the input channel count changes', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor({
      config: SPEAKERS_5_1_AUTO,
      sampleRate: 48000,
      port,
    });
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
      expect(diagnostics.layoutSwitches).toBe(2);
      expect(diagnostics.unsupported_input_blocks).toBe(0);
      expect(Object.getPrototypeOf(diagnostics)).toBe(Object.prototype);
      expect(structuredClone(diagnostics)).toEqual(diagnostics);
    } finally {
      processor.destroy();
    }
  });

  it('renders silence correctly as the very first block, before any real block establishes a layout', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor({
      config: SPEAKERS_5_1_AUTO,
      sampleRate: 48000,
      port,
    });
    try {
      const output = outputBlock(6);
      // An empty inputs[0] (no channels connected yet) renders silence as the processor's very
      // first call, which must neither fix nor switch the automatic input layout.
      expect(processor.process([[]], [output])).toBe(true);
      for (const plane of output) {
        expect(plane.every((v) => v === 0)).toBe(true);
      }
      const diagnostics = requestDiagnostics(processor, port);
      expect(diagnostics.layoutSwitches).toBe(0);
    } finally {
      processor.destroy();
    }
  });

  it('renders an unsupported block as silence without shifting the timeline', () => {
    const port = capturePort();
    const processor = new SonarePlaybackWorkletProcessor({
      config: SPEAKERS_5_1_AUTO,
      sampleRate: 48000,
      port,
    });
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
      // The first block fixes the 5.1 layout; the refused block does not change it.
      expect(diagnostics.layoutSwitches).toBe(0);
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
    const processor = new SonarePlaybackWorkletProcessor({
      config: {},
      hrtf: hrtfBuffer(),
      sampleRate: 48000,
      port,
    });
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

      processor.receiveMessage({ type: 'orientation', yaw: Number.NaN, pitch: 0, roll: 0 });
      const orientationError = port.messages.at(-1);
      expect(orientationError?.type).toBe('error');
      expect(orientationError?.request).toBe('orientation');
      expect(String(orientationError?.message)).toMatch(/yawDeg/);
      port.messages.length = 0;

      // A rejected control message must leave the processor usable on the
      // following valid update and render quantum.
      processor.receiveMessage({ type: 'orientation', yaw: 15, pitch: -5, roll: 2 });
      const afterOrientationError = outputBlock(2);
      expect(processor.process([noiseBlock(6, 99)], [afterOrientationError])).toBe(true);
      expect(afterOrientationError.every((plane) => plane.every(Number.isFinite))).toBe(true);

      processor.receiveMessage({
        type: 'config',
        config: { nightMode: { amount: 1 } },
      });
      expect(port.messages.filter((m) => m.type === 'error')).toHaveLength(0);
      processor.receiveMessage({
        type: 'config',
        config: { target: { kind: 'speakers', layout: '5.1' } },
      });
      const error = port.messages.at(-1);
      expect(error?.type).toBe('error');
      expect(error?.request).toBe('config');
      expect(String(error?.message)).toMatch(/requires a new renderer/);

      processor.receiveMessage({ type: 'reset' });
      const diagnostics = requestDiagnostics(processor, port);
      expect(diagnostics.activeInputLayout).toBe('5.1');
    } finally {
      processor.destroy();
    }
    const messageCountAfterDestroy = port.messages.length;
    const errorCountAfterDestroy = port.messages.filter(
      (message) => message.type === 'error',
    ).length;
    expect(messageCountAfterDestroy).toBeGreaterThan(0);
    processor.receiveMessage({ type: 'orientation', yaw: Number.NaN, pitch: 0, roll: 0 });
    expect(port.messages).toHaveLength(messageCountAfterDestroy);
    expect(port.messages.filter((message) => message.type === 'error')).toHaveLength(
      errorCountAfterDestroy,
    );
    expect(processor.process([noiseBlock(2, 1)], [outputBlock(2)])).toBe(false);
  });

  it('forwards valid orientation after rejecting an update without partial state', () => {
    const expectedPort = capturePort();
    const rejectedPort = capturePort();
    const oldPosePort = capturePort();
    const expected = new SonarePlaybackWorkletProcessor({
      config: {},
      hrtf: hrtfBuffer(),
      sampleRate: 48000,
      port: expectedPort,
    });
    const rejected = new SonarePlaybackWorkletProcessor({
      config: {},
      hrtf: hrtfBuffer(),
      sampleRate: 48000,
      port: rejectedPort,
    });
    const oldPose = new SonarePlaybackWorkletProcessor({
      config: {},
      hrtf: hrtfBuffer(),
      sampleRate: 48000,
      port: oldPosePort,
    });
    try {
      const initialPose = { type: 'orientation' as const, yaw: 90, pitch: -20, roll: 10 };
      for (const processor of [expected, rejected, oldPose]) {
        processor.receiveMessage(initialPose);
      }

      let initialHeard = false;
      for (let block = 0; block < 16; block++) {
        const input = noiseBlock(2, block + 200);
        const expectedOutput = outputBlock(2);
        const rejectedOutput = outputBlock(2);
        const oldPoseOutput = outputBlock(2);
        expect(expected.process([input], [expectedOutput])).toBe(true);
        expect(rejected.process([input], [rejectedOutput])).toBe(true);
        expect(oldPose.process([input], [oldPoseOutput])).toBe(true);
        expect(rejectedOutput).toEqual(expectedOutput);
        initialHeard ||= oldPoseOutput.some((plane) => plane.some((value) => value !== 0));
      }
      expect(initialHeard).toBe(true);

      rejected.receiveMessage({ type: 'orientation', yaw: Number.NaN, pitch: 0, roll: 0 });
      expect(rejectedPort.messages).toHaveLength(1);
      expect(rejectedPort.messages[0]?.type).toBe('error');
      expect(rejectedPort.messages[0]?.request).toBe('orientation');
      expect(expectedPort.messages).toHaveLength(0);
      expect(oldPosePort.messages).toHaveLength(0);

      const newPose = { type: 'orientation' as const, yaw: -90, pitch: 25, roll: -15 };
      expected.receiveMessage(newPose);
      rejected.receiveMessage(newPose);
      let newPoseDiffersFromOld = false;
      let newPoseHeard = false;
      for (let block = 0; block < 16; block++) {
        const input = noiseBlock(2, block + 400);
        const expectedOutput = outputBlock(2);
        const rejectedOutput = outputBlock(2);
        const oldPoseOutput = outputBlock(2);
        expect(expected.process([input], [expectedOutput])).toBe(true);
        expect(rejected.process([input], [rejectedOutput])).toBe(true);
        expect(oldPose.process([input], [oldPoseOutput])).toBe(true);
        expect(rejectedOutput).toEqual(expectedOutput);
        newPoseHeard ||= expectedOutput.some((plane) => plane.some((value) => value !== 0));
        newPoseDiffersFromOld ||= expectedOutput.some((plane, channel) =>
          plane.some((value, frame) => value !== oldPoseOutput[channel][frame]),
        );
      }
      expect(newPoseHeard).toBe(true);
      expect(newPoseDiffersFromOld).toBe(true);
    } finally {
      expected.destroy();
      rejected.destroy();
      oldPose.destroy();
    }
  });

  it('refuses a headphones target without HRTF bytes', () => {
    expect(() => new SonarePlaybackWorkletProcessor({ config: {}, sampleRate: 48000 })).toThrow(
      /hrtf required/,
    );
  });

  it('refuses a serialized JSON string as the configuration', async () => {
    const context = { sampleRate: 44100 } as BaseAudioContext;
    const nodeFactory = () => fakeNode();
    const text = JSON.stringify(SPEAKERS_5_1_AUTO);
    await expect(
      SonarePlaybackNode.create(context, { config: text as never, nodeFactory }),
    ).rejects.toThrow(
      new TypeError('SonarePlaybackNode.create: config must be a configuration object'),
    );
    expect(
      () => new SonarePlaybackWorkletProcessor({ config: text as never, sampleRate: 48000 }),
    ).toThrow(/config must be a configuration object/);
  });

  it('creates a node whose input follows its source without browser remixing', async () => {
    const calls: Array<{ name: string; options: AudioWorkletNodeOptions }> = [];
    const nodeFactory = (
      _context: BaseAudioContext,
      name: string,
      options: AudioWorkletNodeOptions,
    ) => {
      calls.push({ name, options });
      return fakeNode();
    };
    const context = { sampleRate: 44100 } as BaseAudioContext;
    const surround = await SonarePlaybackNode.create(context, {
      config: SPEAKERS_5_1_AUTO,
      nodeFactory,
    });
    const hrtf = hrtfBuffer();
    await SonarePlaybackNode.create(context, { hrtf, nodeFactory, processorName: 'custom' });

    expect(surround.node).toBeDefined();
    expect(calls[0].name).toBe('sonare-playback-processor');
    expect(calls[0].options).toMatchObject({
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [6],
      channelCountMode: 'max',
      channelInterpretation: 'discrete',
    });
    expect(calls[0].options.processorOptions.sampleRate).toBe(44100);
    expect(calls[0].options.processorOptions.config).toEqual(SPEAKERS_5_1_AUTO);
    expect(calls[1].name).toBe('custom');
    expect(calls[1].options.outputChannelCount).toEqual([2]);
    expect(calls[1].options.processorOptions.hrtf).toBe(hrtf);

    // An explicit rate must be the context's: nothing resamples its quanta.
    await SonarePlaybackNode.create(context, { nodeFactory, sampleRate: 44100 });
    expect(calls).toHaveLength(3);
    await expect(
      SonarePlaybackNode.create(context, { nodeFactory, sampleRate: 48000 }),
    ).rejects.toThrow(RangeError);
    expect(calls).toHaveLength(3);
  });

  it('sizes the output from the configuration as the core reads it, refusing an unknown layout', async () => {
    const counts: number[] = [];
    const nodeFactory = (
      _context: BaseAudioContext,
      _name: string,
      options: AudioWorkletNodeOptions,
    ) => {
      counts.push(options.outputChannelCount?.[0] ?? -1);
      return fakeNode();
    };
    const context = { sampleRate: 48000 } as BaseAudioContext;
    for (const layout of ['stereo', '5.1', '7.1'] as const) {
      await SonarePlaybackNode.create(context, {
        config: { target: { kind: 'speakers', layout } },
        nodeFactory,
      });
    }
    expect(counts).toEqual([2, 6, 8]);
    await expect(
      SonarePlaybackNode.create(context, {
        config: { target: { kind: 'speakers', layout: '9.1' as never } },
        nodeFactory,
      }),
    ).rejects.toThrow(expect.objectContaining({ code: ErrorCode.InvalidFormat }));
    expect(counts).toHaveLength(3);
  });

  it('drives the processor through its methods and releases it once', async () => {
    const posted: unknown[] = [];
    const node = fakeNode(posted);
    const playback = await SonarePlaybackNode.create({ sampleRate: 48000 } as BaseAudioContext, {
      config: SPEAKERS_5_1_AUTO,
      nodeFactory: () => node,
    });
    playback.setHeadOrientation(30);
    playback.reset();
    const diagnostics = playback.diagnostics();
    node.port.onmessage?.({
      data: { type: 'diagnostics', diagnostics: { unsupported_input_blocks: 3 } },
    } as MessageEvent);
    await expect(diagnostics).resolves.toMatchObject({ unsupported_input_blocks: 3 });
    expect(posted).toEqual([
      { type: 'orientation', yaw: 30, pitch: 0, roll: 0 },
      { type: 'reset' },
      { type: 'diagnostics' },
    ]);
    const pending = playback.diagnostics();
    await playback.destroy();
    await expect(pending).rejects.toMatchObject({ code: ErrorCode.InvalidState });
    expect(() => playback.reset()).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    expect(posted.at(-1)).toEqual({ type: 'destroy' });
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
      // An already initialized module starts the processor synchronously, and it says so.
      expect(posted).toEqual([{ type: 'ready', runtimeTarget: 'embind' }]);
      expect(instance.process([noiseBlock(2, 3)], [outputBlock(6)])).toBe(true);
      listener?.({ data: { type: 'diagnostics' } });
      listener?.({ data: { type: 'unknown' } });
      expect(posted).toHaveLength(2);
      expect((posted[1] as Posted).type).toBe('diagnostics');
      listener?.({ data: { type: 'destroy' } });
    } finally {
      delete scope.AudioWorkletProcessor;
      delete scope.registerProcessor;
    }
  });
});
