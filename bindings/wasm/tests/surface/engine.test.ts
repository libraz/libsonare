import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  PART_RIG_ALL_PARTS,
  RealtimeEngine,
  SampleBank,
} from '../../dist/index.js';
import { unknownKeyError } from '../_helpers';

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

const BLOCK = 128;

function newEngine(maxBlockSize = BLOCK): RealtimeEngine {
  return new RealtimeEngine({ sampleRate: 48000, maxBlockSize });
}

function withEngine(fn: (engine: RealtimeEngine) => void, maxBlockSize = BLOCK): void {
  const engine = newEngine(maxBlockSize);
  try {
    fn(engine);
  } finally {
    engine.destroy();
  }
}

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

/** The engine as an untyped bag, for old names and wrong-typed arguments. */
function loose(engine: RealtimeEngine): Record<string, (...args: unknown[]) => unknown> {
  return engine as unknown as Record<string, (...args: unknown[]) => unknown>;
}

beforeAll(async () => {
  await init();
});

describe('construction', () => {
  it('constructs from one config object', () => {
    expect(
      () => new (RealtimeEngine as unknown as new (...a: unknown[]) => unknown)(48000, 128),
    ).toThrow(TypeError);
    expect(() => new RealtimeEngine({} as never)).toThrow(TypeError);
    expect(() => new RealtimeEngine({ sampleRate: 48000, maxNumChannels: 8 } as never)).toThrow(
      /maxNumChannels/,
    );
    expect(() => new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 0 })).toThrow(
      /maxBlockSize must be an integer in \[1, 1048576\]$/,
    );
    expect(() => new RealtimeEngine({ sampleRate: Number.NaN })).toThrow(RangeError);
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      expect(engine.transportState().sampleRate).toBe(48000);
    } finally {
      engine.destroy();
    }
  });

  it('refuses an explicit 0 for a count field', () => {
    for (const field of ['maxBlockSize', 'commandCapacity', 'telemetryCapacity', 'maxChannels']) {
      const error = caught(() => new RealtimeEngine({ sampleRate: 48000, [field]: 0 } as never));
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toContain(field);
    }
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 64,
      commandCapacity: 256,
      telemetryCapacity: 256,
      maxChannels: 8,
    });
    engine.destroy();
  });

  it('prepares from the same config object', () => {
    withEngine((engine) => {
      expect(() => engine.prepare({ sampleRate: 44100, maxBlockSize: 256 })).not.toThrow();
      expect(engine.transportState().sampleRate).toBe(44100);
      expect(() => engine.prepare(48000 as never)).toThrow(TypeError);
      expect(() => engine.prepare({ sampleRate: 48000, blockSize: 128 } as never)).toThrow(
        /blockSize/,
      );
      expect(() => engine.prepare({ sampleRate: 48000, maxBlockSize: 0 })).toThrow(RangeError);
    });
  });
});

describe('transport and counts', () => {
  it('reads the transport state with transportState()', () => {
    withEngine((engine) => {
      const state = engine.transportState();
      expect(state.playing).toBe(false);
      expect(state.timeSignature.numerator).toBeGreaterThan(0);
      expect('getTransportState' in engine).toBe(false);
    });
  });

  it('reads graph latency in samples as a getter', () => {
    withEngine((engine) => {
      expect(typeof engine.graphLatencySamples).toBe('number');
      expect(engine.graphLatencySamples).toBe(0);
      expect('graphLatencySamplesQ8' in engine).toBe(false);
    });
  });

  it('reads cheap scalar state as getters', () => {
    const names = [
      'tailSamples',
      'automationLaneCount',
      'clipCount',
      'midiInstrumentCount',
      'midiCcBindingCount',
      'graphNodeCount',
      'graphConnectionCount',
      'externalMidiDroppedCount',
      'externalMidiPendingCount',
      'midiInputPendingCount',
      'clipPageRequestOverflowCount',
      'warpStretchOverflowCount',
      'warpVoiceCapacity',
      'clipPagePrefetchFrames',
    ] as const;
    withEngine((engine) => {
      for (const name of names) {
        expect(
          Object.getOwnPropertyDescriptor(RealtimeEngine.prototype, name)?.get,
          name,
        ).toBeTypeOf('function');
        expect(typeof engine[name], name).toBe('number');
      }
      expect(engine.warpVoiceCapacity).toBe(8);
      expect(engine.clipCount).toBe(0);
    });
  });

  it('takes the loop as one object', () => {
    withEngine((engine) => {
      expect(() => engine.setLoop({ startPpq: 0, endPpq: 4 })).not.toThrow();
      expect(() => engine.setLoop({ startPpq: 0, endPpq: 4, enabled: false })).not.toThrow();
      expect(() => loose(engine).setLoop(0, 4)).toThrow(TypeError);
      expect(() => engine.setLoop({ startPpq: 0, endPpq: 4, enabled: 1 as never })).toThrow(
        TypeError,
      );
      expect(() => engine.setLoop({ startPpq: 0, endPpq: 4, loop: true } as never)).toThrow(/loop/);
    });
  });
});

