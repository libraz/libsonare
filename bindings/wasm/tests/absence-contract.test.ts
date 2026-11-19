/**
 * Absence is spelt by omission: an unset value reads back as a missing key, an empty or zero
 * value that stands for "not given" is refused by name, and `undefined` and `null` both mean
 * "not given". The Node package carries the same cases in its own copy of this file.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project, RealtimeEngine } from '../dist/index.js';

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

const EMPTY_MARKER = 'marker name must not be empty; omit it for an unnamed marker';

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
      expect(() => project.setMarker({ ppq: 0, name: '' })).toThrow(new RangeError(EMPTY_MARKER));
      expect(() => project.setMarker({ ppq: 0, name: 5 as never })).toThrow(
        new TypeError('setMarker: name must be a string'),
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
      expect(() => engine.setMarkers([{ ppq: 0, name: '' }])).toThrow(new RangeError(EMPTY_MARKER));
      engine.setMarkers([{ ppq: 0, name: null as never }]);
      expect(engine.markers()[0]).not.toHaveProperty('name');
    });
  });

  it('names a track, a warp map, a take and source metadata', () => {
    withProject((project, trackId, clipId) => {
      expect(() => project.addTrack({ name: '' })).toThrow(
        new RangeError('addTrack: name must not be empty; omit it for no name'),
      );
      expect(() => project.addTrack({ name: 5 as never })).toThrow(
        new TypeError('addTrack: name must be a string'),
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
        new RangeError('setWarpMap: name must not be empty; omit it for no name'),
      );
      project.setWarpMap({ id: 1, name: null as never, anchors });

      expect(() => project.setClipTakes(clipId, [{ id: 1, name: '' }])).toThrow(
        new RangeError(
          'setClipTakes: takes[0].name must not be empty; omit it for an unnamed take',
        ),
      );
      const sourceId = project.sources()[0].id;
      expect(() => project.setAudioSourceMetadata({ sourceId, contentHash: '' })).toThrow(
        new RangeError(
          'setAudioSourceMetadata: contentHash must not be empty; omit it to clear it',
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
          new TypeError('setClipTakes: takes must be an array'),
        );
        expect(() => project.setClipCompSegments(clipId, absent as never)).toThrow(
          new TypeError('setClipCompSegments: segments must be an array'),
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
        new TypeError('setClipTakes: takes[0].id must be a number'),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 0 }])).toThrow(
        new RangeError('setClipTakes: takes[0].id must not be 0'),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 1, sourceId: 0 }])).toThrow(
        new RangeError(
          "setClipTakes: takes[0].sourceId must not be 0 (omit it for the clip's source)",
        ),
      );
      expect(() => project.setClipTakes(clipId, [{ id: 1 }], { activeTakeId: 0 })).toThrow(
        new RangeError('setClipTakes: activeTakeId must not be 0 (omit it for the default)'),
      );
      project.setClipTakes(clipId, [{ id: 1, sourceId: null as never }], {
        activeTakeId: null as never,
      });
      expect(() =>
        project.setClipCompSegments(clipId, [{ startPpq: 0, endPpq: 1, takeId: 0 }]),
      ).toThrow(
        new RangeError(
          'setClipCompSegments: segments[0].takeId must not be 0 (omit it for the active take)',
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
        new RangeError('addClip: trackId must not be 0; a clip needs an existing track'),
      );
      expect(() => project.moveClip(clipId, 1, { trackId: 0 })).toThrow(
        new RangeError('moveClip: trackId must not be 0 (omit it to keep the track)'),
      );
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
      expect(() => engine.setGraph(graph({}, 0))).toThrow(
        new RangeError('setGraph: nodes[0].numPorts must not be 0 (omit it for the default)'),
      );
      expect(() => engine.setGraph(graph({}, -1))).toThrow(/numPorts must be a positive integer/);
      expect(() => engine.setGraph(graph({}, 1.5))).toThrow(/numPorts must be a positive integer/);
      expect(() => engine.setGraph(graph({ numChannels: 0 }))).toThrow(
        new RangeError('setGraph: numChannels must not be 0 (omit it for the default)'),
      );
      engine.setGraph(graph({ numChannels: null }, undefined));
      expect(engine.graphNodeCount).toBe(2);
      expect(() => engine.setBuiltinInstrument(0, { polyphony: 0 })).toThrow(
        new RangeError('setBuiltinInstrument: polyphony must not be 0 (omit it for the default)'),
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
        new RangeError('setGraph: inputNode must not be empty'),
      );
      expect(() => engine.setGraph(graph({ outputNode: '' }) as never)).toThrow(
        new RangeError('setGraph: outputNode must not be empty'),
      );
      for (const absent of [undefined, null]) {
        expect(() => engine.setGraph(graph({ inputNode: absent }) as never)).toThrow(
          new TypeError('setGraph: inputNode must be a string'),
        );
      }
      expect(() => engine.setGraph(graph({ nodes: [{ id: '' }] }) as never)).toThrow(
        new RangeError('setGraph: nodes[0].id must not be empty'),
      );
    });
  });

  it('on a freeze clip id', () => {
    withEngine((engine) => {
      expect(() => engine.freezeOffline({ totalFrames: 128, numChannels: 2, clipId: 0 })).toThrow(
        new RangeError('freezeOffline: clipId must not be 0 (omit it for the default)'),
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
      for (const field of ['lengthSamples', 'loopLengthSamples']) {
        expect(() => engine.setMidiClips([clip({ [field]: 0 })] as never)).toThrow(
          new RangeError(
            `setMidiClips: clips[0].${field} must not be 0 (omit it for ${
              field === 'lengthSamples' ? 'open-ended' : 'no loop length'
            })`,
          ),
        );
        for (const bad of [-1, 1.5]) {
          expect(() => engine.setMidiClips([clip({ [field]: bad })] as never)).toThrow(
            new RangeError(`setMidiClips: clips[0].${field} must be a positive integer`),
          );
        }
        engine.setMidiClips([clip({ [field]: undefined })] as never);
        engine.setMidiClips([clip({ [field]: null })] as never);
      }
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
        new TypeError('setClipSource: sourceId must be a number'),
      );
      expect(() =>
        project.setSourceAudio({
          sourceId: undefined as never,
          channels: AUDIO,
          sampleRate: 48000,
        }),
      ).toThrow(new TypeError('setSourceAudio: sourceId must be a number'));
      expect(() => project.setAudioSourceMetadata({ sourceId: 'x' as never })).toThrow(
        new TypeError('setAudioSourceMetadata: sourceId must be a number'),
      );
      expect(() => project.renameTrack(1, undefined as never)).toThrow(
        new TypeError('renameTrack: name must be a string'),
      );
    });
  });
});
