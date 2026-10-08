/**
 * Bus fader automation id lifetimes on the WASM RealtimeEngine and across the
 * worklet facade. Mirrors the C ABI case in tests/api/sonare_c_engine_strip_test.cpp.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../dist/index.js';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareEngineCommandType,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const kBlock = 256;
const kFrames = kBlock * 80;

/** The engine `SonareEngine.create` accepts as its offline mirror. */
type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

function alive(engine: RealtimeEngine, id: number): boolean {
  try {
    engine.parameterInfo(id);
    return true;
  } catch {
    return false;
  }
}

function settled(engine: RealtimeEngine, blocks = 30): number {
  let last = 0;
  for (let block = 0; block < blocks; block += 1) {
    const [chunk] = engine.process([new Float32Array(kBlock)]);
    last = chunk[chunk.length - 1];
  }
  return last;
}

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: {
      addModule(): Promise<void> {
        return Promise.resolve();
      },
    },
  } as unknown as BaseAudioContext;
}

function readyWorkletNode(port: {
  onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  [member: string]: unknown;
}): AudioWorkletNode {
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

describe('bus automation ids (WASM)', () => {
  setupWorklet();

  it('keeps a fader id on its bus across a reorder and retires it with the bus', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      // Only track 20 sounds and it feeds bus 200, so the output reads bus 200's fader alone.
      engine.setClips([
        {
          trackId: 20,
          channels: [new Float32Array(kFrames).fill(1)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ]);
      engine.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
      engine.setTrackLanes([{ trackId: 20, outputBusId: 200 }]);
      const fader100 = engine.resolveBusAutomationId(100, 'faderDb');
      const fader200 = engine.resolveBusAutomationId(200, 'faderDb');
      expect(fader100).toBeGreaterThan(0);
      expect(fader200).not.toBe(fader100);
      expect(engine.resolveBusAutomationId(300, 'faderDb')).toBe(-1);
      expect(engine.resolveBusAutomationId(100, 'pan' as 'faderDb')).toBe(-1);

      engine.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
      expect(engine.resolveBusAutomationId(200, 'faderDb')).toBe(fader200);
      engine.setParameter(fader200, -60);
      engine.play();
      expect(settled(engine)).toBeCloseTo(0.001, 4);

      engine.setParameter(fader200, 0);
      engine.setTrackBuses([{ busId: 200 }]);
      expect(alive(engine, fader100)).toBe(false);
      engine.setParameter(fader100, -60);
      expect(settled(engine)).toBeCloseTo(1, 3);
    } finally {
      engine.destroy();
    }
  });

  it('numbers bus fader ids on the mirror as the worklet engine does', async () => {
    const posted: unknown[] = [];
    const offline = new RealtimeEngine(48000, kBlock);
    const engine = await SonareEngine.create(fakeContext(), {
      mode: 'postMessage',
      offlineEngine: offline as unknown as OfflineEngineOption,
      offlineChannelCount: 1,
      nodeFactory: () =>
        readyWorkletNode({
          postMessage: (message: unknown) => posted.push(message),
          onmessage: undefined,
        }),
    });
    const live = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: 48000, blockSize: kBlock, channelCount: 1 },
      { postMessage: () => undefined },
    );
    const liveEngine = (live as unknown as { engine: RealtimeEngine }).engine;
    const flush = () => {
      for (const message of posted.splice(0)) {
        const type = (message as { type?: unknown }).type;
        if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
          live.receiveSync(message as Parameters<typeof live.receiveSync>[0]);
        }
      }
    };
    try {
      const fader100 = engine.busAutomationParamId(100);
      const fader200 = engine.busAutomationParamId(200);
      engine.setTrackLanes([{ trackId: 8, outputBusId: 200 }]);
      flush();
      expect(fader200).not.toBe(fader100);
      expect(liveEngine.resolveBusAutomationId(100, 'faderDb')).toBe(fader100);
      expect(liveEngine.resolveBusAutomationId(200, 'faderDb')).toBe(fader200);

      // setBusGain sends the resolved id, so it drives bus 200 on the worklet engine.
      expect(engine.setBusGain(200, -12)).toBe(true);
      const sent = posted.filter(
        (message) =>
          (message as { type?: unknown }).type === SonareEngineCommandType.SetParamSmoothed,
      );
      expect(sent).toEqual([expect.objectContaining({ targetId: fader200, argFloat: -12 })]);

      // Only track 8 sounds, into bus 200.
      const clips = [
        {
          id: 1,
          trackId: 8,
          channels: [new Float32Array(kFrames).fill(0.25)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ];
      live.receiveSync({ type: 'syncClips', clips });
      live.receiveCommand({
        type: SonareEngineCommandType.SetParamSmoothed,
        targetId: fader200,
        sampleTime: -1,
        argFloat: -12,
      });
      live.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
      let actual = 0;
      for (let block = 0; block < 30; block += 1) {
        const output = [new Float32Array(kBlock)];
        expect(live.process([[]], [output])).toBe(true);
        actual = output[0][kBlock - 1];
      }
      expect(actual).toBeGreaterThan(0.25 * 0.2);
      expect(actual).toBeLessThan(0.25 * 0.3);
    } finally {
      live.destroy();
      engine.destroy();
    }
  });
});
