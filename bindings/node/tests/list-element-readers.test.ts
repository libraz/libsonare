/**
 * The list-valued arguments of the project and engine entries are read alike: a non-object
 * element, an absent required field and a wrong type are refused as TypeError, a value outside its
 * domain as RangeError, and every refusal names the element by its index.
 */

import { describe, expect, it } from 'vitest';
import { Project, RealtimeEngine } from '../dist/index.js';

const UINT32 = 'must be an integer in [0, 4294967295]';
const FLOAT32 = 'must be finite and in [-3.4028235e38, 3.4028235e38]';

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

const withEngine = (run: (engine: RealtimeEngine) => void): void => {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    engine.addParameter({ id: 1, name: 'gain' });
    run(engine);
  } finally {
    engine.destroy();
  }
};

const withProject = (run: (project: Project, track: number) => void): void => {
  const project = new Project();
  try {
    run(project, project.addTrack({ kind: 'audio' }));
  } finally {
    project.destroy();
  }
};

const graph = (patch: Record<string, unknown>) =>
  ({
    nodes: [{ id: 'a' }, { id: 'b' }],
    connections: [],
    inputNode: 'a',
    outputNode: 'b',
    ...patch,
  }) as never;
const link = { sourceNode: 'a', sourcePort: 0, destNode: 'b', destPort: 0 };

const graphCases: ReadonlyArray<
  readonly [string, Record<string, unknown>, typeof TypeError, string]
> = [
  ['a non-object node', { nodes: [{ id: 'a' }, 5] }, TypeError, 'nodes[1] must be an object'],
  [
    'a non-object connection',
    { connections: [link, 5] },
    TypeError,
    'connections[1] must be an object',
  ],
  [
    'an absent source port',
    { connections: [{ sourceNode: 'a', destNode: 'b', destPort: 0 }] },
    TypeError,
    'connections[0].sourcePort must be an integer',
  ],
  [
    'an absent destination port',
    { connections: [{ sourceNode: 'a', sourcePort: 0, destNode: 'b' }] },
    TypeError,
    'connections[0].destPort must be an integer',
  ],
  [
    'a source port outside the integer range at the second element',
    { connections: [link, { ...link, sourcePort: 2 ** 31 }] },
    RangeError,
    'connections[1].sourcePort must be an integer in [-2147483648, 2147483647]',
  ],
  [
    'a non-object parameter binding',
    { parameterBindings: [5] },
    TypeError,
    'parameterBindings[0] must be an object',
  ],
  [
    'an absent parameter id',
    { parameterBindings: [{ nodeId: 'a' }] },
    TypeError,
    'parameterBindings[0].paramId must be an integer',
  ],
  [
    'an absent binding node',
    { parameterBindings: [{ paramId: 1 }] },
    TypeError,
    'parameterBindings[0].nodeId must be a string',
  ],
  [
    'a negative parameter id at the second element',
    {
      parameterBindings: [
        { paramId: 1, nodeId: 'a' },
        { paramId: -1, nodeId: 'a' },
      ],
    },
    RangeError,
    `parameterBindings[1].paramId ${UINT32}`,
  ],
];

describe('RealtimeEngine.setGraph', () => {
  it.each(graphCases)('refuses %s', (_name, patch, errorClass, text) => {
    withEngine((engine) => {
      const error = caught(() => engine.setGraph(graph(patch)));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`RealtimeEngine.setGraph: ${text}`);
    });
  });
});

const mixerCases: ReadonlyArray<
  readonly [string, string, (engine: RealtimeEngine) => unknown, typeof TypeError, string]
