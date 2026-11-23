/**
 * Every stock worklet registration shares one lifecycle: messages held before the module is ready
 * are bounded and an overflow fails the start visibly, and a render quantum that throws is reported,
 * answered with silence and ends the processor instead of escaping into the audio thread.
 */

import { afterEach, describe, expect, it, vi } from 'vitest';
import type { PlaybackRendererConfig } from '../dist/index.js';
import {
  registerSonarePlaybackWorkletProcessor,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareStreamAnalyzerWorkletProcessor,
  registerSonareWorkletProcessor,
  SonarePlaybackWorkletProcessor,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareStreamAnalyzerWorkletProcessor,
  SonareWorkletProcessor,
} from '../dist/worklet.js';

interface Posted {
  type: string;
  [key: string]: unknown;
}

interface Instance {
  port: { posted: Posted[]; onmessage?: (event: { data: unknown }) => void };
  process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean;
}

type Ctor = new (options: unknown) => Instance;

const SPEAKERS_5_1_AUTO: PlaybackRendererConfig = {
  input: { layout: 'auto' },
  target: { kind: 'speakers', layout: '5.1' },
};

const scope = globalThis as unknown as Record<string, unknown>;
const createModule = async () => (await import('../dist/sonare.js')).default();
let release: () => void = () => undefined;
const gate = new Promise<void>((resolve) => {
  release = resolve;
});
// The module stays uninitialised in this realm until the gate opens.
scope.SonareEmbindModuleFactory = async () => {
  await gate;
  return createModule();
};

function register(registerFn: (name: string) => void, name: string): Ctor {
  let registered: unknown;
  Object.assign(scope, {
    AudioWorkletProcessor: class {
      port: Instance['port'] = { posted: [] };
      constructor() {
        const port = this.port as Instance['port'] & { postMessage: (m: Posted) => void };
        port.postMessage = (message: Posted) => port.posted.push(message);
      }
    },
    registerProcessor: (_name: string, ctor: unknown) => {
      registered = ctor;
    },
  });
  registerFn(name);
  return registered as Ctor;
}

