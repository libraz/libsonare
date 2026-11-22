/**
 * `init()` inside an AudioWorkletGlobalScope.
 *
 * The realm cannot fetch, so the bundled loader is refused by name rather than left to fail
 * obscurely; the embind factory recipe a registered processor follows still initialises. This
 * file runs in its own realm, with a stand-in for the worklet global installed before anything
 * loads the module.
 */

import { afterAll, describe, expect, it } from 'vitest';
import * as worklet from '../dist/worklet.js';
import { ErrorCode } from '../src/errors';

interface Posted {
  type: string;
  [key: string]: unknown;
}

const scope = globalThis as unknown as Record<string, unknown>;
scope.AudioWorkletGlobalScope = class AudioWorkletGlobalScope {};

describe('init() in an AudioWorkletGlobalScope', () => {
  afterAll(() => {
    for (const name of [
      'AudioWorkletGlobalScope',
      'AudioWorkletProcessor',
      'registerProcessor',
      'SonareEmbindModuleFactory',
    ]) {
      delete scope[name];
    }
  });

  it('refuses the bundled loader with NotSupported, naming the supported recipe', async () => {
    const failure = await worklet.init().then(
      () => undefined,
      (error: unknown) => error,
    );
    expect(failure).toMatchObject({ code: ErrorCode.NotSupported });
    expect((failure as Error).message).toMatch(
      /^init: the module cannot be loaded inside an AudioWorkletGlobalScope; set globalThis\.SonareEmbindModuleFactory .*registerSonareWorkletProcessor/,
    );
    expect(worklet.isInitialized()).toBe(false);
  });

  it('initialises through the embind factory recipe', async () => {
    const createModule = (await import('../dist/sonare.js')).default;
    scope.SonareEmbindModuleFactory = () => createModule();
    const posted: Posted[] = [];
    type Ctor = new (options: unknown) => unknown;
    const registered: { ctor?: Ctor } = {};
    Object.assign(scope, {
      AudioWorkletProcessor: class {
        port = { postMessage: (message: Posted) => posted.push(message) };
      },
      registerProcessor: (_name: string, ctor: unknown) => {
        registered.ctor = ctor as Ctor;
      },
    });
    worklet.registerSonareWorkletProcessor('sonare-test-realm-init');
    const sceneJson = (await createModule()).mixingScenePresetJson('vocalReverbSend');
    new (registered.ctor as Ctor)({
      processorOptions: { sceneJson, sampleRate: 48000, maxBlockSize: 128 },
    });
    const start = Date.now();
    while (!posted.some((message) => message.type === 'ready' || message.type === 'error')) {
      if (Date.now() - start > 10000) {
        throw new Error(`processor never answered: ${JSON.stringify(posted)}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    expect(posted.find((message) => message.type === 'error')).toBeUndefined();
    expect(worklet.isInitialized()).toBe(true);
  });
});