> = [
  [
    'a non-object send',
    'setTrackBuses',
    (e) => e.setTrackBuses([{ busId: 1, sends: [5] }] as never),
    TypeError,
    'sends[0] must be an object',
  ],
  [
    'an absent send bus',
    'setTrackBuses',
    (e) => e.setTrackBuses([{ busId: 1, sends: [{}] }] as never),
    TypeError,
    'sends[0].busId must be an integer',
  ],
  [
    'a negative send bus at the second element',
    'setTrackLanes',
    (e) => e.setTrackLanes([{ trackId: 1, sends: [{ busId: 1 }, { busId: -1 }] }] as never),
    RangeError,
    `sends[1].busId ${UINT32}`,
  ],
  [
    'an absent lane track',
    'setTrackLanes',
    (e) => e.setTrackLanes([{ trackId: 1 }, {}] as never),
    TypeError,
    'lanes[1].trackId must be an integer',
  ],
  [
    'a negative lane track at the second element',
    'setTrackLanes',
    (e) => e.setTrackLanes([{ trackId: 1 }, { trackId: -1 }] as never),
    RangeError,
    `lanes[1].trackId ${UINT32}`,
  ],
  [
    'an absent bus id',
    'setTrackBuses',
    (e) => e.setTrackBuses([{ busId: 1 }, {}] as never),
    TypeError,
    'buses[1].busId must be an integer',
  ],
  [
    'a negative bus id at the second element',
    'setTrackBuses',
    (e) => e.setTrackBuses([{ busId: 1 }, { busId: -1 }] as never),
    RangeError,
    `buses[1].busId ${UINT32}`,
  ],
];

describe('the engine mixer lists', () => {
  it.each(mixerCases)('refuse %s', (_name, entry, run, errorClass, text) => {
    withEngine((engine) => {
      const error = caught(() => run(engine));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`RealtimeEngine.${entry}: ${text}`);
    });
  });
});

const engineLaneCases: ReadonlyArray<readonly [string, unknown[], typeof TypeError, string]> = [
  ['a non-object point', [{ ppq: 0, value: 0 }, 5], TypeError, 'points[1] must be an object'],
  ['an absent position', [{ value: 0 }], TypeError, 'points[0].ppq must be a number'],
  ['an absent value', [{ ppq: 0 }], TypeError, 'points[0].value must be a number'],
  [
    'a value outside the float range at the second element',
    [
      { ppq: 0, value: 0 },
      { ppq: 1, value: 1e300 },
    ],
    RangeError,
    `points[1].value ${FLOAT32}`,
  ],
];

describe('RealtimeEngine.setAutomationLane', () => {
  it.each(engineLaneCases)('refuses %s', (_name, points, errorClass, text) => {
    withEngine((engine) => {
      const error = caught(() => engine.setAutomationLane(1, points as never));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`RealtimeEngine.setAutomationLane: ${text}`);
    });
  });
});

describe('RealtimeEngine.setMidiClips', () => {
  it('refuses a non-object clip by its index', () => {
    withEngine((engine) => {
      const error = caught(() => engine.setMidiClips([{ events: [] }, 5] as never));
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toBe('RealtimeEngine.setMidiClips: clips[1] must be an object');
    });
  });

  it('accepts a clip without a start position, which is optional', () => {
    withEngine((engine) => {
      expect(() => engine.setMidiClips([{ events: [] }] as never)).not.toThrow();
    });
  });
});

const modCases: ReadonlyArray<readonly [string, unknown[], typeof TypeError, string]> = [
  [
    'a non-object routing',
    [{ source: 'lfo1', destination: 'pitchCents', depth: 1 }, 3],
    TypeError,
    'modRoutings[1] must be an object',
  ],
  [
    'an absent source',
    [{ destination: 'pitchCents', depth: 1 }],
    TypeError,
    'mod source must be a number or string',
  ],
  [
    'an absent destination',
    [{ source: 'lfo1', depth: 1 }],
    TypeError,
    'mod destination must be a number or string',
  ],
  [
    'an absent depth',
    [{ source: 'lfo1', destination: 'pitchCents' }],
    TypeError,
    'modRoutings[0].depth must be a number',
  ],
  [
    'a depth outside the float range at the second element',
    [
      { source: 'lfo1', destination: 'pitchCents', depth: 1 },
      { source: 'lfo2', destination: 'pitchCents', depth: 1e300 },
    ],
    RangeError,
    `modRoutings[1].depth ${FLOAT32}`,
  ],
];

