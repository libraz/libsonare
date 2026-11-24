import { readFileSync } from 'node:fs';
import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  type ClipPageProvider,
  ErrorCode,
  isSonareError,
  RealtimeEngine,
  SampleBank,
} from '../../dist/index.js';
import type {
  EngineClip,
  EngineGraphMix,
  EngineGraphNodeType,
  EngineLoop,
  MixSceneStrip,
  PartRigMode,
  PartRigRequest,
  ProjectMidiCcBinding,
  RealtimeEngineConfig,
} from '../../dist/types.js';
import { unknownKeyError } from '../_helpers.js';

interface MarkerIdStep {
  set: Array<{ ppq: number; id?: number }>;
  ids?: number[];
  stored: number[];
  refused?: boolean;
}
const MARKER_ID_CASES: Array<{ name: string; steps: MarkerIdStep[] }> = JSON.parse(
  readFileSync(
    new URL('../../../../tests/conformance/marker_id_allocation.json', import.meta.url),
    'utf8',
  ),
).cases;

const SR = 48000;
const BLOCK = 128;

function withEngine(
  body: (engine: RealtimeEngine) => void,
  config?: Partial<RealtimeEngineConfig>,
) {
  const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK, ...config });
  try {
    body(engine);
  } finally {
    engine.destroy();
  }
}

function caught(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  return undefined;
}

const anyEngine = (engine: RealtimeEngine): Record<string, unknown> =>
  engine as unknown as Record<string, unknown>;

const block = (): Float32Array[] => [new Float32Array(BLOCK), new Float32Array(BLOCK)];

