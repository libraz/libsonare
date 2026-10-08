/**
 * Insert automation id lifetimes on the WASM RealtimeEngine and across the
 * worklet facade. Mirrors the C ABI cases in
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
const kFrames = kBlock * 40;

/** The engine `SonareEngine.create` accepts as its offline mirror. */
type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

function insertScene(kind: 'strips' | 'buses', id: string, processors: string[]): string {
  const inserts = processors.map((processor, index) => ({
    slot: index < 32 ? 'pre' : 'post',
    processor,
    params: {},
  }));
  const strip = { id, inserts };
  return JSON.stringify({
    version: 1,
    strips: kind === 'strips' ? [strip] : [],
    buses: kind === 'buses' ? [strip] : [],
    connections: [],
  });
}

function alive(engine: RealtimeEngine, id: number): boolean {
  try {
    engine.parameterInfo(id);
    return true;
  } catch {
    return false;
  }
}

function settled(engine: RealtimeEngine, blocks = 30): number {
  engine.seekSample(0);
  engine.play();
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

describe('insert automation ids (WASM)', () => {
  setupWorklet();

  it('keeps a track insert lane on its track when a later lane edit follows a reorder', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      // Only track 20 sounds, so the output reads track 20's gain alone.
      engine.setClips([
        {
          trackId: 20,
          channels: [new Float32Array(kFrames).fill(1)],
          startPpq: 0,
          lengthSamples: kFrames,
          gain: 1,
        },
      ]);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      engine.setTrackStripJson(10, insertScene('strips', 'track-10', ['utility.gain']));
      engine.setTrackStripJson(20, insertScene('strips', 'track-20', ['utility.gain']));
      engine.setMasterStripJson(insertScene('strips', 'master', ['utility.gain']));

      const track10Gain = engine.resolveTrackInsertAutomationId(10, 0, 'levelDb');
      expect(track10Gain).toBeGreaterThan(0);
      engine.setAutomationLane(track10Gain, [{ ppq: 0, value: -60 }]);

      // Reorder, then edit an unrelated lane: the track 10 lane must still name track 10.
      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      const masterGain = engine.resolveMasterInsertAutomationId(0, 'levelDb');
      expect(masterGain).toBeGreaterThan(0);
      engine.setAutomationLane(masterGain, [{ ppq: 0, value: 0 }]);

      expect(settled(engine)).toBeCloseTo(1, 3);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a track id across reorders and the removal of another track', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      engine.setTrackLanes([10, 20]);
      engine.setTrackStripJson(10, insertScene('strips', 'track-10', ['utility.gain']));
      engine.setTrackStripJson(20, insertScene('strips', 'track-20', ['utility.gain']));
      const id10 = engine.resolveTrackInsertAutomationId(10, 0, 'levelDb');
      const id20 = engine.resolveTrackInsertAutomationId(20, 0, 'levelDb');
      expect(id10).toBeGreaterThan(0);
      expect(id20).not.toBe(id10);

      engine.setTrackLanes([20, 10]);
      expect(engine.resolveTrackInsertAutomationId(10, 0, 'levelDb')).toBe(id10);

      engine.setTrackLanes([10]);
      expect(alive(engine, id10)).toBe(true);
      expect(alive(engine, id20)).toBe(false);

      engine.setTrackLanes([20, 10]);
      engine.setTrackStripJson(20, insertScene('strips', 'track-20', ['utility.gain']));
      expect(alive(engine, id20)).toBe(false);
      const readded = engine.resolveTrackInsertAutomationId(20, 0, 'levelDb');
      expect(readded).toBeGreaterThan(0);
      expect(readded).not.toBe(id20);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a bus id across bus reorders and retires it when the bus is removed', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackBuses([{ busId: 1 }, { busId: 2 }]);
      engine.setBusStripJson(1, insertScene('buses', '1', ['utility.gain']));
      engine.setBusStripJson(2, insertScene('buses', '2', ['utility.gain']));
      const id1 = engine.resolveBusInsertAutomationId(1, 0, 'levelDb');
      const id2 = engine.resolveBusInsertAutomationId(2, 0, 'levelDb');
      expect(id1).toBeGreaterThan(0);
      expect(id2).not.toBe(id1);

      engine.setTrackBuses([{ busId: 2 }, { busId: 1 }]);
      expect(engine.resolveBusInsertAutomationId(1, 0, 'levelDb')).toBe(id1);
      expect(alive(engine, id2)).toBe(true);

      engine.setTrackBuses([{ busId: 2 }]);
      expect(alive(engine, id2)).toBe(true);
      expect(alive(engine, id1)).toBe(false);
      engine.setTrackBuses([{ busId: 2 }, { busId: 1 }]);
      engine.setBusStripJson(1, insertScene('buses', '1', ['utility.gain']));
      expect(alive(engine, id1)).toBe(false);
      expect(engine.resolveBusInsertAutomationId(1, 0, 'levelDb')).not.toBe(id1);
    } finally {
      engine.destroy();
    }
  });

  it('retires track, bus and master ids when the slot changes processor type', () => {
    const engine = new RealtimeEngine(48000, kBlock);
    try {
      engine.setTrackLanes([10]);
      engine.setTrackBuses([{ busId: 1 }]);
      const setAll = (processor: string) => {
        engine.setTrackStripJson(10, insertScene('strips', 'track-10', [processor]));
        engine.setBusStripJson(1, insertScene('buses', '1', [processor]));
        engine.setMasterStripJson(insertScene('strips', 'master', [processor]));
      };
      const resolveAll = (key: string) => [
        engine.resolveTrackInsertAutomationId(10, 0, key),
        engine.resolveBusInsertAutomationId(1, 0, key),
        engine.resolveMasterInsertAutomationId(0, key),
      ];

      setAll('utility.gain');
      const gainIds = resolveAll('levelDb');
      for (const id of gainIds) {
        expect(id).toBeGreaterThan(0);
      }
      setAll('utility.gain');
      expect(resolveAll('levelDb')).toEqual(gainIds);

      setAll('dynamics.compressor');
      for (const id of gainIds) {
        expect(alive(engine, id)).toBe(false);
      }
      const compressorIds = resolveAll('thresholdDb');
      compressorIds.forEach((id, index) => {
        expect(alive(engine, id)).toBe(true);
        expect(id).not.toBe(gainIds[index]);
      });
      setAll('utility.gain');
      for (const id of gainIds) {
        expect(alive(engine, id)).toBe(false);
      }
    } finally {
      engine.destroy();
    }
  });

  it('refuses a strip change that would overflow the id table', () => {
    const engine = new RealtimeEngine(48000, 64);
    try {
      // 64 slots per rebuild against 8192 entries: rebuild 129 cannot fit.
      const gain = insertScene('strips', 'master', Array(64).fill('utility.gain'));
      const tilt = insertScene('strips', 'master', Array(64).fill('eq.tilt'));
      for (let round = 0; round < 128; round += 1) {
        engine.setMasterStripJson(round % 2 === 0 ? gain : tilt);
      }
      const before = engine.resolveMasterInsertAutomationId(0, 'tiltDb');
      expect(() => engine.setMasterStripJson(gain)).toThrow();
      expect(alive(engine, before)).toBe(true);
      expect(() => engine.setMasterStripJson(tilt)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('numbers ids identically on the mirror and the worklet engine, and drives the worklet', async () => {
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
    const expectSameIds = () => {
      let resolved = 0;
      for (const slot of [0, 1]) {
        for (const key of ['levelDb', 'thresholdDb']) {
          const pairs = [
            ...[7, 8].map((track) => [
              offline.resolveTrackInsertAutomationId(track, slot, key),
              liveEngine.resolveTrackInsertAutomationId(track, slot, key),
            ]),
            ...[100, 200].map((bus) => [
              offline.resolveBusInsertAutomationId(bus, slot, key),
              liveEngine.resolveBusInsertAutomationId(bus, slot, key),
            ]),
            [
              offline.resolveMasterInsertAutomationId(slot, key),
              liveEngine.resolveMasterInsertAutomationId(slot, key),
            ],
          ];
          for (const [mirror, worklet] of pairs) {
            expect(worklet).toBe(mirror);
            if (mirror > 0) {
              resolved += 1;
            }
          }
        }
      }
      return resolved;
    };
    try {
      engine.setMasterStripJson(insertScene('strips', 'master', ['utility.gain']));
      engine.setTrackLanes([7]);
      engine.setTrackStripJson(7, insertScene('strips', 'track-7', ['utility.gain']));
      flush();
      expect(expectSameIds()).toBe(2);

      engine.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
      engine.setBusStripJson(200, insertScene('buses', '200', ['dynamics.compressor']));
      engine.setBusStripJson(100, insertScene('buses', '100', ['utility.gain']));
      engine.setTrackLanes([7, 8]);
      engine.setTrackStripJson(8, insertScene('strips', 'track-8', ['utility.gain']));
      // A processor-type change at slot 0 retires that id on both engines alike.
      engine.setTrackStripJson(
        7,
        insertScene('strips', 'track-7', ['dynamics.compressor', 'utility.gain']),
      );
      flush();
      expect(expectSameIds()).toBe(6);

      engine.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
      flush();
      expect(expectSameIds()).toBe(6);
      engine.setTrackBuses([{ busId: 200 }]);
      flush();
      expect(expectSameIds()).toBe(5);
      engine.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
      engine.setBusStripJson(100, insertScene('buses', '100', ['utility.gain']));
      flush();
      expect(expectSameIds()).toBe(6);

      // An id resolved on the mirror drives the same parameter on the worklet engine.
      const gainId = offline.resolveTrackInsertAutomationId(7, 1, 'levelDb');
      expect(gainId).toBeGreaterThan(0);
      const clips = [
        {
          id: 1,
          trackId: 7,
          channels: [new Float32Array(kFrames).fill(0.25)],
          startPpq: 0,
          lengthSamples: kFrames,
        },
      ];
      offline.setClips(clips);
      live.receiveSync({ type: 'syncClips', clips });
      offline.setParameter(gainId, -12);
      live.receiveCommand({
        type: SonareEngineCommandType.SetParam,
        targetId: gainId,
        sampleTime: -1,
        argFloat: -12,
      });
      offline.play();
      live.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
      let expected = 0;
      let actual = 0;
      for (let block = 0; block < 30; block += 1) {
        const [mirrorBlock] = offline.process([new Float32Array(kBlock)]);
        const output = [new Float32Array(kBlock)];
        expect(live.process([[]], [output])).toBe(true);
        expected = mirrorBlock[kBlock - 1];
        actual = output[0][kBlock - 1];
      }
      expect(actual).toBeGreaterThan(0);
      expect(actual).toBeLessThan(0.25 * 0.6);
      expect(actual).toBeCloseTo(expected, 5);
    } finally {
      live.destroy();
      engine.destroy();
    }
  });
});
