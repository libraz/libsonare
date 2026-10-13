/**
 * The mixer processor initializes the WASM module itself in a worklet realm
 * where it is not initialized yet, through the same recipe as the realtime
 * engine processor: `wasmBinary` plus `globalThis.SonareEmbindModuleFactory`.
 */

import { describe, expect, it } from 'vitest';
import { registerSonareWorkletProcessor } from '../dist/worklet.js';

interface Posted {
  type: string;
  [key: string]: unknown;
}

describe('mixer worklet embind initialization', () => {
  it('builds the module from the global factory, renders silence until ready, then mixes', async () => {
    const createModule = (await import('../dist/sonare.js')).default;
    const wasmBinary = new Uint8Array([0, 97, 115, 109]);
    const factoryCalls: { wasmBinary?: unknown }[] = [];
    const scope = globalThis as unknown as Record<string, unknown>;
    scope.SonareEmbindModuleFactory = (options?: { wasmBinary?: unknown }) => {
      factoryCalls.push({ wasmBinary: options?.wasmBinary });
      return createModule();
    };
    const posted: Posted[] = [];
    type Ctor = new (
      options: unknown,
    ) => {
      process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean;
    };
    const registered: { ctor?: Ctor } = {};
    Object.assign(globalThis, {
      AudioWorkletProcessor: class {
        port = { postMessage: (message: Posted) => posted.push(message) };
      },
      registerProcessor: (_name: string, ctor: unknown) => {
        registered.ctor = ctor as Ctor;
      },
    });
    registerSonareWorkletProcessor('sonare-test-mixer');
    const sceneJson = (await createModule()).mixingScenePresetJson('vocalReverbSend');
    const instance = new (registered.ctor as Ctor)({
      processorOptions: { sceneJson, sampleRate: 48000, blockSize: 128, wasmBinary },
    });

    const input = [new Float32Array(128).fill(0.25), new Float32Array(128).fill(0.25)];
    const out = [new Float32Array(128).fill(1), new Float32Array(128).fill(1)];
    expect(instance.process([input], [out])).toBe(true);
    expect(out[0].every((sample) => sample === 0)).toBe(true);

    const start = Date.now();
    while (!posted.some((message) => message.type === 'ready')) {
      if (Date.now() - start > 10000) {
        throw new Error(`mixer processor never became ready: ${JSON.stringify(posted)}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    expect(factoryCalls).toEqual([{ wasmBinary }]);
    expect(instance.process([input], [out])).toBe(true);
    expect(out[0].some((sample) => sample !== 0)).toBe(true);
  });
});
