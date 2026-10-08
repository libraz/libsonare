/**
 * Track fader/pan and instrument automation id lifetimes on the WASM
 * RealtimeEngine and across the worklet facade. Mirrors the C ABI case in
 * tests/api/sonare_c_engine_strip_test.cpp.
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

describe('track lane automation ids (WASM)', () => {
  setupWorklet();

  it('keeps a fader id on its track across a reorder and retires it with the track', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      // Only track 20 sounds, so the output reads track 20's fader alone.
      engine.setClips([
        {
          trackId: 20,
          channels: [new Float32Array(kFrames).fill(1)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ]);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      const fader10 = engine.resolveTrackLaneAutomationId(10, 'faderDb');
      expect(fader10).toBeGreaterThan(0);
      expect(engine.resolveTrackLaneAutomationId(10, 'pan')).not.toBe(fader10);
      expect(engine.resolveTrackLaneAutomationId(30, 'faderDb')).toBe(-1);

      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      expect(engine.resolveTrackLaneAutomationId(10, 'faderDb')).toBe(fader10);
      engine.setParameter(fader10, -60);
      engine.play();
      expect(settled(engine)).toBeCloseTo(1, 3);

      engine.setTrackLanes([{ trackId: 20 }]);
      expect(alive(engine, fader10)).toBe(false);
      engine.setParameter(fader10, -60);
      expect(settled(engine)).toBeCloseTo(1, 3);
    } finally {
      engine.destroy();
    }
  });

  it('numbers fader and instrument ids on the mirror as the worklet engine does', async () => {
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
      // The instrument ids are taken before the worklet engine ever resolves one.
      engine.setSynthInstrument(5, {});
      engine.setSynthInstrument(6, {});
      flush();
      const cutoff6 = engine.resolveInstrumentAutomationId(6, 'cutoffHz');
      expect(cutoff6).toBeGreaterThan(0);
      expect(alive(liveEngine, cutoff6)).toBe(true);

      const fader8 = engine.automationParamId(8, 'faderDb');
      const fader7 = engine.automationParamId(7, 'faderDb');
      flush();
      expect(liveEngine.resolveTrackLaneAutomationId(8, 'faderDb')).toBe(fader8);
      expect(liveEngine.resolveTrackLaneAutomationId(7, 'faderDb')).toBe(fader7);

      // Only track 8 sounds; the id resolved on the mirror drives it on the worklet engine.
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
        type: SonareEngineCommandType.SetParam,
        targetId: fader8,
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
