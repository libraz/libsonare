/**
 * `processorOptions` is a request object: an undeclared key, including every retired name, is a
 * TypeError naming the key, from the main-thread factory and from the processor constructor alike.
 */
import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import { init } from '../dist/index.js';
import {
  init as initWorklet,
  registerSonarePlaybackWorkletProcessor,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareStreamAnalyzerWorkletProcessor,
  registerSonareWorkletProcessor,
  SonarePlaybackNode,
  SonarePlaybackWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareStreamAnalyzerNode,
  SonareStreamAnalyzerWorkletProcessor,
} from '../dist/worklet.js';
import { unknownKeyError } from './_helpers';

type Ctor = new (options: unknown) => object;

const scope = globalThis as unknown as Record<string, unknown>;

/** Registers a processor against a fake scope and returns the class the host would construct. */
function registered(register: (name: string) => void, name: string): Ctor {
  let ctor: unknown;
  Object.assign(scope, {
    AudioWorkletProcessor: class {
      port = {
        postMessage: () => undefined,
        addEventListener: () => undefined,
        start: () => undefined,
      };
    },
    registerProcessor: (_name: string, c: unknown) => {
      ctor = c;
    },
  });
  register(name);
  return ctor as Ctor;
}

const nodeFactory = () => ({ port: {} }) as unknown as AudioWorkletNode;
const context = { sampleRate: 48000 } as BaseAudioContext;

describe('processorOptions refuses undeclared keys', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  afterEach(() => {
    delete scope.AudioWorkletProcessor;
    delete scope.registerProcessor;
  });

  describe('voice changer', () => {
    const base = { config: 'neutralMonitor', sampleRate: 48000, numChannels: 1 };

    it.each([
      ['preset', 'neutralMonitor'],
      ['blockSize', 128],
      ['channelCount', 2],
    ])('the processor constructor refuses the retired %s by name', (key, value) => {
      const Processor = registered(registerSonareRealtimeVoiceChangerWorkletProcessor, 'vc-test');
      expect(() => new Processor({ processorOptions: { ...base, [key]: value } })).toThrow(
        unknownKeyError(`unknown key ${key}`),
      );
      expect(
        () => new SonareRealtimeVoiceChangerWorkletProcessor({ ...base, [key]: value } as never),
      ).toThrow(unknownKeyError(`unknown key ${key}`));
    });

    it('accepts every declared key', () => {
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        ...base,
        maxBlockSize: 128,
        numChannels: 2,
        config: 'neutralMonitor',
      });
      processor.destroy();
    });
  });

  describe('playback', () => {
    it.each([
      ['blockSize', 128],
      ['channelCount', 2],
    ])('the node factory and the processor refuse the retired %s by name', async (key, value) => {
      await expect(
        SonarePlaybackNode.create(context, { nodeFactory, [key]: value } as never),
      ).rejects.toThrow(unknownKeyError(`SonarePlaybackNode.create: unknown key ${key}`));
      const Processor = registered(registerSonarePlaybackWorkletProcessor, 'pb-test');
      expect(
        () => new Processor({ processorOptions: { sampleRate: 48000, [key]: value } }),
      ).toThrow(unknownKeyError(`unknown key ${key}`));
      expect(
        () => new SonarePlaybackWorkletProcessor({ sampleRate: 48000, [key]: value } as never),
      ).toThrow(unknownKeyError(`unknown key ${key}`));
    });

    it('refuses processorOptions that are not an object', () => {
      const Processor = registered(registerSonarePlaybackWorkletProcessor, 'pb-test');
      expect(() => new Processor({ processorOptions: 'config' })).toThrow(
        new TypeError('options must be an object'),
      );
    });
  });

  describe('stream analyzer', () => {
    it('the node factory and the processor refuse an undeclared key by name', async () => {
      await expect(
        SonareStreamAnalyzerNode.create(context, { nodeFactory, bufferSize: 4096 } as never),
      ).rejects.toThrow(unknownKeyError('SonareStreamAnalyzerNode.create: unknown key bufferSize'));
      const Processor = registered(registerSonareStreamAnalyzerWorkletProcessor, 'sa-test');
      expect(() => new Processor({ processorOptions: { bufferSize: 4096 } })).toThrow(
        unknownKeyError('unknown key bufferSize'),
      );
      expect(() => new SonareStreamAnalyzerWorkletProcessor({ bufferSize: 4096 } as never)).toThrow(
        unknownKeyError('unknown key bufferSize'),
      );
    });
  });

  describe('the port and transport a structured clone cannot carry', () => {
    it.each([
      ['playback', 'port', registerSonarePlaybackWorkletProcessor],
      ['stream analyzer', 'port', registerSonareStreamAnalyzerWorkletProcessor],
      ['mixer', 'transport', registerSonareWorkletProcessor],
      ['realtime engine', 'transport', registerSonareRealtimeEngineWorkletProcessor],
    ] as const)('the %s processorOptions refuse %s by name', (_label, key, register) => {
      const Processor = registered(register, 'carry-test');
      expect(() => new Processor({ processorOptions: { [key]: {} } })).toThrow(
        unknownKeyError(`unknown key ${key}`),
      );
    });

    it('the processor constructors take them as fields of their one options object', () => {
      const posted: unknown[] = [];
      const port = { postMessage: (message: unknown) => posted.push(message) };
      const analyzer = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128, port });
      expect(posted).toEqual([{ type: 'ready' }]);
      expect(analyzer).toBeInstanceOf(SonareStreamAnalyzerWorkletProcessor);
      expect(() => new SonareStreamAnalyzerWorkletProcessor({ transport: port } as never)).toThrow(
        unknownKeyError('unknown key transport'),
      );
      expect(
        () => new SonarePlaybackWorkletProcessor({ sampleRate: 48000, transport: port } as never),
      ).toThrow(unknownKeyError('unknown key transport'));
    });
  });
});
