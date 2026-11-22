/**
 * Parameter changes through the AudioWorklet engine: `SonareEngine.setParam`
 * takes the main-thread engine's smoothed and render-frame forms, the live
 * engine receives both, and `setParamSmoothingMs` reaches the worklet with the
 * main-thread engine's domain and refusals.
 */

import { describe, expect, it, vi } from 'vitest';
import { RealtimeEngine } from '../dist/index.js';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareEngineCommandType,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const PARAM_ID = 7;

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;
}

interface SetParameterHolder {
  setParameter: (...args: unknown[]) => void;
  setParamSmoothingMs: (smoothingMs: number) => void;
}

/** A facade whose port delivers each message to a live processor, with one registered parameter. */
async function bridgedEngine(): Promise<{
  engine: SonareEngine;
  live: SonareRealtimeEngineWorkletProcessor;
  liveEngine: SetParameterHolder;
  posted: Array<Record<string, unknown>>;
}> {
  const posted: Array<Record<string, unknown>> = [];
  const port: {
    postMessage: (message: unknown) => void;
    onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  } = {
    postMessage: (message) => {
      const typed = message as Record<string, unknown>;
      posted.push(typed);
      if (typeof typed.type === 'string' && typed.type in ENGINE_SYNC_MESSAGE_TYPES) {
        live.receiveSync(message as Parameters<typeof live.receiveSync>[0]);
      } else if (typeof typed.type === 'number') {
        live.receiveCommand(message as never);
      }
    },
  };
  const live: SonareRealtimeEngineWorkletProcessor = new SonareRealtimeEngineWorkletProcessor({
    sampleRate: 48000,
    maxBlockSize: 128,
    numChannels: 2,
    transport: {
      postMessage: (reply) => queueMicrotask(() => port.onmessage?.({ data: reply } as never)),
    },
  });
  const engine = await SonareEngine.create(fakeContext(), {
    numChannels: 2,
    mode: 'postMessage',
    nodeFactory: () => {
      queueMicrotask(() =>
        port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as never),
      );
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
  engine.addParameter({
    id: PARAM_ID,
    name: 'gain',
    unit: 'dB',
    minValue: -60,
    maxValue: 12,
    defaultValue: 0,
    rtSafe: true,
    defaultCurve: 'hold',
  });
  const liveEngine = (live as unknown as { engine: SetParameterHolder }).engine;
  return { engine, live, liveEngine, posted };
}

/** The message the main-thread engine throws for @p call, which the worklet facade must match. */
function mainThreadRefusal(call: (main: RealtimeEngine) => void): Error {
  const main = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    call(main);
  } catch (error) {
    return error as Error;
  } finally {
    main.destroy();
  }
  throw new Error('the main-thread engine accepted the call');
}

/** @p call throws an error of @p expected's class with @p expected's refusal text. */
function expectSameRefusal(call: () => void, expected: Error): void {
  let thrown: unknown;
  try {
    call();
  } catch (error) {
    thrown = error;
  }
  expect(thrown).toBeInstanceOf(expected.constructor);
  // Each entry names itself; the refusal after the name is the shared one.
  const body = (message: string) => message.replace(/^[\w.]+: /, '');
  expect(body((thrown as Error).message)).toBe(body(expected.message));
  expect((thrown as Error).message).toMatch(/^SonareEngine\./);
}

const commands = (posted: Array<Record<string, unknown>>) =>
  posted.filter((message) => typeof message.type === 'number');

describe('SonareEngine.setParam options', () => {
  setupWorklet();

  it('sends a plain change at the next block head and a smoothed one at its render frame', async () => {
    const { engine, live, liveEngine, posted } = await bridgedEngine();
    const applied = vi.spyOn(liveEngine, 'setParameter');
    try {
      posted.length = 0;
      engine.setParam('gain-node', 'gain', -6);
      engine.setParam('gain-node', PARAM_ID, -12, { smoothed: true, renderFrame: 256 });
      engine.setParam('gain-node', 'gain', -3, { renderFrame: 512 });
      expect(commands(posted)).toEqual([
        expect.objectContaining({
          type: SonareEngineCommandType.SetParam,
          targetId: PARAM_ID,
          renderFrame: -1,
          argFloat: -6,
        }),
        expect.objectContaining({
          type: SonareEngineCommandType.SetParamSmoothed,
          targetId: PARAM_ID,
          renderFrame: 256,
          argFloat: -12,
        }),
        expect.objectContaining({
          type: SonareEngineCommandType.SetParam,
          targetId: PARAM_ID,
          renderFrame: 512,
          argFloat: -3,
        }),
      ]);
      // The live engine receives each change with its frame and its smoothing.
      expect(applied.mock.calls).toEqual([
        [PARAM_ID, -6, undefined],
        [PARAM_ID, -12, 256, { smoothed: true }],
        [PARAM_ID, -3, 512],
      ]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it("refuses a bad render frame or option with the main-thread engine's class and text", async () => {
    const { engine, live, posted } = await bridgedEngine();
    try {
      posted.length = 0;
      const negative = mainThreadRefusal((main) => main.setParameter(PARAM_ID, 0, -1));
      expectSameRefusal(
        () => engine.setParam('gain-node', 'gain', 0, { renderFrame: -1 }),
        negative,
      );
      expect(negative).toBeInstanceOf(RangeError);
      const notANumber = mainThreadRefusal((main) =>
        main.setParameter(PARAM_ID, 0, '1' as unknown as number),
      );
      expectSameRefusal(
        () => engine.setParam('gain-node', 'gain', 0, { renderFrame: '1' as unknown as number }),
        notANumber,
      );
      expect(notANumber).toBeInstanceOf(TypeError);
      expect(() =>
        engine.setParam('gain-node', 'gain', 0, { smoothed: 1 as unknown as boolean }),
      ).toThrow(TypeError);
      expect(() =>
        engine.setParam('gain-node', 'gain', 0, { ramp: true } as unknown as { smoothed: boolean }),
      ).toThrow(TypeError);
      expect(commands(posted)).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });
});

describe('SonareEngine.setParamSmoothingMs', () => {
  setupWorklet();

  it('reaches the live engine, and refuses what the main-thread engine refuses', async () => {
    const { engine, live, liveEngine, posted } = await bridgedEngine();
    const applied = vi.spyOn(liveEngine, 'setParamSmoothingMs');
    try {
      posted.length = 0;
      engine.setParamSmoothingMs(5);
      engine.setParamSmoothingMs(0);
      expect(posted.filter((message) => message.type === 'syncParamSmoothing')).toEqual([
        expect.objectContaining({ type: 'syncParamSmoothing', smoothingMs: 5 }),
        expect.objectContaining({ type: 'syncParamSmoothing', smoothingMs: 0 }),
      ]);
      expect(applied.mock.calls).toEqual([[5], [0]]);

      posted.length = 0;
      const negative = mainThreadRefusal((main) => main.setParamSmoothingMs(-1));
      expect(negative).toBeInstanceOf(RangeError);
      expectSameRefusal(() => engine.setParamSmoothingMs(-1), negative);
      const notANumber = mainThreadRefusal((main) =>
        main.setParamSmoothingMs('5' as unknown as number),
      );
      expectSameRefusal(() => engine.setParamSmoothingMs('5' as unknown as number), notANumber);
      expect(posted.filter((message) => message.type === 'syncParamSmoothing')).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });
});