describe('collections', () => {
  it('lists parameters as an array', () => {
    withEngine((engine) => {
      expect(engine.parameters()).toEqual([]);
      engine.addParameter({
        id: 7,
        name: 'gain',
        minValue: -60,
        maxValue: 12,
        defaultCurve: 'hold',
      });
      expect(engine.parameters().map((p) => p.id)).toEqual([7]);
      expect(engine.parameterInfo(7).defaultCurve).toBe('hold');
      for (const name of ['parameterCount', 'parameterInfoByIndex']) {
        expect(name in engine, name).toBe(false);
      }
    });
  });

  it('lists markers as an array and returns the stored markers from setMarkers', () => {
    withEngine((engine) => {
      expect(engine.markers()).toEqual([]);
      const stored = engine.setMarkers([
        { id: 11, ppq: 0, name: 'intro' },
        { id: 12, ppq: 2, kind: 'lyric' },
      ]);
      expect(stored.map((m) => [m.id, m.kind])).toEqual([
        [11, 'marker'],
        [12, 'lyric'],
      ]);
      expect(engine.markers()).toEqual(stored);
      expect(engine.marker(12).kind).toBe('lyric');
      for (const name of ['markerCount', 'markerByIndex']) {
        expect(name in engine, name).toBe(false);
      }
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

  it('pops clip page requests as undefined when none is queued', () => {
    withEngine((engine) => {
      expect(engine.popClipPageRequest()).toBeUndefined();
    });
  });
});

describe('metronome', () => {
  it('names the click length in seconds or samples, never both', () => {
    withEngine((engine) => {
      engine.setMetronome({ enabled: true, clickSec: 0.01 });
      const bySec = engine.metronome();
      expect(typeof bySec.clickSec).toBe('number');
      expect('clickSamples' in bySec).toBe(false);
      expect('clickSeconds' in bySec).toBe(false);
      engine.setMetronome({ enabled: true, clickSamples: 16 });
      const bySamples = engine.metronome();
      expect(bySamples.clickSamples).toBe(16);
      expect('clickSec' in bySamples).toBe(false);
      const both = caught(() =>
        engine.setMetronome({ enabled: true, clickSamples: 16, clickSec: 0.01 }),
      );
      expect(both).toBeInstanceOf(RangeError);
      expect(caught(() => engine.setMetronome({ enabled: true, clickSamples: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => engine.setMetronome({ enabled: true, clickSec: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(() => engine.setMetronome({ enabled: true, clickSeconds: 0.01 } as never)).toThrow(
        /clickSeconds/,
      );
    });
  });
});

describe('telemetry', () => {
  it('reports telemetry kinds as strings and latency in samples', () => {
    withEngine((engine) => {
      engine.process([new Float32Array(256)]);
      const records = engine.drainTelemetry(16);
      expect(records.length).toBeGreaterThan(0);
      for (const record of records) {
        expect(['processBlock', 'error']).toContain(record.type);
        expect(typeof record.error).toBe('string');
        expect(typeof record.graphLatencySamples).toBe('number');
        expect('graphLatencySamplesQ8' in record).toBe(false);
      }
      expect(records.some((record) => record.error === 'maxBlockExceeded')).toBe(true);
    }, 64);
  });
});

describe('closed-set tokens', () => {
  it('refuses an ordinal for a marker kind', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, kind: 2 as never }])).toThrow(TypeError);
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, kind: 'chapter' as never }])).toThrow(
        RangeError,
      );
    });
  });

  it('refuses an ordinal for an automation curve', () => {
    withEngine((engine) => {
      engine.addParameter({ id: 7, name: 'gain', minValue: -60, maxValue: 12 });
      expect(() =>
        engine.setAutomationLane(7, [{ ppq: 0, value: 0, curveToNext: 1 as never }]),
      ).toThrow(TypeError);
      expect(() =>
        engine.setAutomationLane(7, [
          { ppq: 0, value: 0, curveToNext: 'linear' },
          { ppq: 1, value: 1 },
        ]),
      ).not.toThrow();
      expect(() => engine.addParameter({ id: 8, name: 'x', defaultCurve: 1 as never })).toThrow(
        TypeError,
      );
    });
  });

  it('refuses an ordinal for a graph node type and mix', () => {
    const graph = (type: unknown, mix: unknown) => ({
      nodes: [
        { id: 'in', numPorts: 2 },
        { id: 'gain', type, gainDb: 0, numPorts: 2 },
        { id: 'out', numPorts: 2 },
      ],
      connections: [{ sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0, mix }],
      inputNode: 'in',
      outputNode: 'out',
      numChannels: 2,
    });
    withEngine((engine) => {
      expect(() => engine.setGraph(graph('gain', 'replace') as never)).not.toThrow();
      expect(() => engine.setGraph(graph(1, 'add') as never)).toThrow(TypeError);
      expect(() => engine.setGraph(graph('gain', 0) as never)).toThrow(TypeError);
      expect(() => engine.setGraph(graph('gain', 'merge') as never)).toThrow(RangeError);
    });
  });

  it('refuses an ordinal for a dither type', () => {
    withEngine((engine) => {
      const base = { totalFrames: 128, blockSize: BLOCK, numChannels: 2 };
      expect(() => engine.bounceOffline({ ...base, dither: 'tpdf', ditherBits: 16 })).not.toThrow();
      expect(() => engine.bounceOffline({ ...base, dither: 2 as never })).toThrow(TypeError);
      expect(() => engine.bounceOffline({ ...base, dither: 'shaped' as never })).toThrow(
        RangeError,
      );
    });
  });

  it('refuses an ordinal for a warp mode', () => {
    withEngine((engine) => {
      const clip = (warpMode: unknown) => [
        {
          id: 1,
          channels: [new Float32Array(BLOCK)],
          startPpq: 0,
          lengthSamples: BLOCK,
          warpMode,
        },
      ];
      expect(() => engine.setClips(clip('repitch') as never)).not.toThrow();
      expect(() => engine.setClips(clip(1) as never)).toThrow(TypeError);
      expect(() => engine.setClips(clip('tempo-sync') as never)).toThrow(RangeError);
    });
  });

  it('refuses an ordinal for a channel layout', () => {
    withEngine((engine) => {
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 'stereo' }]),
      ).not.toThrow();
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 1 as never }]),
      ).toThrow(TypeError);
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: 0 as never }])).toThrow(
        TypeError,
      );
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: 'quad' as never }])).toThrow(
        RangeError,
      );
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: 'mono' }])).not.toThrow();
    });
  });

  it('refuses an ordinal for a send timing', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      const lane = (sendTiming: unknown) => [{ trackId: 10, sends: [{ busId: 1, sendTiming }] }];
      expect(() => engine.setTrackLanes(lane('preFader') as never)).not.toThrow();
      expect(() => engine.setTrackLanes(lane(1) as never)).toThrow(TypeError);
      expect(() => engine.setTrackLanes(lane('early') as never)).toThrow(RangeError);
    });
  });

  it('refuses an ordinal for a monitor mode', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setTrackMonitorMode(10, 'pfl')).not.toThrow();
      expect(() => engine.setTrackMonitorMode(10, 1 as never)).toThrow(TypeError);
      expect(() => engine.setTrackMonitorMode(10, 'cue' as never)).toThrow(RangeError);
    });
  });

  it('refuses an ordinal for a capture source', () => {
    withEngine((engine) => {
      expect(() => engine.configureCapture({ capacityFrames: 128, source: 'input' })).not.toThrow();
      expect(engine.captureStatus().source).toBe('input');
      expect(() => engine.configureCapture({ capacityFrames: 128, source: 1 as never })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, source: 'both' as never }),
      ).toThrow(RangeError);
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
      const rig = caught(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 1 as never }));
      expect(rig).toBeInstanceOf(TypeError);
      expect(String(rig)).toContain('mode');
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

