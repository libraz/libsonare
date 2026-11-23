/**
 * Processors constructed in one AudioWorklet realm while its module is still being built share that
 * build, although each carries its own structured-clone copy of `wasmBinary`.
 */

import { describe, expect, it } from 'vitest';
import { registerSonareWorkletProcessor } from '../dist/worklet.js';

interface Posted {
  type: string;
  [key: string]: unknown;
}

describe('worklet module initialisation shared across processors', () => {
  it('starts every processor constructed while the first one is still building the module', async () => {
    const createModule = (await import('../dist/sonare.js')).default;
    let release: () => void = () => undefined;
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    let factoryCalls = 0;
    const scope = globalThis as unknown as Record<string, unknown>;
    scope.SonareEmbindModuleFactory = async () => {
      factoryCalls += 1;
      await gate;
      return createModule();
    };
    type Ctor = new (options: unknown) => { port: { posted: Posted[] } };
    const registered: { ctor?: Ctor } = {};
    Object.assign(scope, {
      AudioWorkletProcessor: class {
        port = {
          posted: [] as Posted[],
          postMessage(message: Posted) {
            this.posted.push(message);
          },
        };
      },
      registerProcessor: (_name: string, ctor: unknown) => {
        registered.ctor = ctor as Ctor;
      },
    });
    registerSonareWorkletProcessor('sonare-test-shared-init');
    const sceneJson = (await createModule()).mixingScenePresetJson('vocalReverbSend');
    const instances = [0, 1, 2].map(
      () =>
        new (registered.ctor as Ctor)({
          processorOptions: {
            sceneJson,
            sampleRate: 48000,
            maxBlockSize: 128,
            wasmBinary: new Uint8Array([0, 97, 115, 109]),
          },
        }),
    );
    release();
    const start = Date.now();
    while (instances.some((instance) => instance.port.posted.length === 0)) {
      if (Date.now() - start > 10000) {
        throw new Error('a processor never settled');
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    for (const instance of instances) {
      expect(instance.port.posted).toEqual([{ type: 'ready', runtimeTarget: 'embind' }]);
    }
    expect(factoryCalls).toBe(1);
  });
});
