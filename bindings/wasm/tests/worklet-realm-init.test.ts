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
    expect((failure as Error).message).toBe(
      'init: the module cannot be loaded inside an AudioWorkletGlobalScope; call ' +
        "init({ moduleFactory, wasmBinary }) with the default export of '@libraz/libsonare/module' " +
        "as moduleFactory, or, for a processor registered from '@libraz/libsonare/worklet', set " +
        'globalThis.SonareEmbindModuleFactory to that export and pass wasmBinary in its ' +
        'processorOptions',
    );
    expect(worklet.isInitialized()).toBe(false);
  });

  it("refuses the analysis entry's bundled loader too, naming its own factory", async () => {
    const analysis = await import('../dist/analysis.js');
    const failure = await analysis.init().then(
      () => undefined,
      (error: unknown) => error,
    );
    expect(failure).toMatchObject({ code: ErrorCode.NotSupported });
    expect((failure as Error).message).toBe(
      'init: the module cannot be loaded inside an AudioWorkletGlobalScope; call ' +
        "init({ moduleFactory, wasmBinary }) with the default export of '@libraz/libsonare/module/analysis' " +
        'as moduleFactory',
    );
    expect(analysis.isInitialized()).toBe(false);
  });

  it('has a processor with no factory post a refusal naming where the factory comes from', async () => {
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
    worklet.registerSonareWorkletProcessor('sonare-test-realm-no-factory');
    new (registered.ctor as Ctor)({
      processorOptions: { sceneJson: '{}', sampleRate: 48000, maxBlockSize: 128 },
    });
    const start = Date.now();
    while (!posted.some((message) => message.type === 'error')) {
      if (Date.now() - start > 10000) {
        throw new Error(`processor never refused: ${JSON.stringify(posted)}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    expect(posted.find((message) => message.type === 'error')?.message).toBe(
      'the AudioWorklet realm has no module; set globalThis.SonareEmbindModuleFactory to ' +
        "the default export of '@libraz/libsonare/module' before the processor is constructed, " +
        'and pass wasmBinary in its processorOptions',
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

  it('posts ready from a mixer processor constructed after another one initialised the realm', async () => {
    // Runs after the case above, which left this realm's module initialised.
    expect(worklet.isInitialized()).toBe(true);
    const createModule = (await import('../dist/sonare.js')).default;
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
    worklet.registerSonareWorkletProcessor('sonare-test-realm-second');
    const sceneJson = (await createModule()).mixingScenePresetJson('vocalReverbSend');
    new (registered.ctor as Ctor)({
      processorOptions: { sceneJson, sampleRate: 48000, maxBlockSize: 128 },
    });
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(posted).toEqual([{ type: 'ready', runtimeTarget: 'embind' }]);
  });
});