describe('absent versus zero', () => {
  it('omits an unset bus output and refuses an explicit zero', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      expect(() => engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }])).not.toThrow();
      expect(() => engine.setTrackLanes([{ trackId: 10, outputBusId: 0 }])).toThrow(
        /omit it to route to the master mix/,
      );
      expect(() => engine.setTrackBuses([{ busId: 1, outputBusId: 0 }])).toThrow(RangeError);
      expect(() => engine.setTrackOutputBus(10, null)).not.toThrow();
      expect(() => engine.setTrackOutputBus(10, 0)).toThrow(RangeError);
    });
  });

  it('omits rate and loudness defaults, refuses a zero rate and reads a zero target', () => {
    withEngine((engine) => {
      const base = { totalFrames: 128, blockSize: BLOCK, numChannels: 2 };
      for (const field of ['targetSampleRate', 'sourceSampleRate']) {
        const error = caught(() => engine.bounceOffline({ ...base, [field]: 0 }));
        expect(error, field).toBeInstanceOf(RangeError);
        expect((error as Error).message).toContain(
          `options.${field} must be an integer in [1, 2147483647]`,
        );
      }
      expect(() =>
        engine.bounceOffline({ ...base, normalizeLufs: true, targetLufs: 0 }),
      ).not.toThrow();
      expect(() =>
        engine.bounceOffline({ ...base, normalizeLufs: true, targetLufs: Number.NaN }),
      ).toThrow(/targetLufs/);
    });
  });

  it('takes an omitted block size or record count as the library default and refuses 0', () => {
    const frames = 512;
    const tone = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      tone[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / 48000);
    }
    const render = (blockSize?: number): Float32Array[] => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      try {
        engine.setClips([
          { id: 1, channels: [tone, tone.slice()], startPpq: 0, lengthSamples: frames },
        ]);
        engine.play();
        return engine.renderOffline({
          channels: [new Float32Array(frames), new Float32Array(frames)],
          ...(blockSize === undefined ? {} : { blockSize }),
        });
      } finally {
        engine.destroy();
      }
    };
    const omitted = render();
    expect(omitted[0].some((sample) => Math.abs(sample) > 0.1)).toBe(true);
    expect(omitted).toEqual(render(128));

    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      engine.prepare({ sampleRate: 48000 });
      const planes = [new Float32Array(256), new Float32Array(256)];
      expect(() => engine.renderOffline({ channels: planes, blockSize: 0 })).toThrow(RangeError);
      expect(() => engine.primeOfflineParameters(2, 0)).toThrow(RangeError);
      expect(() => engine.primeOfflineParameters(2)).not.toThrow();
      for (const run of [
        () => engine.bounceOffline({ totalFrames: 256, numChannels: 2, blockSize: 0 }),
        () => engine.freezeOffline({ totalFrames: 256, numChannels: 2, blockSize: 0 }),
      ]) {
        const error = caught(run);
        expect(error).toBeInstanceOf(RangeError);
        expect((error as Error).message).toContain(
          'options.blockSize must be an integer in [1, 2147483647]',
        );
      }
      expect(caught(() => engine.bounceOffline({ totalFrames: 256 } as never))).toBeInstanceOf(
        TypeError,
      );
      expect(engine.bounceOffline({ totalFrames: 256, numChannels: 2 }).channels).toHaveLength(2);
      expect(engine.freezeOffline({ totalFrames: 256, numChannels: 2 }).frames).toBe(256);
      expect(Array.isArray(engine.drainTelemetry())).toBe(true);
      expect(engine.drainTelemetry(0)).toEqual([]);
    } finally {
      engine.destroy();
    }
  });
});

