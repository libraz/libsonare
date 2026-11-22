/**
 * The voice changer as an AudioWorklet node: `ready` carries the prepared
 * latency, `status()` answers from the live changer, a refused `setConfig`
 * arrives through `onError` with the main-thread changer's class and text, and
 * destroying the node rejects what is still pending.
 */

import { RealtimeVoiceChanger, realtimeVoiceChangerPresetConfig } from '../dist/index.js';
import {
  describe,
  expect,
  it,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  SonareRealtimeVoiceChangerNode,
  setupWorklet,
} from './_worklet_helpers';

const SAMPLE_RATE = 48000;
const PROCESSOR = 'sonare-test-voice-changer-node';

type ProcessorCtor = new (
  options: unknown,
) => {
  process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean;
};

interface Bridged {
  voice: SonareRealtimeVoiceChangerNode;
  nodeOptions: AudioWorkletNodeOptions | undefined;
  render: (blocks: number) => void;
  /** Lets every reply the processor queued reach the node. */
  settle: () => Promise<void>;
}

/** A node whose port reaches a registered processor in this realm, and whose replies come back. */
async function bridged(numChannels: 1 | 2 = 1): Promise<Bridged> {
  let deliver: ((event: { data: unknown }) => void) | undefined;
  let registered: ProcessorCtor | undefined;
  const port: {
    postMessage: (message: unknown) => void;
    onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  } = { postMessage: (message) => deliver?.({ data: message }) };
  Object.assign(globalThis, {
    AudioWorkletProcessor: class {
      port = {
        postMessage: (reply: unknown) =>
          queueMicrotask(() => port.onmessage?.({ data: reply } as MessageEvent<unknown>)),
        addEventListener: (_type: string, listener: (event: { data: unknown }) => void) => {
          deliver = listener;
        },
        start: () => undefined,
      };
    },
    registerProcessor: (_name: string, ctor: ProcessorCtor) => {
      registered = ctor;
    },
  });
  registerSonareRealtimeVoiceChangerWorkletProcessor(PROCESSOR);
  let nodeOptions: AudioWorkletNodeOptions | undefined;
  let processor: InstanceType<ProcessorCtor> | undefined;
  const context = { sampleRate: SAMPLE_RATE } as unknown as BaseAudioContext;
  const voice = await SonareRealtimeVoiceChangerNode.create(context, {
    numChannels,
    config: 'neutralMonitor',
    maxBlockSize: 128,
    processorName: PROCESSOR,
    nodeFactory: (_context, name, options) => {
      expect(name).toBe(PROCESSOR);
      nodeOptions = options;
      const node = { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      // The node installs its listener before the processor's ready reply is delivered.
      queueMicrotask(() => {
        processor = new (registered as ProcessorCtor)(options);
      });
      return node;
    },
  });
  const render = (blocks: number) => {
    for (let block = 0; block < blocks; block++) {
      const input = Array.from({ length: numChannels }, () => new Float32Array(128).fill(0.1));
      const output = Array.from({ length: numChannels }, () => new Float32Array(128));
      processor?.process([input], [output]);
    }
  };
  const settle = async () => {
    for (let i = 0; i < 4; i++) {
      await Promise.resolve();
    }
  };
  return { voice, nodeOptions, render, settle };
}

/** The error the main-thread changer throws for @p call on a neutral mono changer. */
function mainThreadRefusal(call: (main: RealtimeVoiceChanger) => void): Error {
  const main = new RealtimeVoiceChanger({
    sampleRate: SAMPLE_RATE,
    numChannels: 1,
    maxBlockSize: 128,
    config: 'neutralMonitor',
  });
  try {
    call(main);
  } catch (error) {
    return error as Error;
  } finally {
    main.destroy();
  }
  throw new Error('the main-thread changer accepted the call');
}

describe('SonareRealtimeVoiceChangerNode', () => {
  setupWorklet();

  it('asks for a voice input, a carrier input and an output of numChannels', async () => {
    const { voice, nodeOptions } = await bridged(2);
    try {
      expect(nodeOptions).toMatchObject({
        numberOfInputs: 2,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        channelCount: 2,
        channelCountMode: 'explicit',
        processorOptions: { sampleRate: SAMPLE_RATE, numChannels: 2, maxBlockSize: 128 },
      });
    } finally {
      await voice.destroy();
    }
  });

  it('carries the prepared latency on ready, and status answers from the live changer', async () => {
    const { voice, render } = await bridged();
    try {
      await voice.ready;
      const main = new RealtimeVoiceChanger({
        sampleRate: SAMPLE_RATE,
        numChannels: 1,
        maxBlockSize: 128,
        config: 'neutralMonitor',
      });
      try {
        expect(voice.latencySamples).toBe(main.latencySamples);
        render(4);
        const status = await voice.status();
        expect(status).toEqual({
          latencySamples: main.latencySamples,
          nonFiniteDiscardCount: 0,
          config: main.config(),
        });

        // A configuration the node sends is the one status reads back.
        const next = { ...main.config(), wetMix: 0.25 };
        voice.setConfig(next);
        expect((await voice.status()).config).toEqual(next);
      } finally {
        main.destroy();
      }
    } finally {
      await voice.destroy();
    }
  });

  it("reports a refused setConfig with the main-thread changer's class and text", async () => {
    const { voice, settle } = await bridged();
    const errors: Error[] = [];
    voice.onError((error) => errors.push(error));
    try {
      await voice.ready;
      const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
      const changes = [
        { formantMode: pod.formantMode === 'relative' ? 'absolute' : 'relative' },
        { voiceEngine: pod.voiceEngine === 'psola' ? 'grain' : 'psola' },
        { voiceEngineF0MinHz: pod.voiceEngineF0MinHz + 50 },
        { vocoderBands: pod.vocoderBands === 24 ? 20 : 24 },
        { wetMix: 2 },
      ] as const;
      const expected = changes.map((change) =>
        mainThreadRefusal((main) => main.setConfig({ ...pod, ...change } as typeof pod)),
      );
      for (const change of changes) {
        voice.setConfig({ ...pod, ...change } as typeof pod);
      }
      await settle();
      expect(errors).toHaveLength(changes.length);
      const body = (message: string) => message.replace(/^[\w.]+: /, '');
      errors.forEach((error, index) => {
        expect(error).toBeInstanceOf(expected[index].constructor);
        expect(body(error.message)).toBe(body(expected[index].message));
      });
      // The refusals left the configuration the processor started with.
      expect((await voice.status()).config).toEqual(pod);
    } finally {
      await voice.destroy();
    }
  });

  it('rejects a pending status read and refuses further calls once destroyed', async () => {
    const { voice } = await bridged();
    await voice.ready;
    const pending = voice.status();
    await voice.destroy();
    await expect(pending).rejects.toMatchObject({ codeName: 'InvalidState' });
    expect(() => voice.reset()).toThrow(/SonareRealtimeVoiceChangerNode/);
    await expect(voice.status()).rejects.toMatchObject({ codeName: 'InvalidState' });
    // A second destroy is the same promise.
    expect(voice.destroy()).toBe(voice.destroy());
  });

  it('refuses a channel count the processor cannot prepare, before creating a node', async () => {
    const context = { sampleRate: SAMPLE_RATE } as unknown as BaseAudioContext;
    let created = false;
    const nodeFactory = () => {
      created = true;
      return {} as AudioWorkletNode;
    };
    await expect(
      SonareRealtimeVoiceChangerNode.create(context, { numChannels: 3 as 1, nodeFactory }),
    ).rejects.toThrow(RangeError);
    await expect(
      SonareRealtimeVoiceChangerNode.create(context, {
        numChannels: '1' as unknown as 1,
        nodeFactory,
      }),
    ).rejects.toThrow(TypeError);
    await expect(
      SonareRealtimeVoiceChangerNode.create(context, {
        numChannels: 1,
        nodeFactory,
        meter: true,
      } as never),
    ).rejects.toThrow(TypeError);
    expect(created).toBe(false);
  });
});