describe('a synth patch mod routing list', () => {
  it.each(modCases)('refuses %s', (_name, modRoutings, errorClass, text) => {
    withEngine((engine) => {
      const error = caught(() => engine.setSynthInstrument(1, { modRoutings } as never));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`RealtimeEngine.setSynthInstrument: ${text}`);
    });
  });
});

const projectLaneCases: ReadonlyArray<
  readonly [string, Record<string, unknown>, typeof TypeError, string]
> = [
  [
    'a non-object point',
    { targetParamId: 5, points: [5] },
    TypeError,
    'points[0] must be an object',
  ],
  [
    'an absent position',
    { targetParamId: 5, points: [{ ppq: 0, value: 0 }, { value: 1 }] },
    TypeError,
    'points[1].ppq must be a number',
  ],
  [
    'an absent value',
    { targetParamId: 5, points: [{ ppq: 0 }] },
    TypeError,
    'points[0].value must be a number',
  ],
  [
    'a position outside its domain at the second element',
    {
      targetParamId: 5,
      points: [
        { ppq: 0, value: 0 },
        { ppq: -1, value: 1 },
      ],
    },
    RangeError,
    'points[1].ppq must be finite and in [0, 1e12]',
  ],
  [
    'an absent target',
    { points: [] },
    TypeError,
    'targetParamId must be given for an opaque target',
  ],
  [
    'a target outside its domain',
    { targetParamId: 2 ** 32, points: [] },
    RangeError,
    'targetParamId must be an integer in [1, 4294967295]',
  ],
];

describe('Project.addAutomationLane', () => {
  it.each(projectLaneCases)('refuses %s', (_name, desc, errorClass, text) => {
    withProject((project, track) => {
      const error = caught(() => project.addAutomationLane(track, desc as never));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`Project.addAutomationLane: ${text}`);
    });
  });
});

const anchorCases: ReadonlyArray<readonly [string, unknown[], typeof TypeError, string]> = [
  [
    'a non-object anchor',
    [{ warpSample: 0, sourceSample: 0 }, 5],
    TypeError,
    'anchors[1] must be an object',
  ],
  [
    'an absent warp sample',
    [{ sourceSample: 0 }],
    TypeError,
    'anchors[0].warpSample must be a number',
  ],
  [
    'an absent source sample',
    [{ warpSample: 0, sourceSample: 0 }, { warpSample: 10 }],
    TypeError,
    'anchors[1].sourceSample must be a number',
  ],
  [
    'an infinite warp sample at the second element',
    [
      { warpSample: 0, sourceSample: 0 },
      { warpSample: Number.POSITIVE_INFINITY, sourceSample: 0 },
    ],
    RangeError,
    'anchors[1].warpSample must be finite',
  ],
];

describe('Project.setWarpMap', () => {
  it.each(anchorCases)('refuses %s', (_name, anchors, errorClass, text) => {
    withProject((project) => {
      const error = caught(() => project.setWarpMap({ id: 1, anchors } as never));
      expect(error).toBeInstanceOf(errorClass);
      expect(error.message).toBe(`Project.setWarpMap: ${text}`);
    });
  });
});

describe('Project.addLoopRecordingTakes', () => {
  it('refuses an absent channel count as a number, without a descriptor root', () => {
    withProject((project, track) => {
      const error = caught(() =>
        project.addLoopRecordingTakes({
          trackId: track,
          startPpq: 0,
          loopLengthPpq: 4,
          audio: new Float32Array(8),
          audioSampleRate: 48000,
        } as never),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toBe('Project.addLoopRecordingTakes: audioChannels must be an integer');
    });
  });
});