describe('track solo and mute', () => {
  it('keys solo and mute by track id', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      expect(() => engine.setTrackSoloMute(20, { solo: true, mute: false })).not.toThrow();
      expect(() => engine.setTrackSoloMute(20, { solo: false, mute: true }, 256)).not.toThrow();
      expect(() => engine.setTrackSoloMute(30, { solo: true, mute: false })).toThrow(RangeError);
      expect(() => engine.setTrackSoloMute(1, { solo: true, mute: false })).toThrow(RangeError);
      expect(() => engine.setTrackSoloMute(20, { solo: true } as never)).toThrow(TypeError);
      expect(() => engine.setTrackSoloMute(20, true as never)).toThrow(TypeError);
      expect(() =>
        engine.setTrackSoloMute(20, { solo: true, mute: false, safe: true } as never),
      ).toThrow(/safe/);
      expect('setSoloMute' in engine).toBe(false);
      engine.setTrackLanes([{ trackId: 20 }]);
      expect(() => engine.setTrackSoloMute(10, { solo: true, mute: false })).toThrow(RangeError);
    });
  });
});

describe('strip addressing', () => {
  it('addresses strips by track id or master', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      const track = engine.resolveStripAutomationId(10, 'faderDb');
      const master = engine.resolveStripAutomationId('master', 'faderDb');
      expect(track).not.toBe(master);
      expect(engine.resolveStripAutomationId('master', 'width')).not.toBe(master);
      expect(() => engine.resolveStripAutomationId(10, 'width')).toThrow(RangeError);
      expect(() => engine.resolveStripAutomationId('10', 'faderDb')).toThrow(RangeError);
      expect(() => engine.resolveStripAutomationId('bus', 'faderDb')).toThrow(RangeError);
      expect(() => engine.resolveStripAutomationId({} as never, 'faderDb')).toThrow(TypeError);
      expect(() => engine.setStripFaderDb(10, -6)).not.toThrow();
      expect(() => engine.setStripFaderDb('master', -3)).not.toThrow();
      expect(() => engine.setStripFaderDb('10', -3)).toThrow(RangeError);
      engine.setTrackBuses([{ busId: 1 }]);
      expect(() => engine.setBusGainDb(1, -4)).not.toThrow();
      expect(() => engine.setBusGainDb(99, -4)).toThrow(RangeError);
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

  it('replaces a strip with a scene strip object', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setTrackBuses([{ busId: 1 }]);
      expect(() => engine.setStrip(10, { id: 'track-10', faderDb: -6 } as never)).not.toThrow();
      expect(() => engine.setStrip('master', { id: 'master', faderDb: -3 } as never)).not.toThrow();
      expect(() => engine.setBusStrip(1, { id: '1', inserts: [] } as never)).not.toThrow();
      expect(() => engine.setStrip(10, '{"strips":[]}' as never)).toThrow(TypeError);
      expect(() => engine.setBusStrip(1, null as never)).toThrow(TypeError);
      expect(() => engine.setStrip('10', {} as never)).toThrow(RangeError);
    });
  });

  it('takes an EQ band as an object', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setStripEqBand(10, 0, '{"type":"Peak"}' as never)).toThrow(TypeError);
      expect(() => engine.setBusEqBand(1, 0, '{"type":"Peak"}' as never)).toThrow(TypeError);
      expect(() => engine.setStripEqBand('10', 0, { type: 'peak' })).toThrow(RangeError);
    });
  });

  it('takes bypass state and insert parameters as objects', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      expect(() => engine.setStripInsertBypassed(10, 0, { bypassed: 'yes' as never })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.setStripInsertBypassed(10, 0, { bypassed: true, reset: true } as never),
      ).toThrow(/reset/);
      expect(() => engine.setBusInsertBypassed(1, 0, true as never)).toThrow(TypeError);
      expect(() => engine.setStripInsertParam(10, 0, { name: 'levelDb' } as never)).toThrow(
        TypeError,
      );
      expect(() =>
        engine.setStripInsertParam(10, 0, { name: 'levelDb', value: 1, extra: 1 } as never),
      ).toThrow(/extra/);
      expect(() => engine.applyStripInsertParamNow(10, 0, { name: 1, value: 1 } as never)).toThrow(
        TypeError,
      );
      expect(() => engine.restoreStripInsertParam('10', 0, { name: 'levelDb', value: 1 })).toThrow(
        RangeError,
      );
      expect(() => engine.setBusInsertParam(1, 0, 'levelDb' as never)).toThrow(TypeError);
    });
  });

  it('sets pan on a track strip and refuses the master strip', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      // The native pan setters address a declared strip.
      engine.setStrip(10, { id: 'track-10' } as never);
      expect(() => engine.setStripPan(10, 0.25)).not.toThrow();
      expect(() => engine.setStripPan('master', 0.25)).toThrow(RangeError);
      expect(() => engine.setStripPanLaw(10, 'const6Db')).not.toThrow();
      expect(() => engine.setStripPanLaw(10, 1 as never)).toThrow(TypeError);
      expect(() => engine.setStripPanMode(10, 'dualPan')).not.toThrow();
      expect(() => engine.setStripPanMode(10, 2 as never)).toThrow(TypeError);
      expect(() => engine.setStripDualPan(10, -1, 1)).not.toThrow();
      expect(() => engine.setStripSurroundPan('master', {})).toThrow(RangeError);
      expect(() => engine.setStripChannelDelaySamples(10, 4)).not.toThrow();
      expect(() => engine.setStripChannelDelaySamples('master', 4)).toThrow(RangeError);
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStrip(1, { id: '1' } as never);
      expect(() => engine.setBusPan(1, 0.5)).not.toThrow();
      expect(() => engine.setBusPanLaw(1, 'const3Db')).not.toThrow();
      expect(() => engine.setBusPanLaw(1, 0 as never)).toThrow(TypeError);
      expect(() => engine.setBusPanMode(1, 'balance')).not.toThrow();
      expect(() => engine.setBusDualPan(1, -1, 1)).not.toThrow();
    });
  });

  it('keys a strip insert from a source object', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
      expect(() => engine.setStripSidechain(10, 0, { kind: 'bus', id: 1 })).toThrow(RangeError);
      expect(() => engine.setStripSidechain(10, 0, { kind: 1 as never, id: 1 })).toThrow(TypeError);
      expect(() => engine.setStripSidechain('10', 0, null)).toThrow(RangeError);
      expect(() => engine.setStripSidechain(10, 0, { kind: 'track', id: 0 })).toThrow(
        /pass null to remove the binding/,
      );
      expect(() =>
        engine.setStripSidechain(10, 0, { kind: 'track', id: 11, extra: 1 } as never),
      ).toThrow(/extra/);
      expect(() => engine.canSetStripSidechain(10, 0, { kind: 2 as never, id: 11 })).toThrow(
        TypeError,
      );
      expect(typeof engine.canSetStripSidechain(10, 0, { kind: 'track', id: 11 }).ok).toBe(
        'boolean',
      );
      expect(() => engine.setBusSidechain(1, 0, { kind: 'lane' as never, id: 1 })).toThrow(
        RangeError,
      );
      expect(() => engine.canSetBusSidechain(1, 0, { kind: 7 as never, id: 2 })).toThrow(TypeError);
    });
  });
});