async function settled(instance: Instance): Promise<Posted> {
  const start = Date.now();
  for (;;) {
    const found = instance.port.posted.find((m) => m.type === 'ready' || m.type === 'error');
    if (found) {
      return found;
    }
    if (Date.now() - start > 10000) {
      throw new Error(`processor never settled: ${JSON.stringify(instance.port.posted)}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

const silentQuantum = (channels: number) =>
  Array.from({ length: channels }, () => new Float32Array(128).fill(1));

async function wirings() {
  const sceneJson = (await createModule()).mixingScenePresetJson('vocalReverbSend');
  return [
    {
      label: 'engine',
      bridge: SonareRealtimeEngineWorkletProcessor,
      ctor: register(registerSonareRealtimeEngineWorkletProcessor, 'lifecycle-engine'),
      options: { sampleRate: 48000, maxBlockSize: 128, numChannels: 2 },
      inputs: () => [] as Float32Array[][],
      outputs: () => [silentQuantum(2)],
    },
    {
      label: 'mixer',
      bridge: SonareWorkletProcessor,
      ctor: register(registerSonareWorkletProcessor, 'lifecycle-mixer'),
      options: { sceneJson, sampleRate: 48000, maxBlockSize: 128 },
      inputs: () => [silentQuantum(2)],
      outputs: () => [silentQuantum(2)],
    },
    {
      label: 'playback',
      bridge: SonarePlaybackWorkletProcessor,
      ctor: register(registerSonarePlaybackWorkletProcessor, 'lifecycle-playback'),
      options: { config: SPEAKERS_5_1_AUTO, sampleRate: 48000 },
      inputs: () => [silentQuantum(2)],
      outputs: () => [silentQuantum(6)],
    },
    {
      label: 'voice changer',
      bridge: SonareRealtimeVoiceChangerWorkletProcessor,
      ctor: register(registerSonareRealtimeVoiceChangerWorkletProcessor, 'lifecycle-voice'),
      options: { config: 'neutralMonitor', sampleRate: 48000, maxBlockSize: 128, numChannels: 1 },
      inputs: () => [silentQuantum(1)],
      outputs: () => [silentQuantum(1)],
    },
  ];
}

describe('stock worklet processor lifecycle', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('fails the start of a processor sent more messages than it holds before the module is ready', async () => {
    const all = await wirings();
    const overflowed = all.map((wiring) => {
      const instance = new wiring.ctor({ processorOptions: wiring.options });
      for (let i = 0; i < 1025; i++) {
        instance.port.onmessage?.({ data: { type: 'ping' } });
      }
      return { wiring, instance };
    });
    const held = new all[1].ctor({ processorOptions: all[1].options });
    for (let i = 0; i < 1024; i++) {
      held.port.onmessage?.({ data: { type: 'ping' } });
    }
    release();
    for (const { wiring, instance } of overflowed) {
      const outcome = await settled(instance);
      expect(outcome, wiring.label).toMatchObject({
        type: 'error',
        request: 'init',
        failure: expect.objectContaining({
          codeName: 'InvalidState',
          bare: 'AudioWorklet initialization message buffer overflowed.',
        }),
      });
      expect(
        instance.port.posted.some((m) => m.type === 'ready'),
        wiring.label,
      ).toBe(false);
      const out = wiring.outputs();
      expect(instance.process(wiring.inputs(), out), wiring.label).toBe(false);
      expect(
        out[0].every((channel) => channel.every((sample) => sample === 0)),
        wiring.label,
      ).toBe(true);
    }
    await settled(held);
    // The held control is started: its unknown messages are replayed and refused, then it is ready.
    const replayed = held.port.posted;
    expect(replayed.at(-1)).toEqual({ type: 'ready', runtimeTarget: 'embind' });
    expect(replayed.filter((m) => m.type === 'error' && m.request === 'invalid')).toHaveLength(
      1024,
    );
  });

  it('refuses a port message of a type the processor does not take, as a TypeError', async () => {
    const all = await wirings();
    const kinds: Record<string, string> = {
      mixer: 'mixer',
      playback: 'playback',
      'voice changer': 'voice changer',
    };
    const analyzer = {
      label: 'stream analyzer',
      ctor: register(registerSonareStreamAnalyzerWorkletProcessor, 'lifecycle-analyzer-unknown'),
      options: {},
    };
    kinds['stream analyzer'] = 'stream analyzer';
    for (const wiring of [...all.filter((w) => w.label !== 'engine'), analyzer]) {
      const instance = new wiring.ctor({ processorOptions: wiring.options });
      expect((await settled(instance)).type, wiring.label).toBe('ready');
      instance.port.onmessage?.({ data: { type: 'bogus' } });
      instance.port.onmessage?.({ data: 42 });
      const errors = instance.port.posted.filter((m) => m.type === 'error');
      expect(errors, wiring.label).toEqual(
        ['bogus', 'undefined'].map((type) => ({
          type: 'error',
          request: 'invalid',
          message: `Unrecognized ${kinds[wiring.label]} message: ${type}`,
          failure: expect.objectContaining({ name: 'TypeError' }),
        })),
      );
    }
  });

  it('reports a render quantum that throws, answers it with silence and ends the processor', async () => {
    const all = await wirings();
    const analyzer = {
      label: 'stream analyzer',
      bridge: SonareStreamAnalyzerWorkletProcessor,
      ctor: register(registerSonareStreamAnalyzerWorkletProcessor, 'lifecycle-analyzer'),
      options: {},
      inputs: () => [silentQuantum(1)],
      outputs: () => [silentQuantum(1)],
    };
    for (const wiring of [...all, analyzer]) {
      const instance = new wiring.ctor({ processorOptions: wiring.options });
      expect((await settled(instance)).type, wiring.label).toBe('ready');
      vi.spyOn(wiring.bridge.prototype, 'process').mockImplementation(() => {
        throw new RangeError('render failed');
      });
      const out = wiring.outputs();
      expect(instance.process(wiring.inputs(), out), wiring.label).toBe(false);
      expect(
        out[0].every((channel) => channel.every((sample) => sample === 0)),
        wiring.label,
      ).toBe(true);
      expect(instance.process(wiring.inputs(), wiring.outputs()), wiring.label).toBe(false);
      const errors = instance.port.posted.filter((m) => m.type === 'error');
      expect(errors, wiring.label).toEqual([
        {
          type: 'error',
          request: 'process',
          message: 'render failed',
          failure: expect.objectContaining({ name: 'RangeError', message: 'render failed' }),
        },
      ]);
      vi.restoreAllMocks();
    }
  });
});