describe('RealtimeEngine construction', () => {
  it('takes one config object with a required sampleRate', () => {
    withEngine((engine) => {
      expect(engine.transportState().sampleRate).toBe(SR);
    });
    expect(
      caught(() => new (RealtimeEngine as unknown as new () => RealtimeEngine)()),
    ).toBeInstanceOf(TypeError);
    expect(caught(() => new RealtimeEngine({} as RealtimeEngineConfig))).toBeInstanceOf(TypeError);
    expect(() => new RealtimeEngine({ sampleRate: 48000, maxChannels: 2 }).destroy()).not.toThrow();
  });

  it('refuses an unknown config key by name', () => {
    const error = caught(
      () =>
        new RealtimeEngine({
          sampleRate: SR,
          maxNumChannels: 8,
        } as unknown as RealtimeEngineConfig),
    );
    expect(error).toBeInstanceOf(TypeError);
    expect((error as Error).message).toContain('maxNumChannels');
  });

  it('refuses an explicit 0 on a defaulted field', () => {
    for (const key of ['maxBlockSize', 'commandCapacity', 'telemetryCapacity', 'maxChannels']) {
      const error = caught(
        () => new RealtimeEngine({ sampleRate: SR, [key]: 0 } as unknown as RealtimeEngineConfig),
      );
      expect(error, key).toBeInstanceOf(RangeError);
      expect((error as Error).message).toMatch(
        key === 'maxBlockSize' || key === 'maxChannels'
          ? new RegExp(`${key} must be an integer in \\[1, \\d+\\]$`)
          : /omit it for the default/,
      );
    }
  });

  it('prepares from the same config object', () => {
    withEngine((engine) => {
      engine.prepare({ sampleRate: 44100, maxBlockSize: 256, maxChannels: 8 });
      expect(engine.transportState().sampleRate).toBe(44100);
      expect(
        caught(() => engine.prepare({ maxBlockSize: 256 } as RealtimeEngineConfig)),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() =>
          engine.prepare({ sampleRate: 44100, blockSize: 256 } as unknown as RealtimeEngineConfig),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });
});

describe('RealtimeEngine transport and state reads', () => {
  it('reads the transport through transportState() without isPlaying', () => {
    withEngine((engine) => {
      const state = engine.transportState();
      expect(state.playing).toBe(false);
      expect('isPlaying' in state).toBe(false);
      expect(anyEngine(engine).getTransportState).toBeUndefined();
    });
  });

  it('reads counts and tail as getters', () => {
    withEngine((engine) => {
      for (const name of [
        'tailSamples',
        'automationLaneCount',
        'clipCount',
        'midiInstrumentCount',
        'graphNodeCount',
        'graphConnectionCount',
        'externalMidiDroppedCount',
        'midiInputPendingCount',
        'clipPageRequestOverflowCount',
        'warpStretchOverflowCount',
        'warpVoiceCapacity',
        'clipPagePrefetchFrames',
      ]) {
        expect(typeof anyEngine(engine)[name], name).toBe('number');
      }
      expect(typeof anyEngine(engine).midiCcBindings).toBe('function');
    });
  });

  it('reports latency in fractional samples as the graphLatencySamples getter', () => {
    withEngine((engine) => {
      expect(typeof engine.graphLatencySamples).toBe('number');
      expect(anyEngine(engine).graphLatencySamplesQ8).toBeUndefined();
    });
  });

  it('sets the loop from one object', () => {
    withEngine((engine) => {
      const loop: EngineLoop = { startPpq: 0, endPpq: 4 };
      engine.setLoop(loop);
      engine.flushControlCommands();
      expect(engine.transportState().looping).toBe(true);
      engine.setLoop({ startPpq: 0, endPpq: 4, enabled: false });
      engine.flushControlCommands();
      expect(engine.transportState().looping).toBe(false);
      expect(
        caught(() => engine.setLoop({ startPpq: 0, endPpq: 4, enable: true } as never)),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.setLoop({ startPpq: 0, endPpq: 4, enabled: 1 } as never)),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('lists parameters and markers instead of count and by-index pairs', () => {
    withEngine((engine) => {
      engine.addParameter({ id: 7, name: 'gain', defaultCurve: 'exponential' });
      expect(engine.parameters().map((parameter) => parameter.name)).toEqual(['gain']);
      expect(engine.parameters()[0].defaultCurve).toBe('exponential');
      expect(engine.parameterInfo(7).id).toBe(7);
      for (const name of [
        'parameterCount',
        'parameterInfoByIndex',
        'markerCount',
        'markerByIndex',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('returns the stored markers with their ids from setMarkers', () => {
    withEngine((engine) => {
      const stored = engine.setMarkers([
        { id: 1, ppq: 0, name: 'intro' },
        { id: 2, ppq: 4, name: 'G', kind: 'keySignature', keyFifths: 1, keyMinor: false },
      ]);
      expect(stored.map((marker) => marker.id)).toEqual([1, 2]);
      expect(stored[0].kind).toBe('marker');
      expect(engine.markers()[1].kind).toBe('keySignature');
      expect(engine.marker(2).keyFifths).toBe(1);
    });
  });

  it('refuses out-of-domain fifths on a key signature without replacing the snapshot', () => {
    const check = (engine: RealtimeEngine) => {
      engine.setMarkers([{ id: 1, ppq: 1, kind: 'keySignature', keyFifths: 7 }]);
      const before = engine.markers();
      for (const keyFifths of [-8, 8]) {
        expect(() =>
          engine.setMarkers([{ id: 1, ppq: 2, kind: 'keySignature', keyFifths }]),
        ).toThrow(
          new RangeError(
            'RealtimeEngine.setMarkers: markers[0].keyFifths must be an integer in [-7, 7]',
          ),
        );
        expect(engine.markers()).toEqual(before);
      }
    };
    withEngine(check);
  });

  it('refuses key-signature fields on a non-key marker', () => {
    withEngine((engine) => {
      engine.setMarkers([
        { id: 1, ppq: 0, name: 'key', kind: 'keySignature', keyFifths: 2, keyMinor: true },
      ]);
      const before = engine.markers();

      const fifthsError = caught(() =>
        engine.setMarkers([{ id: 2, ppq: 1, name: 'invalid', kind: 'cuePoint', keyFifths: 1 }]),
      );
      expect(fifthsError).toBeInstanceOf(RangeError);
      expect((fifthsError as Error).message).toBe(
        'RealtimeEngine.setMarkers: markers[0].keyFifths must be 0 unless markers[0].kind is key signature',
      );
      expect(engine.markers()).toEqual(before);

      const minorError = caught(() =>
        engine.setMarkers([{ id: 2, ppq: 1, name: 'invalid', kind: 'cuePoint', keyMinor: true }]),
      );
      expect(minorError).toBeInstanceOf(RangeError);
      expect((minorError as Error).message).toBe(
        'RealtimeEngine.setMarkers: markers[0].keyMinor must be 0 unless markers[0].kind is key signature',
      );
      expect(engine.markers()).toEqual(before);
    });
  });

  it('assigns an id-less marker the lowest free id and never reuses one', () => {
    expect(MARKER_ID_CASES.length).toBeGreaterThan(0);
    for (const { name, steps } of MARKER_ID_CASES) {
      withEngine((engine) => {
        for (const [index, step] of steps.entries()) {
          const label = `${name}, step ${index}`;
          if (step.refused) {
            expect(() => engine.setMarkers(step.set), label).toThrow(RangeError);
          } else {
            expect(
              engine.setMarkers(step.set).map((m) => m.id),
              label,
            ).toEqual(step.ids);
          }
          expect(
            engine.markers().map((m) => m.id),
            label,
          ).toEqual(step.stored);
        }
      });
    }
  });

  it('names a marker kind and refuses an ordinal', () => {
    withEngine((engine) => {
      expect(caught(() => engine.setMarkers([{ id: 1, ppq: 0, kind: 4 as never }]))).toBeInstanceOf(
        TypeError,
      );
      expect(
        caught(() => engine.setMarkers([{ id: 1, ppq: 0, kind: 'chapter' as never }])),
      ).toBeInstanceOf(RangeError);
    });
  });

  it('takes tempo and time-signature segments as Engine*Segment records', () => {
    withEngine((engine) => {
      expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 90 }])).not.toThrow();
      expect(() =>
        engine.setTimeSignatureSegments([{ startPpq: 0, numerator: 3, denominator: 4 }]),
      ).not.toThrow();
    });
  });
});

describe('RealtimeEngine metronome', () => {
  it('spells the click length clickSec and reports only the spelling in effect', () => {
    withEngine((engine) => {
      engine.setMetronome({ enabled: true, clickSec: 0.01 });
      const bySec = engine.metronome();
      expect(bySec.clickSec).toBeCloseTo(0.01, 6);
      expect(bySec.clickSamples).toBeUndefined();
      engine.setMetronome({ enabled: true, clickSamples: 16 });
      const bySamples = engine.metronome();
      expect(bySamples.clickSamples).toBe(16);
      expect(bySamples.clickSec).toBeUndefined();
      engine.setMetronome({ enabled: true });
      expect(engine.metronome().clickSamples).toBeUndefined();
      // The engine default click (0.002 s) is the spelling in effect.
      expect(engine.metronome().clickSec).toBeCloseTo(0.002, 6);
    });
  });

  it('refuses an explicit 0 click length and both spellings together', () => {
    withEngine((engine) => {
      expect(caught(() => engine.setMetronome({ enabled: true, clickSamples: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.setMetronome({ enabled: true, clickSec: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(
        caught(() => engine.setMetronome({ enabled: true, clickSamples: 16, clickSec: 0.01 })),
      ).toBeInstanceOf(RangeError);
      expect(
        caught(() => engine.setMetronome({ enabled: true, clickSeconds: 0.01 } as never)),
      ).toBeInstanceOf(TypeError);
    });
  });
});

describe('RealtimeEngine telemetry', () => {
  it('names the record type and error and reports latency in samples', () => {
    withEngine((engine) => {
      engine.play();
      engine.process(block());
      const records = engine.drainTelemetry();
      expect(records.length).toBeGreaterThan(0);
      const last = records[records.length - 1];
      expect(last.type).toBe('processBlock');
      expect(last.error).toBe('none');
      expect(typeof last.graphLatencySamples).toBe('number');
      expect('graphLatencySamplesQ8' in last).toBe(false);
    });
  });
});

describe('RealtimeEngine closed sets are names', () => {
  it('maps an automation curve name and refuses a number', () => {
    withEngine((engine) => {
      engine.addParameter({ id: 9, name: 'gain' });
      expect(() =>
        engine.setAutomationLane(9, [{ ppq: 0, value: 0, curveToNext: 'hold' }]),
      ).not.toThrow();
      expect(
        caught(() => engine.setAutomationLane(9, [{ ppq: 0, value: 0, curveToNext: 1 as never }])),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() =>
          engine.setAutomationLane(9, [{ ppq: 0, value: 0, curveToNext: 'wavy' as never }]),
        ),
      ).toBeInstanceOf(RangeError);
      expect(
        caught(() => engine.addParameter({ id: 10, name: 'pan', defaultCurve: 2 as never })),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('names graph node types and mixing intents', () => {
    withEngine((engine) => {
      const spec = {
        nodes: [
          { id: 'in', numPorts: 2 },
          { id: 'gain', type: 'gain' as const, gainDb: 0, numPorts: 2 },
          { id: 'out', numPorts: 2 },
        ],
        connections: [
          { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0, mix: 'add' as const },
          { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
          {
            sourceNode: 'gain',
            sourcePort: 0,
            destNode: 'out',
            destPort: 0,
            mix: 'replace' as const,
          },
          { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
        ],
        inputNode: 'in',
        outputNode: 'out',
        numChannels: 2,
      };
      expect(() => engine.setGraph(spec)).not.toThrow();
      expect(engine.graphNodeCount).toBe(3);
      const numeric = {
        ...spec,
        nodes: [{ id: 'in', type: 1 as never, numPorts: 2 }, ...spec.nodes.slice(1)],
      };
      expect(caught(() => engine.setGraph(numeric))).toBeInstanceOf(TypeError);
      const numericMix = {
        ...spec,
        connections: [{ ...spec.connections[0], mix: 1 as never }],
      };
      expect(caught(() => engine.setGraph(numericMix))).toBeInstanceOf(TypeError);
      const unknown = {
        ...spec,
        connections: [{ ...spec.connections[0], mix: 'sum' as never }],
      };
      expect(caught(() => engine.setGraph(unknown))).toBeInstanceOf(RangeError);
    });
  });

  it('EngineGraphNodeType is a named token set', () => {
    expectTypeOf<EngineGraphNodeType>().toEqualTypeOf<'passThrough' | 'gain'>();
  });

  it('EngineGraphMix is a named token set', () => {
    expectTypeOf<EngineGraphMix>().toEqualTypeOf<'replace' | 'add'>();
  });

  it('EngineClip.pageProvider requires a ClipPageProvider instance', () => {
    expectTypeOf<EngineClip['pageProvider']>().toEqualTypeOf<ClipPageProvider | undefined>();
    withEngine((engine) => {
      for (const pageProvider of [7, { id: 7 }]) {
        const refuse = () =>
          engine.setClips([{ id: 1, startPpq: 0, pageProvider: pageProvider as never }]);
        expect(refuse).toThrow(TypeError);
        expect(refuse).toThrow(/pageProvider must be a ClipPageProvider instance/);
      }
    });
  });

  it('names a warp mode and refuses an ordinal', () => {
    withEngine((engine) => {
      const clip = (warpMode: unknown) => ({
        id: 1,
        channels: [new Float32Array(BLOCK)],
        startPpq: 0,
        lengthSamples: BLOCK,
        warpMode: warpMode as never,
      });
      expect(() => engine.setClips([clip('off')])).not.toThrow();
      expect(caught(() => engine.setClips([clip(1)]))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setClips([clip('tempo-sync')]))).toBeInstanceOf(RangeError);
    });
  });

  it('names channel layouts, send timings and refuses ordinals', () => {
    withEngine((engine) => {
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: '5.1' }])).not.toThrow();
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 'stereo', outputBusId: 1 }]),
      ).not.toThrow();
      expect(
        caught(() => engine.setTrackBuses([{ busId: 1, channelLayout: 2 as never }])),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.setTrackBuses([{ busId: 1, channelLayout: 'quad' as never }])),
      ).toBeInstanceOf(RangeError);
      expect(
        caught(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 1 as never }])),
      ).toBeInstanceOf(TypeError);
      engine.setTrackBuses([{ busId: 1 }, { busId: 2 }]);
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, sendTiming: 'preFader' }] }]),
      ).not.toThrow();
      expect(
        caught(() =>
          engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, sendTiming: 1 as never }] }]),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('refuses outputBusId 0 and takes null for the master mix', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      expect(caught(() => engine.setTrackLanes([{ trackId: 10, outputBusId: 0 }]))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.setTrackBuses([{ busId: 1, outputBusId: 0 }]))).toBeInstanceOf(
        RangeError,
      );
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      expect(() => engine.setTrackOutputBus(10, null)).not.toThrow();
      expect(() => engine.setTrackOutputBus(10, 1)).not.toThrow();
      expect(caught(() => engine.setTrackOutputBus(10, 0))).toBeInstanceOf(RangeError);
    });
  });

  it('refuses an unknown lane or bus key and a bare number lane', () => {
    withEngine((engine) => {
      expect(caught(() => engine.setTrackLanes([{ trackId: 10, bus: 1 } as never]))).toBeInstanceOf(
        TypeError,
      );
      expect(caught(() => engine.setTrackLanes([10 as never]))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setTrackBuses([{ busId: 1, gain: 0 } as never]))).toBeInstanceOf(
        TypeError,
      );
    });
  });
  it('takes every synth closed set by name and refuses a number, in range or not', () => {
    withEngine((engine) => {
      const patch = {
        engineMode: 'subtractive' as const,
        waveform: 'saw' as const,
        filterModel: 'svf' as const,
        modRoutings: [{ source: 'lfo1' as const, destination: 'cutoffCents' as const, depth: 100 }],
      };
      expect(() => engine.setSynthInstrument(3, patch)).not.toThrow();
      for (const field of ['engineMode', 'waveform', 'filterModel', 'lfoShape', 'sampleLoop']) {
        const refused = caught(() =>
          engine.setSynthInstrument(3, { ...patch, [field]: 1 } as never),
        );
        expect(refused, field).toBeInstanceOf(TypeError);
        expect(String(refused)).toContain(field);
      }
      for (const end of ['source', 'destination'] as const) {
        const routing = { ...patch.modRoutings[0], [end]: 2 };
        expect(
          caught(() => engine.setSynthInstrument(3, { ...patch, modRoutings: [routing] } as never)),
          end,
        ).toBeInstanceOf(TypeError);
      }
      expect(
        caught(() => engine.setSynthInstrument(3, { ...patch, engineMode: 'analog' } as never)),
      ).toBeInstanceOf(RangeError);
      expect(() => engine.setBuiltinInstrument(4, { waveform: 'sawtooth' })).not.toThrow();
      expect(caught(() => engine.setBuiltinInstrument(4, { waveform: 1 as never }))).toBeInstanceOf(
        TypeError,
      );
      const binding = { input: 'channelPressure', axis: 'brightness' } as const;
      expect(() => engine.bindController(3, binding)).not.toThrow();
      for (const field of ['input', 'axis'] as const) {
        expect(
          caught(() => engine.bindController(3, { ...binding, [field]: 1 } as never)),
          field,
        ).toBeInstanceOf(TypeError);
      }
    });
    const bank = new SampleBank();
    try {
      const pcm = new Float32Array(256);
      expect(() => bank.addSample(pcm, { loopMode: 'keyDown' })).not.toThrow();
      expect(caught(() => bank.addSample(pcm, { loopMode: 3 as never }))).toBeInstanceOf(TypeError);
    } finally {
      bank.destroy();
    }
  });
});