describe('capture', () => {
  it('sets up capture in one call', () => {
    withEngine((engine) => {
      engine.configureCapture({
        capacityFrames: 128,
        numChannels: 2,
        recordOffsetSamples: -4,
        inputMonitor: { enabled: true, gain: 0.5 },
        punch: { startSample: 100, endSec: 1 },
      });
      const status = engine.captureStatus();
      expect(status.punchEnabled).toBe(true);
      expect(status.recordOffsetSamples).toBe(-4);
      expect(status.source).toBe('output');
      engine.configureCapture({ capacityFrames: 128 });
      expect(engine.captureStatus().punchEnabled).toBe(false);
      expect(engine.captureStatus().recordOffsetSamples).toBe(0);
      expect(caught(() => engine.configureCapture({ numChannels: 2 } as never))).toBeInstanceOf(
        TypeError,
      );
      expect(() => engine.configureCapture({ capacityFrames: 128, numChannels: 0 })).toThrow(
        /numChannels must be an integer in \[1, 2147483647\]/,
      );
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, bufferFrames: 1 } as never),
      ).toThrow(/bufferFrames/);
      for (const name of [
        'setCaptureBuffer',
        'setCapturePunch',
        'setCaptureSource',
        'setRecordOffsetSamples',
        'setInputMonitor',
      ]) {
        expect(name in engine, name).toBe(false);
      }
    });
  });

  it('reallocates the capture buffer only while disarmed', () => {
    withEngine((engine) => {
      engine.configureCapture({ capacityFrames: 128 });
      engine.armCapture({ armed: true });
      expect(engine.captureStatus().armed).toBe(true);
      expect(() => engine.configureCapture({ capacityFrames: 128, source: 'input' })).not.toThrow();
      const error = caught(() => engine.configureCapture({ capacityFrames: 256 }));
      expect(isSonareError(error)).toBe(true);
      expect((error as { code: number }).code).toBe(ErrorCode.InvalidState);
      engine.armCapture({ armed: false });
      expect(() => engine.configureCapture({ capacityFrames: 256 })).not.toThrow();
    });
  });

  it('takes the punch window as one spelling per bound', () => {
    withEngine((engine) => {
      const punch = (p: Record<string, number>) =>
        engine.configureCapture({ capacityFrames: 128, punch: p });
      expect(() => punch({ startPpq: 2, endPpq: 4 })).not.toThrow();
      expect(() => punch({ startPpq: 1, startSample: 1, endSample: 2 })).toThrow(
        new RangeError(
          'RealtimeEngine.configureCapture: give punch.startPpq, punch.startSample or punch.startSec, not more than one',
        ),
      );
      expect(() => punch({ startSample: 1 })).toThrow(
        new TypeError(
          'RealtimeEngine.configureCapture: give punch.endPpq, punch.endSample or punch.endSec',
        ),
      );
      expect(() => punch({ startSample: -1, endSample: 2 })).toThrow(RangeError);
      expect(() => punch({ startSample: 1, endSample: 2, enabled: 1 })).toThrow(TypeError);
      expect(() => punch({ startSample: 5, endSample: 5 })).toThrow(RangeError);
    });
  });

  it('arms capture with a state object', () => {
    withEngine((engine) => {
      engine.configureCapture({ capacityFrames: 128 });
      expect(() => engine.armCapture(true as never)).toThrow(TypeError);
      expect(() => engine.armCapture({ armed: 1 as never })).toThrow(TypeError);
      expect(() => engine.armCapture({ armed: true, punch: true } as never)).toThrow(/punch/);
      engine.armCapture({ armed: true });
      expect(engine.captureStatus().armed).toBe(true);
    });
  });
});

