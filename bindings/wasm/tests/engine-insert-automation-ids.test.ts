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
    version: 2,
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

function caught(run: () => unknown): Error {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected a refusal');
}

describe('insert automation ids (WASM)', () => {
  setupWorklet();

  it.each([
    {
      name: 'track insert',
      cEntry: 'engine_resolve_track_insert_automation_id',
      subjects: ['paramName'],
      text: 'RealtimeEngine.resolveStripInsertAutomationId: paramName must be the name of a parameter of the insert that can be set while running',
      run: (engine: RealtimeEngine) => engine.resolveStripInsertAutomationId(10, 0, 'noSuchParam'),
    },
    {
      name: 'master insert',
      cEntry: 'engine_resolve_master_insert_automation_id',
      subjects: ['paramName'],
      text: 'RealtimeEngine.resolveStripInsertAutomationId: paramName must be the name of a parameter of the insert that can be set while running',
      run: (engine: RealtimeEngine) =>
        engine.resolveStripInsertAutomationId('master', 0, 'noSuchParam'),
    },
    {
      name: 'bus insert',
      cEntry: 'engine_resolve_bus_insert_automation_id',
      subjects: ['paramName'],
      text: 'RealtimeEngine.resolveBusInsertAutomationId: paramName must be the name of a parameter of the insert that can be set while running',
      run: (engine: RealtimeEngine) => engine.resolveBusInsertAutomationId(1, 0, 'noSuchParam'),
    },
    {
      name: 'track lane kind',
      cEntry: 'engine_resolve_track_lane_automation_id',
      subjects: ['kind'],
      text: 'RealtimeEngine.resolveStripAutomationId: kind must be the name of a track strip parameter: faderDb or pan',
      run: (engine: RealtimeEngine) => engine.resolveStripAutomationId(10, 'noSuchParam' as never),
    },
    {
      name: 'master strip kind',
      cEntry: 'engine_resolve_master_automation_id',
      subjects: ['kind'],
      text: 'RealtimeEngine.resolveStripAutomationId: kind must be the name of a master strip parameter: faderDb, pan or width',
      run: (engine: RealtimeEngine) =>
        engine.resolveStripAutomationId('master', 'noSuchParam' as never),
    },
    {
      name: 'bus strip parameter',
      cEntry: 'engine_resolve_bus_automation_id',
      subjects: ['paramName'],
      text: 'RealtimeEngine.resolveBusAutomationId: paramName must be the name of a bus strip parameter: faderDb',
      run: (engine: RealtimeEngine) => engine.resolveBusAutomationId(1, 'noSuchParam' as never),
    },
    {
      name: 'instrument parameter',
      cEntry: 'engine_resolve_instrument_automation_id',
      subjects: ['paramName', 'destinationId'],
      text: 'RealtimeEngine.resolveInstrumentAutomationId: paramName must be the name of a parameter of the instrument bound to destinationId',
      run: (engine: RealtimeEngine) => engine.resolveInstrumentAutomationId(5, 'noSuchParam'),
    },
  ])('uses the native refusal for an unknown $name', ({ cEntry, subjects, text, run }) => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setStrip(
        10,
        JSON.parse(insertScene('strips', 'track-10', ['utility.gain'])).strips[0],
      );
      engine.setStrip(
        'master',
        JSON.parse(insertScene('strips', 'master', ['utility.gain'])).strips[0],
      );
      engine.setBusStrip(1, JSON.parse(insertScene('buses', '1', ['utility.gain'])).buses[0]);
      engine.setSynthInstrument(5, {});

      const error = caught(() => run(engine));
      expect(error).toBeInstanceOf(RangeError);
      expect(error.message).toBe(text);
      const refusal = (
        error as never as Record<
          symbol,
          {
            readonly cEntry: string | null;
            readonly subjects: readonly string[];
          }
        >
      )[Symbol.for('libsonare.refusal')];
      expect(refusal?.subjects).toEqual(subjects);
      expect(refusal?.cEntry).toBe(cEntry);
    } finally {
      engine.destroy();
    }
  });

  it('uses destinationId as the instrument-refusal sibling when no instrument is bound', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    try {
      const error = caught(() => engine.resolveInstrumentAutomationId(99, 'gain'));
      expect(error).toBeInstanceOf(RangeError);
      expect(error.message).toBe(
        'RealtimeEngine.resolveInstrumentAutomationId: destinationId must be the id of a destination with a MIDI instrument in this engine',
      );
      const refusal = (
        error as never as Record<
          symbol,
          {
            readonly cEntry: string | null;
            readonly subjects: readonly string[];
          }
        >
      )[Symbol.for('libsonare.refusal')];
      expect(refusal?.subjects).toEqual(['destinationId']);
      expect(refusal?.cEntry).toBe('engine_resolve_instrument_automation_id');
    } finally {
      engine.destroy();
    }
  });

  it('routes track width through the declared track-lane name refusal', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      const error = caught(() => engine.resolveStripAutomationId(10, 'width'));
      expect(error).toBeInstanceOf(RangeError);
      expect(error.message).toBe(
        'RealtimeEngine.resolveStripAutomationId: kind must be the name of a track strip parameter: faderDb or pan',
      );
      const refusal = (
        error as never as Record<
          symbol,
          {
            readonly cEntry: string | null;
            readonly subjects: readonly string[];
          }
        >
      )[Symbol.for('libsonare.refusal')];
      expect(refusal?.subjects).toEqual(['kind']);
      expect(refusal?.cEntry).toBe('engine_resolve_track_lane_automation_id');
    } finally {
      engine.destroy();
    }
  });

  it('keeps a track insert lane on its track when a later lane edit follows a reorder', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
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
      engine.setStrip(
        10,
        JSON.parse(insertScene('strips', 'track-10', ['utility.gain'])).strips[0],
      );
      engine.setStrip(
        20,
        JSON.parse(insertScene('strips', 'track-20', ['utility.gain'])).strips[0],
      );
      engine.setStrip(
        'master',
        JSON.parse(insertScene('strips', 'master', ['utility.gain'])).strips[0],
      );

      const track10Gain = engine.resolveStripInsertAutomationId(10, 0, 'levelDb');
      expect(track10Gain).toBeGreaterThan(0);
      engine.setAutomationLane(track10Gain, [{ ppq: 0, value: -60 }]);

      // Reorder, then edit an unrelated lane: the track 10 lane must still name track 10.
      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      const masterGain = engine.resolveStripInsertAutomationId('master', 0, 'levelDb');
      expect(masterGain).toBeGreaterThan(0);
      engine.setAutomationLane(masterGain, [{ ppq: 0, value: 0 }]);

      expect(settled(engine)).toBeCloseTo(1, 3);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a track id across reorders and the removal of another track', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    try {
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      engine.setStrip(
        10,
        JSON.parse(insertScene('strips', 'track-10', ['utility.gain'])).strips[0],
      );
      engine.setStrip(
        20,
        JSON.parse(insertScene('strips', 'track-20', ['utility.gain'])).strips[0],
      );
      const id10 = engine.resolveStripInsertAutomationId(10, 0, 'levelDb');
      const id20 = engine.resolveStripInsertAutomationId(20, 0, 'levelDb');
      expect(id10).toBeGreaterThan(0);
      expect(id20).not.toBe(id10);

      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      expect(engine.resolveStripInsertAutomationId(10, 0, 'levelDb')).toBe(id10);

      engine.setTrackLanes([{ trackId: 10 }]);
      expect(alive(engine, id10)).toBe(true);
      expect(alive(engine, id20)).toBe(false);

      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      engine.setStrip(
        20,
        JSON.parse(insertScene('strips', 'track-20', ['utility.gain'])).strips[0],
      );
      expect(alive(engine, id20)).toBe(false);
      const readded = engine.resolveStripInsertAutomationId(20, 0, 'levelDb');
      expect(readded).toBeGreaterThan(0);
      expect(readded).not.toBe(id20);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a bus id across bus reorders and retires it when the bus is removed', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      engine.setTrackBuses([{ busId: 1 }, { busId: 2 }]);
      engine.setBusStrip(1, JSON.parse(insertScene('buses', '1', ['utility.gain'])).buses[0]);
      engine.setBusStrip(2, JSON.parse(insertScene('buses', '2', ['utility.gain'])).buses[0]);
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
      engine.setBusStrip(1, JSON.parse(insertScene('buses', '1', ['utility.gain'])).buses[0]);
      expect(alive(engine, id1)).toBe(false);
      expect(engine.resolveBusInsertAutomationId(1, 0, 'levelDb')).not.toBe(id1);
    } finally {
      engine.destroy();
    }
  });

  it('retires track, bus and master ids when the slot changes processor type', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setTrackBuses([{ busId: 1 }]);
      const setAll = (processor: string) => {
        engine.setStrip(10, JSON.parse(insertScene('strips', 'track-10', [processor])).strips[0]);
        engine.setBusStrip(1, JSON.parse(insertScene('buses', '1', [processor])).buses[0]);
        engine.setStrip(
          'master',
          JSON.parse(insertScene('strips', 'master', [processor])).strips[0],
        );
      };
      const resolveAll = (key: string) => [
        engine.resolveStripInsertAutomationId(10, 0, key),
        engine.resolveBusInsertAutomationId(1, 0, key),
        engine.resolveStripInsertAutomationId('master', 0, key),
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 64 });
    try {
      // 64 slots per rebuild against 8192 entries: rebuild 129 cannot fit.
      const gain = insertScene('strips', 'master', Array(64).fill('utility.gain'));
      const tilt = insertScene('strips', 'master', Array(64).fill('eq.tilt'));
      for (let round = 0; round < 128; round += 1) {
        engine.setStrip('master', JSON.parse(round % 2 === 0 ? gain : tilt).strips[0]);
      }
      const before = engine.resolveStripInsertAutomationId('master', 0, 'tiltDb');
      expect(() => engine.setStrip('master', JSON.parse(gain).strips[0])).toThrow();
      expect(alive(engine, before)).toBe(true);
      expect(() => engine.setStrip('master', JSON.parse(tilt).strips[0])).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('numbers ids identically on the mirror and the worklet engine, and drives the worklet', async () => {
    const posted: unknown[] = [];
    const offline = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: kBlock });
    const engine = await SonareEngine.create(fakeContext(), {
      numChannels: 2,
      mode: 'postMessage',
      offlineEngine: offline as unknown as OfflineEngineOption,
      offlineNumChannels: 1,
      nodeFactory: () =>
        readyWorkletNode({
          postMessage: (message: unknown) => posted.push(message),
          onmessage: undefined,
        }),
    });
    const live = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: 48000,
      maxBlockSize: kBlock,
      numChannels: 1,
      transport: { postMessage: () => undefined },
    });
    const liveEngine = (live as unknown as { engine: RealtimeEngine }).engine;
    const flush = () => {
      for (const message of posted.splice(0)) {
        const type = (message as { type?: unknown }).type;
        if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
          live.receiveMessage(message);
        }
      }
    };
    // An unresolvable name throws RangeError on both engines; a resolved one must match by id.
    const outcome = (resolve: () => number): number | 'refused' => {
      try {
        return resolve();
      } catch (error) {
        expect(error).toBeInstanceOf(RangeError);
        return 'refused';
      }
    };
    const expectSameIds = () => {
      let resolved = 0;
      for (const slot of [0, 1]) {
        for (const key of ['levelDb', 'thresholdDb']) {
          const pairs = [
            ...[7, 8].map((track) => [
              outcome(() => offline.resolveStripInsertAutomationId(track, slot, key)),
              outcome(() => liveEngine.resolveStripInsertAutomationId(track, slot, key)),
            ]),
            ...[100, 200].map((bus) => [
              outcome(() => offline.resolveBusInsertAutomationId(bus, slot, key)),
              outcome(() => liveEngine.resolveBusInsertAutomationId(bus, slot, key)),
            ]),
            [
              outcome(() => offline.resolveStripInsertAutomationId('master', slot, key)),
              outcome(() => liveEngine.resolveStripInsertAutomationId('master', slot, key)),
            ],
          ];
          for (const [mirror, worklet] of pairs) {
            expect(worklet).toBe(mirror);
            if (mirror !== 'refused') {
              resolved += 1;
            }
          }
        }
      }
      return resolved;
    };
    try {
      engine.setStrip(
        'master',
        JSON.parse(insertScene('strips', 'master', ['utility.gain'])).strips[0],
      );
      engine.setTrackLanes([{ trackId: 7 }]);
      engine.setStrip(7, JSON.parse(insertScene('strips', 'track-7', ['utility.gain'])).strips[0]);
      flush();
      expect(expectSameIds()).toBe(2);

      engine.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
      engine.setBusStrip(
        200,
        JSON.parse(insertScene('buses', '200', ['dynamics.compressor'])).buses[0],
      );
      engine.setBusStrip(100, JSON.parse(insertScene('buses', '100', ['utility.gain'])).buses[0]);
      engine.setTrackLanes([{ trackId: 7 }, { trackId: 8 }]);
      engine.setStrip(8, JSON.parse(insertScene('strips', 'track-8', ['utility.gain'])).strips[0]);
      // A processor-type change at slot 0 retires that id on both engines alike.
      engine.setStrip(
        7,
        JSON.parse(insertScene('strips', 'track-7', ['dynamics.compressor', 'utility.gain']))
          .strips[0],
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
      engine.setBusStrip(100, JSON.parse(insertScene('buses', '100', ['utility.gain'])).buses[0]);
      flush();
      expect(expectSameIds()).toBe(6);

      // An id resolved on the mirror drives the same parameter on the worklet engine.
      const gainId = offline.resolveStripInsertAutomationId(7, 1, 'levelDb');
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
      live.receiveMessage({ type: 'syncClips', clips });
      offline.setParameter(gainId, -12);
      live.receiveMessage({
        type: SonareEngineCommandType.SetParam,
        targetId: gainId,
        renderFrame: -1,
        argFloat: -12,
      });
      offline.play();
      live.receiveMessage({ type: SonareEngineCommandType.TransportPlay, renderFrame: -1 });
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