describe('RealtimeEngine strip addressing', () => {
  const strip = (id: string): MixSceneStrip => ({ id }) as unknown as MixSceneStrip;

  it('addresses a strip by track id or master and refuses any other string', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(10, strip('s'));
      expect(() => engine.setStripPan(10, 0.25)).not.toThrow();
      for (const ref of ['10', 'track-10', '']) {
        expect(
          caught(() => engine.setStripPan(ref, 0)),
          ref,
        ).toBeInstanceOf(RangeError);
        expect(
          caught(() => engine.setStrip(ref, strip('s'))),
          ref,
        ).toBeInstanceOf(RangeError);
      }
      expect(caught(() => engine.setStripPan(true as never, 0))).toBeInstanceOf(TypeError);
      // The master strip has no pan setter here: its pan is an automation id.
      expect(caught(() => engine.setStripPan('master', 0))).toBeInstanceOf(RangeError);
    });
  });

  it('replaces a strip from an object, not a JSON string', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setStrip(10, strip('s'))).not.toThrow();
      expect(() => engine.setStrip('master', strip('master'))).not.toThrow();
      expect(caught(() => engine.setStrip(10, '{"strips":[]}' as never))).toBeInstanceOf(TypeError);
      for (const name of ['setTrackStripJson', 'setMasterStripJson', 'setBusStripJson']) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('takes pan and send names in a scene strip and refuses their ordinals', () => {
    withEngine((engine) => {
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
    });
  });

  it('replaces a bus strip and one band of it', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      expect(() => engine.setBusStrip(1, { id: '1', role: 'aux', inserts: [] })).not.toThrow();
      expect(caught(() => engine.setBusStrip(1, null as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setBusEqBand(1, 0, '{}' as never))).toBeInstanceOf(TypeError);
      for (const name of [
        'setBusStripEqBand',
        'setBusStripEqBandJson',
        'setTrackStripEqBandJson',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('sets one EQ band from an object only', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(10, strip('s'));
      expect(caught(() => engine.setStripEqBand(10, 0, '{"type":"peak"}' as never))).toBeInstanceOf(
        TypeError,
      );
      expect(anyEngine(engine).setTrackStripEqBand).toBeUndefined();
      expect(anyEngine(engine).setMasterStripEqBand).toBeUndefined();
    });
  });

  it('bypasses an insert from a labelled state object', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(10, strip('s'));
      expect(caught(() => engine.setStripInsertBypassed(10, 0, {} as never))).toBeInstanceOf(
        TypeError,
      );
      expect(caught(() => engine.setStripInsertBypassed(10, 0, true as never))).toBeInstanceOf(
        TypeError,
      );
      expect(
        caught(() =>
          engine.setStripInsertBypassed(10, 0, { bypassed: true, reset: true } as never),
        ),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.setBusInsertBypassed(1, 0, { bypassed: 1 } as never)),
      ).toBeInstanceOf(TypeError);
      for (const name of [
        'setTrackStripInsertBypassed',
        'setMasterStripInsertBypassed',
        'setBusStripInsertBypassed',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('sets, applies and restores an insert parameter from one { name, value } object', () => {
    withEngine((engine) => {
      for (const call of [
        () => engine.setStripInsertParam(10, 0, { name: 'gainDb', value: 1, extra: 1 } as never),
        () => engine.applyStripInsertParamNow(10, 0, { name: 3, value: 1 } as never),
        () => engine.restoreStripInsertParam(10, 0, { name: 'gainDb' } as never),
        () => engine.setBusInsertParam(1, 0, 'gainDb' as never),
        () => engine.applyBusInsertParamNow(1, 0, { name: 'gainDb', value: Number.NaN }),
        () => engine.restoreBusInsertParam(1, 0, null as never),
      ]) {
        const error = caught(call);
        expect(error instanceof TypeError || error instanceof RangeError).toBe(true);
      }
      for (const name of [
        'setTrackStripInsertParamByName',
        'setMasterStripInsertParamByName',
        'setBusStripInsertParamByName',
        'applyTrackStripInsertParamByNameNow',
        'applyMasterStripInsertParamByNameNow',
        'applyBusStripInsertParamByNameNow',
        'restoreTrackStripInsertParamByName',
        'restoreMasterStripInsertParamByName',
        'restoreBusStripInsertParamByName',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('clears insert parameter bases and resolves automation ids by strip', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.clearStripInsertParameterBases(10)).not.toThrow();
      expect(typeof engine.resolveStripAutomationId(10, 'faderDb')).toBe('number');
      expect(typeof engine.resolveStripAutomationId(10, 'pan')).toBe('number');
      expect(typeof engine.resolveStripAutomationId('master', 'width')).toBe('number');
      expect(caught(() => engine.resolveStripAutomationId(10, 'width'))).toBeInstanceOf(RangeError);
      expect(caught(() => engine.resolveStripAutomationId('10', 'faderDb'))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.resolveStripAutomationId(10, 'gain' as never))).toBeInstanceOf(
        RangeError,
      );
      for (const name of [
        'clearTrackInsertParameterBases',
        'clearMasterInsertParameterBases',
        'resolveTrackInsertAutomationId',
        'resolveMasterInsertAutomationId',
        'resolveTrackLaneAutomationId',
        'resolveMasterAutomationId',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('sets a strip fader and a bus gain in decibels', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setStripFaderDb(10, -6)).not.toThrow();
      expect(() => engine.setStripFaderDb('master', -3, 0)).not.toThrow();
      expect(() => engine.setBusGainDb(1, -6)).not.toThrow();
      expect(caught(() => engine.setStripFaderDb(10, -6, -1))).toBeInstanceOf(RangeError);
    });
  });

  it('names the pan family by strip and bus', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(10, strip('s'));
      engine.setBusStrip(1, { id: '1', role: 'aux', inserts: [] });
      expect(() => engine.setStripDualPan(10, -1, 1)).not.toThrow();
      expect(() => engine.setStripChannelDelaySamples(10, 0)).not.toThrow();
      expect(() => engine.setBusPan(1, 0.5)).not.toThrow();
      expect(() => engine.setBusDualPan(1, -1, 1)).not.toThrow();
      expect(caught(() => engine.setStripPanLaw(10, 1 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setStripPanMode(10, 1 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setBusPanLaw(1, 1 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setBusPanMode(1, 1 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setStripSurroundPan('master', {}))).toBeInstanceOf(RangeError);
      for (const key of ['elevation', 'distance']) {
        expect(() => engine.setStripSurroundPan(10, { [key]: 0 } as never)).toThrow(
          unknownKeyError(`RealtimeEngine.setStripSurroundPan: unknown key ${key}`),
        );
      }
      for (const name of [
        'setTrackStripPan',
        'setTrackStripPanLaw',
        'setTrackStripPanMode',
        'setTrackStripDualPan',
        'setTrackStripSurroundPan',
        'setTrackStripChannelDelaySamples',
        'setBusStripPan',
        'setBusStripPanLaw',
        'setBusStripPanMode',
        'setBusStripDualPan',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });
});

describe('RealtimeEngine track lanes, solo and sidechain', () => {
  it('keys solo/mute and monitor mode by track id', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 20 }, { trackId: 10 }]);
      expect(() => engine.setTrackSoloMute(10, { solo: true, mute: false })).not.toThrow();
      expect(() => engine.setTrackMonitorMode(20, 'pfl')).not.toThrow();
      expect(caught(() => engine.setTrackSoloMute(99, { solo: true, mute: false }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.setTrackMonitorMode(99, 'pfl'))).toBeInstanceOf(RangeError);
      expect(caught(() => engine.setTrackSoloMute(10, { solo: true } as never))).toBeInstanceOf(
        TypeError,
      );
      expect(
        caught(() => engine.setTrackSoloMute(10, { solo: true, mute: false, cue: true } as never)),
      ).toBeInstanceOf(TypeError);
      expect(anyEngine(engine).setSoloMute).toBeUndefined();
    });
  });

  it('names the monitor mode and refuses a number', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setTrackMonitorMode(10, 'afl')).not.toThrow();
      expect(caught(() => engine.setTrackMonitorMode(10, 2 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setTrackMonitorMode(10, 'solo' as never))).toBeInstanceOf(
        RangeError,
      );
    });
  });

  it('keys a sidechain by strip and bus with one source object', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 30 }]);
      // Id 0 is the native "no binding": removal is null, and 0 is refused.
      expect(caught(() => engine.setBusSidechain(1, 0, { kind: 'track', id: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.setStripSidechain(10, 0, { kind: 'bus', id: 1 }))).toBeInstanceOf(
        RangeError,
      );
      expect(
        caught(() => engine.setStripSidechain(10, 0, { kind: 2 as never, id: 30 })),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.setStripSidechain('master', 0, { kind: 'sideways' as never, id: 30 })),
      ).toBeInstanceOf(RangeError);
      expect(engine.canSetStripSidechain(0, 0, { kind: 'track', id: 10 }).ok).toBe(false);
      expect(engine.canSetBusSidechain(9, 0, { kind: 'track', id: 30 }).ok).toBe(false);
      expect(caught(() => engine.canSetBusSidechain(1, 0, null as never))).toBeInstanceOf(
        TypeError,
      );
      for (const name of [
        'setLaneSidechain',
        'setMasterSidechain',
        'canSetLaneSidechain',
        'canSetMasterSidechain',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });
});