describe('offline render and process', () => {
  it('renders offline from one request object', () => {
    withEngine((engine) => {
      const plane = () => new Float32Array(BLOCK);
      expect(() => engine.renderOffline({ channels: [plane(), plane()] })).not.toThrow();
      expect(() => loose(engine).renderOffline([plane()], BLOCK)).toThrow(TypeError);
      expect(() =>
        engine.renderOffline({ channels: [plane()], sampleRate: 48000 } as never),
      ).toThrow(/sampleRate/);
      const poisoned = plane();
      poisoned[3] = Number.NaN;
      const error = caught(() => engine.renderOffline({ channels: [poisoned] }));
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toContain('index 3');
      expect(() => engine.renderOffline({ channels: [plane()], validate: false } as never)).toThrow(
        unknownKeyError('RealtimeEngine.renderOffline: unknown key validate'),
      );
    });
  });

  it('names the zero-copy buffer accessors without get', () => {
    withEngine((engine) => {
      engine.prepareChannels(2, BLOCK);
      expect(engine.channelBuffer(0, BLOCK)).toBeInstanceOf(Float32Array);
      engine.prepareMonitorChannels(2, BLOCK);
      expect(engine.monitorChannelBuffer(0, BLOCK)).toBeInstanceOf(Float32Array);
      expect('getChannelBuffer' in engine).toBe(false);
      expect('getMonitorChannelBuffer' in engine).toBe(false);
    });
  });

  it('keeps the scratch accessors off the public class', () => {
    withEngine((engine) => {
      for (const name of [
        'popExternalMidiToScratch',
        'popClipPageRequestToScratch',
        'popTelemetryToScratch',
        'popMeterTelemetryToScratch',
        'popScopeTelemetryToScratch',
        'consumeExternalMidiScratch',
        'telemetryScratchType',
        'meterScratchValue',
        'scopeScratchBand',
        'externalMidiScratchByteWord',
        'clipPageRequestScratchSample',
      ]) {
        expect(name in engine, name).toBe(false);
      }
    });
  });

  it('delivers external MIDI bytes as a Uint8Array', () => {
    withEngine((engine) => {
      engine.setBuiltinInstrument(5, { gain: 0.5 });
      engine.setMidiDestinationExternal(5, true);
      engine.setMidiClips([
        {
          id: 1,
          trackId: 5,
          destinationId: 5,
          lengthSamples: 8192,
          events: [
            { renderFrame: 0, word0: (0x2 << 28) | (0x90 << 16) | (60 << 8) | 100, wordCount: 1 },
          ],
        },
      ]);
      engine.play();
      engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      const events = engine.drainExternalMidi(16);
      expect(events.length).toBe(1);
      expect(events[0].bytes).toBeInstanceOf(Uint8Array);
      expect(events[0].bytes).toEqual(Uint8Array.of(0x90, 60, 100));
    });
  });
});

