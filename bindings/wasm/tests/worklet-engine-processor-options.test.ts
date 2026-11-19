/**
 * The realtime engine and mixer worklets take a request object as `processorOptions`: an
 * undeclared key, including every retired name, is a TypeError naming the key, from the
 * main-thread node factory and from the processor constructor alike.
 */
import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import { init } from '../dist/index.js';
import {
  init as initWorklet,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareWorkletProcessor,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  SonareWorkletProcessor,
} from '../dist/worklet.js';
import { mixingScenePreset } from './_worklet_helpers';

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

const context = { sampleRate: 48000 } as BaseAudioContext;

describe('engine and mixer processorOptions refuse undeclared keys', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  afterEach(() => {
    delete scope.AudioWorkletProcessor;
    delete scope.registerProcessor;
  });

  describe('realtime engine processor', () => {
    const base = { sampleRate: 48000 };

    it.each([
      ['blockSize', 128],
      ['channelCount', 2],
      ['bufferSize', 4096],
    ])('the processor constructor refuses %s by name', (key, value) => {
      const Processor = registered(registerSonareRealtimeEngineWorkletProcessor, 'engine-test');
      expect(() => new Processor({ processorOptions: { ...base, [key]: value } })).toThrow(
        new TypeError(`unknown option '${key}'`),
      );
      expect(
        () =>
          new SonareRealtimeEngineWorkletProcessor({
            numChannels: 2,
            ...base,
            [key]: value,
          } as never),
      ).toThrow(new TypeError(`unknown option '${key}'`));
    });

    it('accepts the renamed maxBlockSize and numChannels', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        ...base,
        maxBlockSize: 128,
        numChannels: 2,
      });
      processor.destroy();
    });
  });

  describe('realtime engine node', () => {
    it.each([
      ['blockSize', 128],
      ['channelCount', 2],
      ['bufferSize', 4096],
    ])('creation refuses %s by name before any node exists', async (key, value) => {
      let nodes = 0;
      const nodeFactory = () => {
        nodes += 1;
        return { port: {} } as unknown as AudioWorkletNode;
      };
      await expect(
        SonareRealtimeEngineNode.create(context, {
          numChannels: 2,
          nodeFactory,
          [key]: value,
        } as never),
      ).rejects.toThrow(new TypeError(`SonareRealtimeEngineNode.create: unknown option '${key}'`));
      expect(nodes).toBe(0);
    });
  });

  describe('mixer processor', () => {
    const base = () => ({
      sceneJson: JSON.stringify(mixingScenePreset('vocalReverbSend')),
      sampleRate: 48000,
    });

    it('the processor constructor refuses an undeclared key by name', () => {
      const Processor = registered(registerSonareWorkletProcessor, 'mixer-test');
      expect(() => new Processor({ processorOptions: { ...base(), bufferSize: 4096 } })).toThrow(
        new TypeError("unknown option 'bufferSize'"),
      );
      expect(() => new SonareWorkletProcessor({ ...base(), bufferSize: 4096 } as never)).toThrow(
        new TypeError("unknown option 'bufferSize'"),
      );
    });

    it('accepts maxBlockSize', () => {
      const processor = new SonareWorkletProcessor({ ...base(), maxBlockSize: 128 });
      processor.destroy();
    });

    it('refuses the retired blockSize key by name', () => {
      expect(() => new SonareWorkletProcessor({ ...base(), blockSize: 128 } as never)).toThrow(
        new TypeError("unknown option 'blockSize'"),
      );
    });
  });
});
