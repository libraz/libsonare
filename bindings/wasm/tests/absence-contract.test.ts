/**
 * Absence is spelt by omission: an unset value reads back as a missing key, an empty or zero
 * value that stands for "not given" is refused by name, and `undefined` and `null` both mean
 * "not given". The Node package carries the same cases in its own copy of this file.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  extractPercussiveEvents,
  init,
  Project,
  RealtimeEngine,
  renderPercussiveEvents,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const AUDIO = [new Float32Array(480).fill(0.25)];

function withProject(body: (project: Project, trackId: number, clipId: number) => void): void {
  const project = new Project();
  try {
    const trackId = project.addTrack({ kind: 'audio' });
    const clipId = project.addClip({
      trackId,
      startPpq: 0,
      lengthPpq: 4,
      channels: AUDIO,
      sampleRate: 48000,
    });
    body(project, trackId, clipId);
  } finally {
    project.destroy();
  }
}

function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    body(engine);
  } finally {
    engine.destroy();
  }
}

const EMPTY_MARKER = (member: string) =>
  `${member}: marker name must not be empty; omit it for an unnamed marker`;

describe('an unset value reads back as an absent key and the record is passable back in', () => {
  it('project marker', () => {
    const project = new Project();
    try {
      project.setMarker({ ppq: 1 });
      const [marker] = project.markers();
      expect(marker).not.toHaveProperty('name');
      expect(project.setMarker(marker)).toBe(marker.id);
      expect(project.markers()).toEqual([marker]);
      project.setMarker({ ppq: 2, name: 'verse' });
      expect(project.markers()[1].name).toBe('verse');
    } finally {
      project.destroy();
    }
  });

  it('project source metadata', () => {
    withProject((project) => {
      expect(project.sources()[0]).not.toHaveProperty('contentHash');
      expect(project.sources()[0]).not.toHaveProperty('externalStemRole');
      const sourceId = project.sources()[0].id;
      project.setAudioSourceMetadata({ sourceId, contentHash: 'abc' });
      expect(project.sources()[0].contentHash).toBe('abc');
      expect(project.sources()[0]).not.toHaveProperty('externalStemRole');
    });
  });

  it('engine marker', () => {
    withEngine((engine) => {
      engine.setMarkers([{ ppq: 1 }]);
      const stored = engine.markers();
      expect(stored[0]).not.toHaveProperty('name');
      engine.setMarkers(stored);
      expect(engine.markers()).toEqual(stored);
    });
  });

  it('engine metronome click length keeps the spelling that was configured', () => {
    withEngine((engine) => {
      engine.setMetronome({ enabled: true });
      const byDefault = engine.metronome();
      expect(byDefault).not.toHaveProperty('clickSamples');
      expect(byDefault.clickSec).toBeGreaterThan(0);
      engine.setMetronome(byDefault);
      expect(engine.metronome()).toEqual(byDefault);

      engine.setMetronome({ enabled: true, clickSamples: 480 });
      const bySamples = engine.metronome();
      expect(bySamples.clickSamples).toBe(480);
      expect(bySamples).not.toHaveProperty('clickSec');
      engine.setMetronome(bySamples);
      expect(engine.metronome()).toEqual(bySamples);

      engine.setMetronome({ enabled: true, clickSec: 0.01 });
      const bySeconds = engine.metronome();
      expect(bySeconds.clickSec).toBe(0.01);
      expect(bySeconds).not.toHaveProperty('clickSamples');
    });
  });
});

describe('an empty name is refused by name and an omitted or null one is not given', () => {
  it('names a project marker', () => {
    const project = new Project();
    try {
      expect(() => project.setMarker({ ppq: 0, name: '' })).toThrow(
        new RangeError(EMPTY_MARKER('Project.setMarker')),
      );
      expect(() => project.setMarker({ ppq: 0, name: 5 as never })).toThrow(
        new TypeError('Project.setMarker: name must be a string'),
      );
      project.setMarker({ ppq: 0, name: null as never });
      expect(project.markers()[0]).not.toHaveProperty('name');
      const long = 'x'.repeat(200);
      project.setMarker({ ppq: 1, name: long });
      expect(project.markers()[1].name).toBe(long);
    } finally {
      project.destroy();
    }
  });

  it('names an engine marker', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ ppq: 0, name: '' }])).toThrow(
        new RangeError(
          'RealtimeEngine.setMarkers: markers[0].name must not be empty; omit it for an unnamed marker',
        ),
      );
      engine.setMarkers([{ ppq: 0, name: null as never }]);
      expect(engine.markers()[0]).not.toHaveProperty('name');
    });
  });

  it('names a track, a warp map, a take and source metadata', () => {
    withProject((project, trackId, clipId) => {
      expect(() => project.addTrack({ name: '' })).toThrow(
        new RangeError('Project.addTrack: name must not be empty; omit it for no name'),
      );
      expect(() => project.addTrack({ name: 5 as never })).toThrow(
        new TypeError('Project.addTrack: name must be a string'),
      );
      expect(project.addTrack({ name: null as never })).toBeGreaterThan(0);
      const long = 'y'.repeat(200);
      const named = project.addTrack({ name: long });
      expect(project.tracks().find((t) => t.id === named)?.name).toBe(long);

      const anchors = [
        { warpSample: 0, sourceSample: 0 },
        { warpSample: 100, sourceSample: 100 },
      ];
      expect(() => project.setWarpMap({ id: 1, name: '', anchors })).toThrow(
        new RangeError('Project.setWarpMap: name must not be empty; omit it for no name'),
      );
      project.setWarpMap({ id: 1, name: null as never, anchors });

      expect(() => project.setClipTakes(clipId, [{ id: 1, name: '' }])).toThrow(
        new RangeError(
          'Project.setClipTakes: takes[0].name must not be empty; omit it for an unnamed take',
        ),
      );
      const sourceId = project.sources()[0].id;
      expect(() => project.setAudioSourceMetadata({ sourceId, contentHash: '' })).toThrow(
        new RangeError(
          'Project.setAudioSourceMetadata: contentHash must not be empty; omit it to clear it',
        ),
      );
      expect(trackId).toBeGreaterThan(0);
    });
  });
});

describe('a list is required and an empty one is the clear', () => {
  it('refuses an absent take list and comp segment list with the same text', () => {
    withProject((project, _track, clipId) => {
      for (const absent of [undefined, null]) {
        expect(() => project.setClipTakes(clipId, absent as never)).toThrow(
          new TypeError('Project.setClipTakes: takes must be an array'),
        );
        expect(() => project.setClipCompSegments(clipId, absent as never)).toThrow(
          new TypeError('Project.setClipCompSegments: segments must be an array'),
        );
      }
      project.setClipTakes(clipId, []);
      project.setClipCompSegments(clipId, []);
    });
  });
});

describe('a take id is required and 0 is refused by name', () => {
  it('on the take, its source, the active take and a comp segment', () => {
    withProject((project, _track, clipId) => {
      expect(() => project.setClipTakes(clipId, [{} as never])).toThrow(
        new TypeError('Project.setClipTakes: takes[0].id must be an integer'),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 0 }])).toThrow(
        new RangeError('Project.setClipTakes: takes[0].id must not be 0'),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 1, sourceId: 0 }])).toThrow(
        new RangeError(
          "Project.setClipTakes: takes[0].sourceId must not be 0; omit it for the clip's source",
        ),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 1 }], { activeTakeId: 0 })).toThrow(
        new RangeError('Project.setClipTakes: activeTakeId must not be 0; omit it for the default'),
      );
      project.setClipTakes(clipId, [{ id: 1, sourceId: null as never }], {
        activeTakeId: null as never,
      });
      expect(() =>
        project.setClipCompSegments(clipId, [{ startPpq: 0, endPpq: 1, takeId: 0 }]),
      ).toThrow(
        new RangeError(
          'Project.setClipCompSegments: segments[0].takeId must not be 0; omit it for the active take',
        ),
      );
      project.setClipCompSegments(clipId, [{ startPpq: 0, endPpq: 1, takeId: null as never }]);
    });
  });
});

describe('an omitted id is the default and an explicit 0 is refused', () => {
  it('on a clip track and on a clip move', () => {
    withProject((project, trackId, clipId) => {
      expect(() => project.addClip({ trackId: 0, lengthPpq: 1 })).toThrow(
        new RangeError('Project.addClip: trackId must not be 0; a clip needs an existing track'),
      );
      expect(() => project.moveClip(clipId, 1, { trackId: 0 })).toThrow(
        new RangeError('Project.moveClip: trackId must not be 0; omit it to keep the track'),
      );
      // Every other value outside a positive id is refused with the same interval.
      for (const bad of [-1, 1.5, 2 ** 32]) {
        expect(() => project.moveClip(clipId, 1, { trackId: bad })).toThrow(
          new RangeError('Project.moveClip: trackId must be an integer in [1, 4294967295]'),
        );
      }
      project.moveClip(clipId, 1, { trackId: null as never });
      project.moveClip(clipId, 2, { trackId });
    });
  });

  it('on graph ports, graph channels and built-in polyphony', () => {
    withEngine((engine) => {
      const graph = (patch: Record<string, unknown> = {}, port?: number) => ({
        nodes: [
          { id: 'in', numPorts: port },
          { id: 'out', numPorts: port },
        ],
        connections: [{ sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0 }],
        inputNode: 'in',
        outputNode: 'out',
        ...patch,
      });
      for (const port of [0, -1]) {
        expect(() => engine.setGraph(graph({}, port))).toThrow(
          new RangeError(
            'RealtimeEngine.setGraph: spec.nodes[0].numPorts must be an integer in [1, 2147483647]',
          ),
        );
      }
      expect(() => engine.setGraph(graph({}, 1.5))).toThrow(
        /spec\.nodes\[0\]\.numPorts must be an integer in \[1, 2147483647\]$/,
      );
      expect(() => engine.setGraph(graph({ numChannels: 0 }))).toThrow(
        new RangeError(
          'RealtimeEngine.setGraph: numChannels must not be 0; omit it for the default',
        ),
      );
      // A non-finite count is not an integer, and says so with the interval it misses.
      for (const bad of [Number.POSITIVE_INFINITY, Number.NaN]) {
        expect(() => engine.setGraph(graph({ numChannels: bad }))).toThrow(
          new RangeError(
            'RealtimeEngine.setGraph: numChannels must be an integer in [1, 2147483647]',
          ),
        );
      }
      engine.setGraph(graph({ numChannels: null }, undefined));
      expect(engine.graphNodeCount).toBe(2);
      expect(() => engine.setBuiltinInstrument(0, { polyphony: 0 })).toThrow(
        /polyphony must be an integer in \[1, 64\]$/,
      );
    });
  });

  it('on a graph node name, which is a string and never empty', () => {
    withEngine((engine) => {
      const graph = (patch: Record<string, unknown>) => ({
        nodes: [{ id: 'a' }],
        connections: [],
        inputNode: 'a',
        outputNode: 'a',
        ...patch,
      });
      expect(() => engine.setGraph(graph({ inputNode: '' }) as never)).toThrow(
        new RangeError('RealtimeEngine.setGraph: inputNode must not be empty'),
      );
      expect(() => engine.setGraph(graph({ outputNode: '' }) as never)).toThrow(
        new RangeError('RealtimeEngine.setGraph: outputNode must not be empty'),
      );
      for (const absent of [undefined, null]) {
        expect(() => engine.setGraph(graph({ inputNode: absent }) as never)).toThrow(
          new TypeError('RealtimeEngine.setGraph: inputNode must be a string'),
        );
      }
      expect(() => engine.setGraph(graph({ nodes: [{ id: '' }] }) as never)).toThrow(
        new RangeError('RealtimeEngine.setGraph: nodes[0].id must not be empty'),
      );
      expect(() => engine.setGraph(graph({ nodes: [{ id: 'a'.repeat(64) }] }) as never)).toThrow(
        /graph node id must be at most 63 bytes/,
      );
      expect(() => engine.setGraph(graph({ nodes: [{ id: 'a'.repeat(64) }] }) as never)).toThrow(
        RangeError,
      );
    });
  });

  it('on a freeze clip id', () => {
    withEngine((engine) => {
      expect(() => engine.freezeOffline({ totalFrames: 128, numChannels: 2, clipId: 0 })).toThrow(
        new RangeError(
          'RealtimeEngine.freezeOffline: clipId must not be 0; omit it for the default',
        ),
      );
      expect(
        engine.freezeOffline({ totalFrames: 128, numChannels: 2, clipId: null as never }).clipId,
      ).toBe(1);
    });
  });
});

describe('a destination id of 0 is a real id and an absent one means 0', () => {
  it('on instrument binding, clearing and track routing', () => {
    withEngine((engine) => {
      for (const absent of [undefined, null]) {
        engine.setBuiltinInstrument(absent, {});
        expect(engine.midiInstrumentCount).toBe(1);
        engine.clearMidiInstrument(absent);
        expect(engine.midiInstrumentCount).toBe(0);
      }
      engine.setBuiltinInstrument(0, {});
      expect(engine.midiInstrumentCount).toBe(1);
    });
    withProject((project, trackId) => {
      project.setTrackMidiDestination(trackId, 5);
      expect(project.tracks().find((t) => t.id === trackId)?.midiDestinationId).toBe(5);
      project.setTrackMidiDestination(trackId, undefined);
      expect(project.tracks().find((t) => t.id === trackId)?.midiDestinationId).toBe(0);
      project.setTrackMidiDestination(trackId, 5);
      project.setTrackMidiDestination(trackId, null);
      expect(project.tracks().find((t) => t.id === trackId)?.midiDestinationId).toBe(0);
      project.setTrackMidiDestination(trackId, 5);
      project.setTrackMidiDestination(trackId, 0);
      expect(project.tracks().find((t) => t.id === trackId)?.midiDestinationId).toBe(0);
    });
  });
});

describe('a MIDI clip length is absent for open-ended or a positive whole number', () => {
  const clip = (extra: Record<string, unknown>) => ({ events: [], ...extra });

  it('refuses 0, a negative and a fraction by name', () => {
    withEngine((engine) => {
      // A looping clip needs its loop length, so both fields are measured on one.
      const looping = (extra: Record<string, unknown>) =>
        clip({ lengthSamples: 100, loop: true, loopLengthSamples: 50, ...extra });
      for (const field of ['lengthSamples', 'loopLengthSamples']) {
        for (const bad of [0, -1]) {
          expect(() => engine.setMidiClips([looping({ [field]: bad })] as never)).toThrow(
            new RangeError(
              `RealtimeEngine.setMidiClips: clips[0].${field} must be an integer of at least 1`,
            ),
          );
        }
        // A sample position or length past 2^53 - 1 was never an exact JS number.
        for (const inexact of [1.5, 2 ** 53]) {
          expect(() => engine.setMidiClips([looping({ [field]: inexact })] as never)).toThrow(
            new RangeError(
              `RealtimeEngine.setMidiClips: ${field} must be an integer in [-9007199254740991, 9007199254740991]`,
            ),
          );
        }
      }
      for (const absent of [undefined, null]) {
        engine.setMidiClips([clip({ lengthSamples: absent })] as never);
        engine.setMidiClips([clip({ loopLengthSamples: absent })] as never);
      }
      expect(() =>
        engine.setMidiClips([clip({ loop: true, loopLengthSamples: undefined })] as never),
      ).toThrow(
        new RangeError(
          'RealtimeEngine.setMidiClips: clips[0].loopLengthSamples must be an integer of at least 1',
        ),
      );
      engine.setMidiClips([
        clip({ lengthSamples: 100, loop: true, loopLengthSamples: 50 }),
      ] as never);
    });
  });
});

describe('a required id of the wrong type is a TypeError with the facade text', () => {
  it('on setClipSource and setSourceAudio', () => {
    withProject((project, _track, clipId) => {
      expect(() => project.setClipSource(clipId, undefined as never)).toThrow(
        new TypeError('Project.setClipSource: sourceId must be an integer'),
      );
      expect(() =>
        project.setSourceAudio({
          sourceId: undefined as never,
          channels: AUDIO,
          sampleRate: 48000,
        }),
      ).toThrow(new TypeError('Project.setSourceAudio: sourceId must be an integer'));
      expect(() => project.setAudioSourceMetadata({ sourceId: 'x' as never })).toThrow(
        new TypeError('Project.setAudioSourceMetadata: sourceId must be an integer'),
      );
      expect(() => project.renameTrack(1, undefined as never)).toThrow(
        new TypeError('Project.renameTrack: name must be a string'),
      );
    });
  });
});

describe('a percussive-event field whose native 0 is the default refuses an explicit 0', () => {
  const sampleRate = 22050;
  const hits = new Float32Array(sampleRate).map((_, i) =>
    i % 4410 < 200 ? Math.sin((2 * Math.PI * 900 * i) / sampleRate) : 0,
  );
  const EXTRACT = [
    'nFft',
    'hopLength',
    'hpssKernelHarmonic',
    'hpssKernelPercussive',
    'onsetWait',
    'onsetDelta',
    'maxEventMs',
  ] as const;
  const RENDER = [
    'nFft',
    'hopLength',
    'hpssKernelHarmonic',
    'hpssKernelPercussive',
    'fadeMs',
  ] as const;
  const zeroText = (fnName: string, field: string) =>
    `${fnName}: ${field} must not be 0; omit it for the library value`;

  it.each(EXTRACT)('extractPercussiveEvents %s', (field) => {
    expect(() => extractPercussiveEvents({ samples: hits, sampleRate, [field]: 0 })).toThrow(
      new RangeError(zeroText('extractPercussiveEvents', field)),
    );
    expect(extractPercussiveEvents({ samples: hits, sampleRate, [field]: undefined })).toEqual(
      extractPercussiveEvents({ samples: hits, sampleRate }),
    );
  });

  it.each(RENDER)('renderPercussiveEvents %s', (field) => {
    const events = [{ onsetSample: 0, offsetSample: 200, edit: { muted: true } }];
    expect(() => renderPercussiveEvents({ samples: hits, sampleRate, events, [field]: 0 })).toThrow(
      new RangeError(zeroText('renderPercussiveEvents', field)),
    );
    expect(
      renderPercussiveEvents({ samples: hits, sampleRate, events, [field]: undefined }),
    ).toEqual(renderPercussiveEvents({ samples: hits, sampleRate, events }));
  });
});

describe('a whole-clip loop is spelt by omitting the loop length', () => {
  const ZERO = 'Project.setClipLoop: loopLengthPpq must not be 0; omit it to loop the whole clip';

  it('refuses an explicit 0 and reads a whole-clip loop back without a length', () => {
    withProject((project, _trackId, clipId) => {
      expect(project.clips()[0]).not.toHaveProperty('loopLengthPpq');
      expect(() => project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 0 })).toThrow(
        new RangeError(ZERO),
      );
      for (const loopLengthPpq of [undefined, null]) {
        project.setClipLoop(clipId, {
          loopMode: 'loop',
          loopLengthPpq: loopLengthPpq as number | undefined,
        });
        const clip = project.clips()[0];
        expect(clip.loopMode).toBe('loop');
        expect(clip).not.toHaveProperty('loopLengthPpq');
      }
      project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 2 });
      const looped = project.clips()[0];
      expect(looped.loopLengthPpq).toBe(2);
      project.setClipLoop(clipId, {
        loopMode: looped.loopMode,
        loopLengthPpq: looped.loopLengthPpq,
      });
      expect(project.clips()[0].loopLengthPpq).toBe(2);
    });
  });
});
