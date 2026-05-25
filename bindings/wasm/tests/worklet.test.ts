import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';
import { registerSonareWorkletProcessor, SonareWorkletProcessor } from '../dist/worklet.js';

describe('SonareWorkletProcessor', () => {
  beforeAll(async () => {
    await init();
  });

  it('matches the offline mixer for one 128-sample render quantum', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const sceneJson = mixingScenePresetJson('vocalReverbSend');

    const offline = Mixer.fromSceneJson(sceneJson, sampleRate, blockSize);
    const processor = new SonareWorkletProcessor({ sceneJson, sampleRate, blockSize });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const alive = processor.process(
        [
          [vocalL, vocalR],
          [returnL, returnR],
        ],
        [[outL, outR]],
      );

      expect(alive).toBe(true);
      expect(Array.from(outL)).toEqual(Array.from(expected.left));
      expect(Array.from(outR)).toEqual(Array.from(expected.right));
    } finally {
      processor.destroy();
      offline.delete();
    }
  });

  it('keeps processing silence when an input strip is missing', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(processor.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(outL.length).toBe(blockSize);
      expect(outR.length).toBe(blockSize);
    } finally {
      processor.destroy();
    }
  });

  it('accepts postMessage-style automation and publishes meters', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const meters: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
      },
      { onMeter: (meter) => meters.push(meter) },
    );
    try {
      processor.receiveMessage({
        type: 'scheduleInsertAutomation',
        stripIndex: 0,
        insertIndex: 0,
        paramId: 0,
        samplePos: 0,
        value: 0,
        curve: 'linear',
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);

      expect(
        processor.process(
          [
            [vocalL, vocalR],
            [returnL, returnR],
          ],
          [[outL, outR]],
        ),
      ).toBe(true);
      expect(meters).toHaveLength(1);
      expect(meters[0]).toMatchObject({ type: 'meter', frame: blockSize });
    } finally {
      processor.destroy();
    }
  });

  it('routes messages and meters through a registered processor port', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {
          port = {
            posted: [] as unknown[],
            postMessage: (message: unknown) => {
              this.port.posted.push(message);
            },
          };
        },
        registerProcessor: (_name: string, ctor: unknown) => {
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor('sonare-test-processor');
      const Ctor = registeredCtor as new (options: {
        processorOptions: {
          sceneJson: string;
          sampleRate: number;
          blockSize: number;
          meterIntervalFrames: number;
        };
      }) => {
        port: { posted: unknown[]; onmessage?: (event: { data: unknown }) => void };
        process: (inputs: Float32Array[][], outputs: Float32Array[][][]) => boolean;
      };
      const blockSize = 128;
      const instance = new Ctor({
        processorOptions: {
          sceneJson: mixingScenePresetJson('vocalReverbSend'),
          sampleRate: 48000,
          blockSize,
          meterIntervalFrames: blockSize,
        },
      });
      instance.port.onmessage?.({
        data: {
          type: 'scheduleInsertAutomation',
          stripIndex: 0,
          insertIndex: 0,
          paramId: 0,
          value: 0,
        },
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1;
      vocalR[0] = 1;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(instance.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(instance.port.posted).toHaveLength(1);
      expect(instance.port.posted[0]).toMatchObject({ type: 'meter', frame: blockSize });
      instance.port.onmessage?.({ data: { type: 'destroy' } });
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });

  it('registers in an AudioWorklet-like global scope', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredName = '';
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {},
        registerProcessor: (name: string, ctor: unknown) => {
          registeredName = name;
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor();
      expect(registeredName).toBe('sonare-worklet-processor');
      expect(typeof registeredCtor).toBe('function');
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });
});