describe('instruments and MIDI', () => {
  it('leads with the destination id on instrument setters', () => {
    withEngine((engine) => {
      expect(() => engine.setBuiltinInstrument(3, { gain: 0.5 })).not.toThrow();
      expect(() => engine.setSynthInstrument(4, { engineMode: 'reed' })).not.toThrow();
      expect(() => engine.setSf2Instrument(5, {})).not.toThrow();
      expect(engine.midiInstrumentCount).toBe(3);
      const builtin = caught(() => engine.setBuiltinInstrument(3, { destinationId: 3 } as never));
      expect(builtin).toBeInstanceOf(TypeError);
      expect((builtin as Error).message).toContain('destinationId');
      expect(() => engine.setSf2Instrument(5, { destinationId: 5 } as never)).toThrow(
        /destinationId/,
      );
    });
  });

  it('sets a part rig from one request', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(0, { engineMode: 'reed' });
      expect(() =>
        engine.setPartRig({
          destinationId: 0,
          part: PART_RIG_ALL_PARTS,
          mode: 'chain',
          inserts: [{ processor: 'dynamics.limiter', params: { thresholdDb: -6 } }],
        }),
      ).not.toThrow();
      expect(() => loose(engine).setPartRig(0, 0, 'none')).toThrow(TypeError);
      expect(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 0 as never })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.setPartRig({ destinationId: 0, part: 0, mode: 'none', extra: 1 } as never),
      ).toThrow(/extra/);
    });
  });

  it('binds a MIDI controller from one descriptor', () => {
    withEngine((engine) => {
      engine.addParameter({ id: 5, name: 'p', minValue: 0, maxValue: 1 });
      engine.bindMidiCc({ ccNumber: 7, channel: 0, paramId: 5, minValue: 0, maxValue: 1 });
      expect(engine.midiCcBindingCount).toBe(1);
      expect('bindMidiCcBinding' in engine).toBe(false);
    });
  });

  it('takes a program bank as one object', () => {
    withEngine((engine) => {
      engine.setSf2Instrument(0, {});
      expect(() => engine.pushMidiProgram(0, 0, 0, 5)).not.toThrow();
      expect(() => engine.pushMidiProgram(0, 0, 0, 5, { msb: 1, lsb: 2 }, 256)).not.toThrow();
      expect(() => engine.pushMidiProgram(0, 0, 0, 5, { msb: 128, lsb: 0 })).toThrow(RangeError);
      expect(() =>
        engine.pushMidiProgram(0, 0, 0, 5, { msb: 1, lsb: 2, valid: true } as never),
      ).toThrow(/valid/);
      engine.setMidiInputSource(0);
      expect(() => engine.pushMidiInputProgram(0, 0, 5, { msb: 1, lsb: 2 }, 64)).not.toThrow();
      expect(() => engine.pushMidiInputProgram(0, 0, 5)).not.toThrow();
    });
  });

  it('refuses an ordinal for an articulation and a note-tracking rule', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(0, { engineMode: 'reed' });
      expect(() => engine.setArticulation(0, 0, 2 as never)).toThrow(TypeError);
      expect(() => engine.setControllerNoteTracking(0, 0 as never, 'last')).toThrow(TypeError);
      expect(() => engine.setControllerNoteTracking(0, 'bend', 1 as never)).toThrow(TypeError);
      expect(() => engine.bindController(0, { input: 2 as never, axis: 'excitation' })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.bindController(0, { input: 'controlChange', index: 2, axis: 3 as never }),
      ).toThrow(TypeError);
    });
  });
});

