/**
 * A Program Change with no bank leaves `bank` out; `null` is not a spelling of
 * "no bank" and is refused by name, on the engine and on the worklet facade.
 */

import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import { SonareEngine, setupWorklet } from './_worklet_helpers';

setupWorklet();

const DEST = 7;
const TEXT = 'bank must be a { msb, lsb } object';

const fakeContext = {
  sampleRate: 48000,
  audioWorklet: { addModule: () => Promise.resolve() },
} as unknown as BaseAudioContext;

async function workletEngine(): Promise<SonareEngine> {
  return SonareEngine.create(fakeContext, {
    numChannels: 2,
    mode: 'postMessage',
    nodeFactory: () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      queueMicrotask(() => {
        port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent);
      });
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
}

describe('Program Change bank absence', () => {
  const cleanup: Array<() => void> = [];
  beforeAll(async () => {
    await init();
  });
  afterEach(() => {
    for (const release of cleanup.splice(0)) {
      release();
    }
  });

  it('RealtimeEngine refuses a null bank and accepts an omitted one', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    cleanup.push(() => engine.destroy());
    engine.setMidiInputSource(DEST);
    expect(() => engine.pushMidiProgram(DEST, 0, 0, 5, null as never)).toThrow(
      new TypeError(`RealtimeEngine.pushMidiProgram: ${TEXT}`),
    );
    expect(() => engine.pushMidiInputProgram(0, 0, 5, null as never)).toThrow(
      new TypeError(`RealtimeEngine.pushMidiInputProgram: ${TEXT}`),
    );
    expect(() => engine.pushMidiProgram(DEST, 0, 0, 5, undefined, 0)).not.toThrow();
    expect(() => engine.pushMidiInputProgram(0, 0, 5, undefined)).not.toThrow();
  });

  it('SonareEngine refuses a null bank and accepts an omitted one', async () => {
    const engine = await workletEngine();
    cleanup.push(() => engine.destroy());
    for (const call of [
      () => engine.pushMidiProgram(DEST, 0, 0, 5, null as never),
      () => engine.pushMidiInputProgram(0, 0, 5, null as never),
    ]) {
      expect(call).toThrow(TypeError);
      expect(call).toThrow(TEXT);
    }
    expect(() => engine.pushMidiProgram(DEST, 0, 0, 5)).not.toThrow();
  });
});
