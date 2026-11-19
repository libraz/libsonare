import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError } from '../../dist/index.js';
import { init, SonareEngine } from '../../dist/worklet.js';
import { unknownKeyText } from '../_helpers';

type Port = { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
  string,
  unknown
>;

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;
}

function readyNode(port: Port): AudioWorkletNode {
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

async function newEngine(options: Record<string, unknown> = {}) {
  const posted: unknown[] = [];
  const port: Port = { postMessage: (message: unknown) => posted.push(message) };
  const engine = await SonareEngine.create(fakeContext(), {
    numChannels: 2,
    mode: 'postMessage',
    nodeFactory: () => readyNode(port),
    ...options,
  });
  return { engine, port, posted };
}

/** The engine as an untyped bag, for old names and wrong-typed arguments. */
function loose(engine: SonareEngine): Record<string, (...args: unknown[]) => unknown> {
  return engine as unknown as Record<string, (...args: unknown[]) => unknown>;
}

beforeAll(async () => {
  await init();
});

describe('SonareEngine shares the RealtimeEngine vocabulary', () => {
  it('requires numChannels rather than following the destination', async () => {
    await expect(newEngine({ numChannels: undefined })).rejects.toThrow(
      /SonareEngine\.create: numChannels must be an integer/,
    );
  });

  it('reads state through transportState and getters', async () => {
    const { engine } = await newEngine();
    try {
      expect(loose(engine).getTransportState).toBeUndefined();
      expect(loose(engine).graphLatencySamplesQ8).toBeUndefined();
      expect(loose(engine).listParameters).toBeUndefined();
      expect(loose(engine).markerCount).toBeUndefined();
      expect(typeof engine.tailSamples).toBe('number');
      expect(typeof engine.graphLatencySamples).toBe('number');
      expect(engine.automationLaneCount).toBe(0);
      expect(engine.warpVoiceCapacity).toBe(8);
      expect(engine.parameters()).toEqual([]);
      expect(engine.markers()).toEqual([]);
      expect(typeof engine.transportState).toBe('function');
    } finally {
      engine.destroy();
    }
  });

  it('takes the loop as one EngineLoop object and returns nothing', async () => {
    const { engine } = await newEngine();
    try {
      expect(engine.setLoop({ startPpq: 0, endPpq: 4 })).toBeUndefined();
      expect(() => engine.setLoop({ startPpq: 0, endPpq: 4, extra: 1 } as never)).toThrow(
        TypeError,
      );
      expect(() => loose(engine).setLoop(0, 4, true)).toThrow(TypeError);
      expect(engine.transport.setLoop({ startPpq: 0, endPpq: 8, enabled: false })).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('names the render frame and the seconds suffix on the transport', async () => {
    const { engine } = await newEngine();
    try {
      expect(engine.transport.play()).toBeUndefined();
      expect(engine.transport.seekPpq(1, 0)).toBeUndefined();
      expect(engine.transport.seekSec(0.5)).toBeUndefined();
      expect(engine.transport.stop()).toBeUndefined();
      expect((engine.transport as unknown as Record<string, unknown>).seekSeconds).toBeUndefined();
      expect(() => engine.transport.seekSec(-1)).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });

  it('throws OutOfMemory instead of returning false when the command ring is full', async () => {
    const { engine } = await newEngine({ mode: 'sab', commandRingCapacity: 1 });
    try {
      engine.setTrackLanes([{ trackId: 3 }]);
      engine.addParameter({ id: 7, name: 'p' });
      // Nothing drains the ring here, so one queued command fills it.
      engine.transport.play();
      const refused = (call: () => unknown): unknown => {
        try {
          call();
        } catch (error) {
          return error;
        }
        return undefined;
      };
      const calls: Array<() => unknown> = [
        () => engine.setParam('', 7, 0.5),
        () => engine.setLoop({ startPpq: 0, endPpq: 4 }),
        () => engine.setMetronome({ enabled: true }),
        () => engine.resetProcessorState(),
        () => engine.resetMasterLoudnessMeter(),
        () => engine.setTrackSoloMute(3, { solo: true, mute: false }),
        () => engine.setTrackMonitorMode(3, 'pfl'),
        () => engine.setStripFaderDb(3, -6),
        () => engine.setStripPan(3, 0.25),
        () => engine.setBusGainDb(1, -3),
        () => engine.transport.stop(),
        () => engine.transport.seekPpq(1),
      ];
      for (const call of calls) {
        const error = refused(call);
        expect(isSonareError(error) && error.code).toBe(ErrorCode.OutOfMemory);
      }
    } finally {
      engine.destroy();
    }
  });

  it('reads markers as a collection and returns the stored markers from setMarkers', async () => {
    const { engine } = await newEngine();
    try {
      const stored = engine.setMarkers([
        { ppq: 4, name: 'b' },
        { ppq: 0, name: 'a' },
      ]);
      expect(stored.map((marker) => marker.name)).toEqual(['b', 'a']);
      expect(engine.markers().map((marker) => marker.ppq)).toEqual([0, 4]);
      expect(engine.seekMarker(stored[0].id)).toBeUndefined();
      expect(engine.setLoopFromMarkers(stored[1].id, stored[0].id)).toBeUndefined();
      expect(() => engine.seekMarker(9999)).toThrow(RangeError);
      expect(loose(engine).markerByIndex).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('takes pan and send names in a scene strip and refuses their ordinals', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setTrackBuses([{ busId: 1 }]);
      const strip: Parameters<typeof engine.setStrip>[1] = {
        id: 'track-10',
        panMode: 'stereoPan',
        panLaw: 'const6Db',
        sends: [{ id: 'send', destinationBusId: '1', sendDb: -6, timing: 'postFader' }],
      };
      expect(() => engine.setStrip(10, strip)).not.toThrow();
      expect(() => engine.setStrip('master', { id: 'master', panLaw: 'linear0Db' })).not.toThrow();
      expect(() =>
        engine.setBusStrip(1, { id: '1', role: 'aux', panMode: 'balance', panLaw: 'const3Db' }),
      ).not.toThrow();
      expect(() => engine.setStrip(10, { id: 'track-10', panMode: 2 as never })).toThrow(TypeError);
      expect(() =>
        engine.setStrip(10, {
          id: 'track-10',
          sends: [{ id: 's', destinationBusId: 'bus-1', timing: 'post' as never }],
        }),
      ).toThrow(RangeError);
      expect(() =>
        engine.setBusStrip(1, { id: '1', role: 'aux', panLaw: 'const-6db' as never }),
      ).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a strip edit on the target and takes labelled objects', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 3 }]);
      expect(engine.setStrip(3, { id: 'track-3' } as never)).toBeUndefined();
      expect(() => engine.setStrip(3, 'not an object' as never)).toThrow(TypeError);
      expect(() => engine.setStripEqBand(3, 0, 'x' as never)).toThrow(TypeError);
      expect(() => engine.setStripInsertBypassed(3, 0, true as never)).toThrow(TypeError);
      expect(() =>
        engine.setStripInsertParam(3, 0, { name: 'a', value: 1, extra: 1 } as never),
      ).toThrow(/setStripInsertParam: unknown key extra \(expected /);
      expect(() => engine.setBusInsertBypassed(1, 0, { bypassed: 'yes' } as never)).toThrow();
      for (const old of [
        'setTrackStripJson',
        'setMasterStripJson',
        'setStripInserts',
        'setMasterChain',
        'setStripGain',
        'setBusGain',
        'setStripEq',
        'setTrackStripEqBand',
        'setMasterStripEqBand',
        'setTrackStripInsertBypassed',
        'setMasterStripInsertBypassed',
        'setTrackStripInsertParamByName',
        'setMasterStripInsertParamByName',
        'setStripInsertParamByName',
        'setBusStripInsertParamByName',
        'setTrackStripPan',
        'setTrackStripPanLaw',
        'setBusStripPan',
        'setBusStripJson',
        'setBusStripEqBand',
        'setBusStripInsertBypassed',
        'automationParamId',
        'busAutomationParamId',
        'resolveTrackInsertAutomationId',
        'resolveMasterInsertAutomationId',
      ]) {
        expect(loose(engine)[old], old).toBeUndefined();
      }
    } finally {
      engine.destroy();
    }
  });

  it('refuses the master strip on a track-only pan setter', async () => {
    const { engine } = await newEngine();
    try {
      expect(() => engine.setStripPanLaw('master', 'const3Db')).toThrow(RangeError);
      expect(() => engine.setStripPanMode('master', 'balance')).toThrow(RangeError);
      expect(() => engine.setStripDualPan('master', -1, 1)).toThrow(RangeError);
      expect(() => engine.setStripSurroundPan('master', {})).toThrow(RangeError);
      expect(() => engine.setStripChannelDelaySamples('master', 4)).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });

  it('resolves strip automation ids by target and parameter', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 3 }]);
      const fader = engine.resolveStripAutomationId(3, 'faderDb');
      expect(engine.resolveStripAutomationId(3, 'pan')).not.toBe(fader);
      expect(engine.resolveStripAutomationId('master', 'faderDb')).not.toBe(fader);
      expect(() => engine.resolveStripAutomationId(3, 'width')).toThrow(RangeError);
      engine.setTrackBuses([{ busId: 1 }]);
      expect(engine.resolveBusAutomationId(1, 'faderDb')).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });

  it('keys track solo, mute and monitor by track id', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 5 }, { trackId: 9 }]);
      expect(engine.setTrackSoloMute(9, { solo: true, mute: false })).toBeUndefined();
      expect(engine.setTrackMonitorMode(9, 'afl')).toBeUndefined();
      expect(() =>
        engine.setTrackSoloMute(9, { solo: true, mute: false, extra: 1 } as never),
      ).toThrow(TypeError);
      expect(() => engine.setTrackMonitorMode(9, 1 as never)).toThrow(TypeError);
      expect(loose(engine).setSoloMute).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('declares lanes as descriptors and routes to the master with null', async () => {
    const { engine } = await newEngine();
    try {
      expect(() => engine.setTrackLanes([3] as never)).toThrow(TypeError);
      expect(() => engine.setTrackLanes([{ trackId: 3, sourceChannelLayout: '5.1' }])).toThrow(
        RangeError,
      );
      engine.setTrackLanes([{ trackId: 3 }]);
      expect(engine.setTrackOutputBus(3, null)).toBeUndefined();
      expect(() => engine.setTrackOutputBus(3, 0)).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });

  it('keys an insert from a source object and refuses id 0', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 3 }, { trackId: 4 }]);
      expect(() => engine.setStripSidechain(3, 0, { kind: 'track', id: 0 })).toThrow(RangeError);
      expect(() => engine.setStripSidechain(3, 0, { kind: 1 as never, id: 4 })).toThrow(TypeError);
      expect(() =>
        engine.setStripSidechain(3, 0, { kind: 'track', id: 4, extra: 1 } as never),
      ).toThrow(TypeError);
      expect(() => engine.setBusSidechain(1, 0, { kind: 'track', id: 0 })).toThrow(RangeError);
      expect(() => engine.canSetBusSidechain(1, 0, { kind: 'track', id: 0 })).toThrow(RangeError);
      for (const old of [
        'setLaneSidechain',
        'setMasterSidechain',
        'canSetLaneSidechain',
        'canSetMasterSidechain',
      ]) {
        expect(loose(engine)[old], old).toBeUndefined();
      }
    } finally {
      engine.destroy();
    }
  });

  it('configures capture from one options object', async () => {
    const { engine } = await newEngine();
    try {
      expect(engine.configureCapture({ capacityFrames: 128 })).toBeUndefined();
      expect(
        engine.configureCapture({
          capacityFrames: 128,
          numChannels: 2,
          source: 'input',
          punch: { startSec: 0.5, endSec: 1 },
        }),
      ).toBeUndefined();
      expect(() => engine.configureCapture({ bufferFrames: 128 } as never)).toThrow(
        /configureCapture: unknown key bufferFrames \(expected /,
      );
      expect(() => engine.configureCapture({ capacityFrames: 128, source: 1 as never })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.configureCapture({
          capacityFrames: 128,
          punch: { startSample: 1, startSec: 1, endSample: 2 },
        }),
      ).toThrow(RangeError);
      expect(engine.armCapture({ armed: true })).toBeUndefined();
      expect(engine.armCapture({ armed: false })).toBeUndefined();
      expect(() => engine.armCapture(true as never)).toThrow(TypeError);
      expect(loose(engine).armRecord).toBeUndefined();
      expect(loose(engine).punch).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('renders from a request and refuses the positional frame count', async () => {
    const { engine } = await newEngine();
    try {
      const planes = await engine.renderOffline({ totalFrames: 256 });
      expect(planes).toHaveLength(2);
      expect(planes[0]).toHaveLength(256);
      await expect(
        engine.renderOffline({ totalFrames: 256, validate: false } as never),
      ).rejects.toThrow(unknownKeyText('renderOffline: unknown key validate'));
      await expect(engine.renderOffline(256 as never)).rejects.toThrow(TypeError);
      await expect(
        engine.renderOffline({ totalFrames: 256, sampleRate: 48000 } as never),
      ).rejects.toThrow(/SonareEngine.renderOffline: unknown key sampleRate \(expected /);
    } finally {
      engine.destroy();
    }
  });

  it('bounces the timeline to planar channels', async () => {
    const { engine } = await newEngine();
    try {
      const result = await engine.bounceOffline({ totalFrames: 256, numChannels: 2 });
      expect(result).not.toHaveProperty('numChannels');
      expect(result.sampleRate).toBe(48000);
      expect(result.channels).toHaveLength(2);
      expect(result.channels[0]).toBeInstanceOf(Float32Array);
      expect(result.channels[0]).toHaveLength(256);
      expect(typeof result.integratedLufs).toBe('number');
      expect(result).not.toHaveProperty('interleaved');
    } finally {
      engine.destroy();
    }
  });

  it('adds a clip from one clip object', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 3 }]);
      const id = engine.addClip(3, { id: 1, channels: [new Float32Array(64)], startPpq: 0 });
      expect(id).toBe(1);
      expect(() => engine.addClip(3, { startPpq: 0 })).toThrow(
        new TypeError('SonareEngine.addClip: give clip.channels or clip.pageProvider'),
      );
      expect(() =>
        loose(engine).addClip(3, { channels: [new Float32Array(8)], pageProvider: 1, startPpq: 0 }),
      ).toThrow(
        new RangeError('SonareEngine.addClip: give clip.channels or clip.pageProvider, not both'),
      );
      expect(() => loose(engine).addClip(3, [new Float32Array(8)], 0, { id: 2 })).toThrow(
        TypeError,
      );
    } finally {
      engine.destroy();
    }
  });

  it('schedules automation points as objects with string curves', async () => {
    const { engine } = await newEngine();
    try {
      engine.addParameter({ id: 7, name: 'p' });
      expect(
        engine.scheduleParam('', 'p', { ppq: 0, value: 0.5, curveToNext: 'exponential' }),
      ).toBeUndefined();
      expect(engine.addAutomationPoint(7, { ppq: 1, value: 1 })).toBeUndefined();
      expect(() =>
        engine.scheduleParam('', 'p', { ppq: 2, value: 1, curveToNext: 1 as never }),
      ).toThrow(TypeError);
      expect(() =>
        engine.scheduleParam('', 'p', { ppq: 2, value: 1, curveToNext: 'bogus' as never }),
      ).toThrow(RangeError);
      expect(() => engine.scheduleParam('', 'p', { ppq: 2, value: 1, extra: 1 } as never)).toThrow(
        TypeError,
      );
      expect(() => loose(engine).scheduleParam('', 'p', 3, 1, 'linear')).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('binds instruments by track first and refuses a destinationId in the config', async () => {
    const { engine } = await newEngine();
    try {
      engine.setTrackLanes([{ trackId: 3 }]);
      expect(engine.setBuiltinInstrument(3)).toBeUndefined();
      expect(() => engine.setBuiltinInstrument(3, { destinationId: 3 } as never)).toThrow(
        /destinationId/,
      );
      expect(() => engine.setSf2Instrument(3, { destinationId: 3 } as never)).toThrow(
        /destinationId/,
      );
      expect(engine.setSynthInstrument(3, { preset: 'sawLead' })).toBeUndefined();
      expect(() => engine.setSynthInstrument(3, 'sawLead' as never)).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('binds a controller from one binding and packs the program bank as an object', async () => {
    const { engine, posted } = await newEngine();
    try {
      engine.addParameter({ id: 7, name: 'p' });
      engine.setTrackLanes([{ trackId: 3 }]);
      expect(
        engine.bindMidiCc({ ccNumber: 1, paramId: 7, minValue: 0, maxValue: 1 }),
      ).toBeUndefined();
      expect(
        posted.some((message) => (message as { type?: string }).type === 'syncMidiCcBinding'),
      ).toBe(true);
      expect(() => loose(engine).bindMidiCc(0, 1, 7, {})).toThrow(TypeError);
      expect(engine.pushMidiProgram(3, 0, 0, 5, { msb: 0, lsb: 1 })).toBeUndefined();
      expect(engine.pushMidiProgram(3, 0, 0, 5)).toBeUndefined();
      expect(() =>
        engine.pushMidiProgram(3, 0, 0, 5, { msb: 0, lsb: 1, extra: 1 } as never),
      ).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('names the single-boolean setters after the property they set', async () => {
    const { engine } = await newEngine();
    try {
      expect(engine.setMidiDestinationExternal.length).toBe(2);
      expect(engine.setExternalMidiClockEnabled.length).toBe(1);
      expect(engine.setExternalMidiClockEnabled(false)).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('delivers telemetry and external MIDI as the public engine types', async () => {
    const { engine, port } = await newEngine();
    try {
      const telemetry: unknown[] = [];
      const midi: unknown[] = [];
      engine.onTelemetry((record) => telemetry.push(record));
      engine.onMidiOut((events) => midi.push(...events));
      port.onmessage?.({
        data: {
          type: 1,
          error: 19,
          renderFrame: 5,
          timelineSample: 6,
          audibleTimelineSample: 7,
          graphLatencySamples: 2,
          value: 3,
        },
      } as MessageEvent<unknown>);
      port.onmessage?.({
        data: {
          type: 'externalMidi',
          events: [{ destinationId: 1, renderFrame: 9, bytes: [0x90, 60, 100] }],
        },
      } as MessageEvent<unknown>);
      expect(telemetry).toEqual([
        {
          type: 'error',
          error: 'invalidCommand',
          renderFrame: 5,
          timelineSample: 6,
          audibleTimelineSample: 7,
          graphLatencySamples: 2,
          value: 3,
        },
      ]);
      expect(midi).toEqual([
        { destinationId: 1, renderFrame: 9, bytes: Uint8Array.of(0x90, 60, 100) },
      ]);
    } finally {
      engine.destroy();
    }
  });

  it('names the offline plane count offlineNumChannels', async () => {
    const { engine } = await newEngine({ offlineNumChannels: 1 });
    try {
      const planes = await engine.renderOffline({ totalFrames: 128 });
      expect(planes).toHaveLength(1);
    } finally {
      engine.destroy();
    }
    await expect(newEngine({ offlineNumChannels: 0 })).rejects.toThrow(RangeError);
  });
});