describe('removed names', () => {
  it('no longer exposes the one-off and per-strip-kind spellings', () => {
    const removed = [
      'setTrackStripJson',
      'setMasterStripJson',
      'setBusStripJson',
      'setTrackStripEqBand',
      'setMasterStripEqBand',
      'setBusStripEqBand',
      'setTrackStripEqBandJson',
      'setMasterStripEqBandJson',
      'setBusStripEqBandJson',
      'setTrackStripInsertBypassed',
      'setMasterStripInsertBypassed',
      'setBusStripInsertBypassed',
      'setTrackStripInsertParamByName',
      'setMasterStripInsertParamByName',
      'setBusStripInsertParamByName',
      'applyTrackStripInsertParamByNameNow',
      'applyMasterStripInsertParamByNameNow',
      'applyBusStripInsertParamByNameNow',
      'restoreTrackStripInsertParamByName',
      'restoreMasterStripInsertParamByName',
      'restoreBusStripInsertParamByName',
      'clearTrackInsertParameterBases',
      'clearMasterInsertParameterBases',
      'resolveTrackInsertAutomationId',
      'resolveMasterInsertAutomationId',
      'resolveTrackLaneAutomationId',
      'resolveMasterAutomationId',
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
      'setLaneSidechain',
      'setMasterSidechain',
      'canSetLaneSidechain',
      'canSetMasterSidechain',
    ];
    withEngine((engine) => {
      for (const name of removed) {
        expect(name in engine, name).toBe(false);
      }
    });
  });
});

describe('offline bounce', () => {
  it('bounces the timeline to planar channels', () => {
    withEngine((engine) => {
      const result = engine.bounceOffline({ totalFrames: 256, blockSize: BLOCK, numChannels: 2 });
      expect(result).not.toHaveProperty('numChannels');
      expect(result.sampleRate).toBe(48000);
      expect(result.channels).toHaveLength(2);
      expect(result.channels[0]).toBeInstanceOf(Float32Array);
      expect(result.channels[0].length).toBe(256);
      expect(result.channels[1].length).toBe(256);
      expect(typeof result.integratedLufs).toBe('number');
      expect(result).not.toHaveProperty('interleaved');
      expect(result).not.toHaveProperty('data');
    });
  });
});

describe('RealtimeEngine maxBlockSize', () => {
  it('reports the library default block size when maxBlockSize is omitted', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    // The audio-thread entry point does not throw: a block over the bound is
    // silenced and reported through telemetry.
    const exceeded = (frames: number): number => {
      engine.process([new Float32Array(frames), new Float32Array(frames)]);
      return engine.drainTelemetry().filter((record) => record.error === 'maxBlockExceeded').length;
    };
    try {
      const max = engine.maxBlockSize;
      expect(max).toBeGreaterThan(0);
      expect(exceeded(max)).toBe(0);
      expect(exceeded(max + 1)).toBe(1);
    } finally {
      engine.destroy();
    }
  });

  it('reports the given block size', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      expect(engine.maxBlockSize).toBe(256);
    } finally {
      engine.destroy();
    }
  });
});