describe('RealtimeEngine capture', () => {
  it('configures the whole capture setup in one call', () => {
    withEngine((engine) => {
      engine.configureCapture({
        capacityFrames: 256,
        numChannels: 2,
        source: 'input',
        recordOffsetSamples: -16,
        punch: { startSample: 48000, endSec: 1.01 },
      });
      const status = engine.captureStatus();
      expect(status.source).toBe('input');
      expect(status.recordOffsetSamples).toBe(-16);
      expect(status.punchEnabled).toBe(true);
      // An absent punch is no punch.
      engine.configureCapture({ capacityFrames: 256 });
      expect(engine.captureStatus().punchEnabled).toBe(false);
      expect(engine.captureStatus().source).toBe('output');
      for (const name of [
        'setCaptureBuffer',
        'setCapturePunch',
        'setCaptureSource',
        'setRecordOffsetSamples',
      ]) {
        expect(anyEngine(engine)[name], name).toBeUndefined();
      }
    });
  });

  it('refuses a bad capture request by class', () => {
    withEngine((engine) => {
      expect(caught(() => engine.configureCapture({} as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.configureCapture({ capacityFrames: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(
        caught(() => engine.configureCapture({ capacityFrames: 64, numChannels: 0 })),
      ).toBeInstanceOf(RangeError);
      expect(
        caught(() => engine.configureCapture({ capacityFrames: 64, channels: 2 } as never)),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.configureCapture({ capacityFrames: 64, source: 0 as never })),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.configureCapture({ capacityFrames: 64, source: 'line' as never })),
      ).toBeInstanceOf(RangeError);
    });
  });

  it('takes the punch window as exactly one start and one end spelling', () => {
    withEngine((engine) => {
      const configure = (punch: object) =>
        engine.configureCapture({ capacityFrames: 64, punch: punch as never });
      expect(() => configure({ startSample: 0, endSample: 48 })).not.toThrow();
      expect(() => configure({ startSec: 0, endSec: 0.5 })).not.toThrow();
      expect(() => configure({ startPpq: 0, endPpq: 4 })).not.toThrow();
      expect(
        caught(() => configure({ startSample: 0, startSec: 0, endSample: 48 })),
      ).toBeInstanceOf(RangeError);
      expect(() => configure({ startSample: 0 })).toThrow(
        new TypeError(
          'RealtimeEngine.configureCapture: give punch.endPpq, punch.endSample or punch.endSec',
        ),
      );
      expect(caught(() => configure({ startSample: -1, endSample: 2 }))).toBeInstanceOf(RangeError);
      expect(
        caught(() => configure({ startSample: 0, endSample: 48, enabled: false })),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('refuses a resize while armed with a coded InvalidState', () => {
    withEngine((engine) => {
      engine.configureCapture({ capacityFrames: 64 });
      engine.armCapture({ armed: true });
      expect(() => engine.configureCapture({ capacityFrames: 64, source: 'output' })).not.toThrow();
      const error = caught(() => engine.configureCapture({ capacityFrames: 128 }));
      expect(isSonareError(error)).toBe(true);
      expect((error as { code: number }).code).toBe(ErrorCode.InvalidState);
    });
  });

  it('arms from a labelled state object', () => {
    withEngine((engine) => {
      engine.configureCapture({ capacityFrames: 64 });
      engine.armCapture({ armed: true });
      expect(engine.captureStatus().armed).toBe(true);
      engine.armCapture({ armed: false });
      expect(engine.captureStatus().armed).toBe(false);
      expect(caught(() => engine.armCapture(true as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.armCapture({} as never))).toBeInstanceOf(TypeError);
    });
  });
});

describe('RealtimeEngine offline render and bounce', () => {
  it('renders from a request object only', () => {
    withEngine((engine) => {
      engine.play();
      const rendered = engine.renderOffline({ channels: block(), blockSize: BLOCK });
      expect(rendered).toHaveLength(2);
      expect(caught(() => engine.renderOffline(block() as never))).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.renderOffline({ channels: block(), sampleRate: SR } as never)),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('scans the input for non-finite samples and refuses a validate key', () => {
    withEngine((engine) => {
      const bad = block();
      bad[0][3] = Number.NaN;
      expect(caught(() => engine.renderOffline({ channels: bad }))).toBeInstanceOf(RangeError);
      expect(() => engine.renderOffline({ channels: block(), validate: false } as never)).toThrow(
        unknownKeyError('RealtimeEngine.renderOffline: unknown key validate'),
      );
    });
  });

  const bounce = (engine: RealtimeEngine, options: object) =>
    engine.bounceOffline({
      totalFrames: 256,
      blockSize: BLOCK,
      numChannels: 2,
      ...options,
    } as never);

  it('treats an explicit 0 on a rate as an error and absence as the default', () => {
    withEngine((engine) => {
      expect(bounce(engine, {}).sampleRate).toBe(SR);
      expect(caught(() => bounce(engine, { targetSampleRate: 0 }))).toBeInstanceOf(RangeError);
      expect(caught(() => bounce(engine, { sourceSampleRate: 0 }))).toBeInstanceOf(RangeError);
    });
  });

  it('reads targetLufs 0 as a literal target, absence as -14 and a non-finite value as an error', () => {
    withEngine((engine) => {
      expect(() => bounce(engine, { normalizeLufs: true })).not.toThrow();
      expect(() => bounce(engine, { normalizeLufs: true, targetLufs: 0 })).not.toThrow();
      expect(
        caught(() => bounce(engine, { normalizeLufs: true, targetLufs: Number.NaN })),
      ).toBeInstanceOf(RangeError);
    });
  });

  it('names the dither and refuses a number', () => {
    withEngine((engine) => {
      for (const dither of ['rpdf', 'tpdf', 'noiseShaped']) {
        expect(
          () => bounce(engine, { dither, ditherBits: 8, ditherSeed: 1 }),
          dither,
        ).not.toThrow();
      }
      expect(caught(() => bounce(engine, { dither: 1 }))).toBeInstanceOf(TypeError);
      expect(caught(() => bounce(engine, { dither: 'triangle' }))).toBeInstanceOf(RangeError);
      expect(caught(() => bounce(engine, { ditherDepth: 8 }))).toBeInstanceOf(TypeError);
    });
  });
});

describe('RealtimeEngine instruments and MIDI', () => {
  it('binds an instrument to a destination id given first', () => {
    withEngine((engine) => {
      engine.setBuiltinInstrument(5, { gain: 0.5 });
      engine.setSynthInstrument(6, {});
      engine.setSf2Instrument(7, {});
      expect(engine.midiInstrumentCount).toBe(3);
      engine.setBuiltinInstrument(8);
      expect(engine.midiInstrumentCount).toBe(4);
    });
  });

  it('refuses a destinationId repeated inside the config', () => {
    withEngine((engine) => {
      for (const call of [
        () => engine.setBuiltinInstrument(5, { destinationId: 5 } as never),
        () => engine.setSynthInstrument(5, { destinationId: 5 } as never),
        () => engine.setSf2Instrument(5, { destinationId: 5 } as never),
      ]) {
        const error = caught(call);
        expect(error).toBeInstanceOf(TypeError);
        expect((error as Error).message).toContain('destinationId');
      }
    });
  });

  it('takes the part rig as a request object with a required part', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(3, {});
      expect(
        caught(() => engine.setPartRig({ destinationId: 3, mode: 'bank' } as never)),
      ).toBeInstanceOf(TypeError);
      expect(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 'none' })).not.toThrow();
      expect(
        caught(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 1 as never })),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 'bank', x: 1 } as never)),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => (engine.setPartRig as (...a: unknown[]) => void)(3, 0, 'none')),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() =>
          engine.setPartRig({
            destinationId: 3,
            part: 0,
            mode: 'chain',
            inserts: [{ processor: 'saturation.overdrive', params: '{}' as never }],
          }),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('PartRigRequest.mode excludes numeric ordinals', () => {
    expectTypeOf<PartRigRequest['mode']>().toEqualTypeOf<PartRigMode>();
  });

  it('ProjectMidiCcBinding uses named controller-kind tokens', () => {
    expectTypeOf<ProjectMidiCcBinding['kind']>().toEqualTypeOf<
      'controlChange7' | 'controlChange14' | 'rpn' | 'nrpn' | undefined
    >();
    withEngine((engine) => {
      engine.addParameter({ id: 1, name: 'gain' });
      expect(() => engine.bindMidiCc({ ccNumber: 7, paramId: 1, kind: 0 } as never)).toThrow(
        TypeError,
      );
      expect(() =>
        engine.bindMidiCc({ ccNumber: 7, paramId: 1, kind: 'controlChange7' }),
      ).not.toThrow();
    });
  });

  it('reads and rebinds detached MIDI CC descriptors with consume state', () => {
    withEngine((engine) => {
      engine.bindMidiCc({ ccNumber: 7, paramId: 5 });
      engine.bindMidiCc({
        ccNumber: 10,
        channel: 2,
        kind: 'rpn',
        selectorMsb: 1,
        selectorLsb: 3,
        paramId: 6,
        minValue: -2,
        maxValue: 8,
        consume: true,
      });
      const byCcNumber = (a: { ccNumber: number }, b: { ccNumber: number }) =>
        a.ccNumber - b.ccNumber;
      const bindings = engine.midiCcBindings().sort(byCcNumber);
      expect(Object.hasOwn(bindings[0], 'kind')).toBe(true);
      expect(typeof bindings[0].kind).toBe('string');
      expect(bindings).toEqual([
        {
          ccNumber: 7,
          kind: 'controlChange7',
          ccLsbNumber: 0,
          selectorMsb: 0,
          selectorLsb: 0,
          paramId: 5,
          minValue: 0,
          maxValue: 1,
          consume: false,
        },
        {
          ccNumber: 10,
          channel: 2,
          kind: 'rpn',
          ccLsbNumber: 0,
          selectorMsb: 1,
          selectorLsb: 3,
          paramId: 6,
          minValue: -2,
          maxValue: 8,
          consume: true,
        },
      ]);

      for (const binding of bindings) {
        engine.bindMidiCc(binding);
      }
      expect(engine.midiCcBindings().sort(byCcNumber)).toEqual(bindings);
      bindings.length = 0;
      expect(engine.midiCcBindings().length).toBe(2);
      expect(engine.midiCcBindings()).toHaveLength(2);
    });
  });

  it('binds one CC descriptor with bindMidiCc', () => {
    withEngine((engine) => {
      expect(anyEngine(engine).midiCcBindingCount).toBeUndefined();
      engine.addParameter({ id: 4, name: 'cutoff' });
      engine.bindMidiCc({ ccNumber: 74, paramId: 4, minValue: 0, maxValue: 1 });
      expect(engine.midiCcBindings()).toHaveLength(1);
      const bindings = engine.midiCcBindings();
      expect(bindings).toHaveLength(1);
      expect('channel' in bindings[0]).toBe(false);
      expect(bindings[0].channel).toBeUndefined();
      expect(bindings[0]).toMatchObject({
        ccNumber: 74,
        kind: 'controlChange7',
        paramId: 4,
        consume: false,
      });
      engine.clearMidiCcBindings();
      engine.bindMidiCc(bindings[0]);
      expect(engine.midiCcBindings()).toEqual(bindings);

      engine.clearMidiCcBindings();
      engine.bindMidiCc({ ccNumber: 75, paramId: 4, consume: true });
      const consumedBinding = engine.midiCcBindings();
      expect(consumedBinding).toMatchObject([{ ccNumber: 75, consume: true }]);
      engine.clearMidiCcBindings();
      engine.bindMidiCc(consumedBinding[0]);
      expect(engine.midiCcBindings()).toEqual(consumedBinding);
      expect(
        caught(() =>
          engine.bindMidiCc({ ccNumber: 75, paramId: 4, consume: 'yes' as unknown as boolean }),
        ),
      ).toBeInstanceOf(TypeError);
      expect(anyEngine(engine).bindMidiCcBinding).toBeUndefined();
      expect(
        caught(() => (engine.bindMidiCc as (...a: unknown[]) => void)(0, 74, 4)),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('pushes a program change with an optional bank object', () => {
    withEngine((engine) => {
      engine.setSf2Instrument(3, {});
      expect(() => engine.pushMidiProgram(3, 0, 0, 5)).not.toThrow();
      expect(() => engine.pushMidiProgram(3, 0, 0, 5, { msb: 8, lsb: 1 })).not.toThrow();
      expect(() => engine.pushMidiProgram(3, 0, 0, 5, undefined, 0)).not.toThrow();
      engine.setMidiInputSource(3);
      expect(() => engine.pushMidiInputProgram(0, 0, 5, { msb: 8, lsb: 1 })).not.toThrow();
      expect(caught(() => engine.pushMidiProgram(3, 0, 0, 5, { msb: 8 } as never))).toBeInstanceOf(
        TypeError,
      );
      expect(caught(() => engine.pushMidiProgram(3, 0, 0, 5, { msb: 128, lsb: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.pushMidiInputProgram(0, 0, 5, true as never))).toBeInstanceOf(
        TypeError,
      );
    });
  });

  it('refuses an ordinal for an articulation and a note-tracking name', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(3, { engineMode: 'karplusStrong' });
      expect(caught(() => engine.setArticulation(3, 0, 1 as never))).toBeInstanceOf(TypeError);
      expect(caught(() => engine.setControllerNoteTracking(3, 0 as never, 'last'))).toBeInstanceOf(
        TypeError,
      );
      expect(caught(() => engine.setControllerNoteTracking(3, 'bend', 1 as never))).toBeInstanceOf(
        TypeError,
      );
      expect(
        caught(() => engine.bindController(3, { input: 1 as never, axis: 'excitation' })),
      ).toBeInstanceOf(TypeError);
      expect(
        caught(() => engine.bindController(3, { input: 'controlChange', axis: 2 as never })),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('keeps the single-boolean setters positional under the property name', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(3, {});
      expect(() => engine.setMidiDestinationExternal(3, true)).not.toThrow();
      expect(() => engine.setExternalMidiClockEnabled(true)).not.toThrow();
      expect(() => engine.setControllerVelocityMeaningful(3, true)).not.toThrow();
    });
  });
});

describe('RealtimeEngine external MIDI events', () => {
  it('drains ExternalMidiEvent records', () => {
    withEngine((engine) => {
      engine.setMidiDestinationExternal(3, true);
      expect(engine.drainExternalMidi()).toEqual([]);
    });
  });
});
