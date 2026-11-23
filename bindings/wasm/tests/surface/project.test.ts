import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project, ProjectTimeline } from '../../dist/index.js';

const SR = 48000;

function withProject(fn: (project: Project) => void): void {
  const project = new Project();
  project.setSampleRate(SR);
  try {
    fn(project);
  } finally {
    project.destroy();
  }
}

/** The project as an untyped bag, for removed names and wrong-typed arguments. */
function loose(project: Project): Record<string, (...args: unknown[]) => unknown> {
  return project as unknown as Record<string, (...args: unknown[]) => unknown>;
}

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

/** A metadata-only audio clip: an unresolved source the host fills later. */
function addUriClip(project: Project): { trackId: number; clipId: number } {
  const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
  const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'mem://a.wav' });
  return { trackId, clipId };
}

function clicks(bpm: number, seconds: number): Float32Array {
  const out = new Float32Array(Math.round(seconds * SR));
  const period = Math.round((60 / bpm) * SR);
  for (let start = 0; start < out.length; start += period) {
    for (let i = 0; i < 64 && start + i < out.length; i++) {
      out[start + i] = (1 - i / 64) * 0.8;
    }
  }
  return out;
}

beforeAll(async () => {
  await init();
});

describe('construction', () => {
  it('compiles a timeline that callers cannot construct', () => {
    withProject((project) => {
      const { timeline } = project.compile({ withTimeline: true });
      expect(timeline).not.toBeNull();
      try {
        expect('nativeId' in (timeline as object)).toBe(false);
      } finally {
        timeline?.destroy();
      }
    });
    expect(() => new (ProjectTimeline as unknown as new (native: unknown) => unknown)({})).toThrow(
      TypeError,
    );
  });
});

describe('closed sets', () => {
  it('refuses an ordinal with TypeError and an unknown name with RangeError', () => {
    withProject((project) => {
      expect(() => project.addTrack({ kind: 1 as never })).toThrow(TypeError);
      expect(() => project.addTrack({ kind: 'bus' as never })).toThrow(RangeError);
      const trackId = project.addTrack({ kind: 'audio' });
      expect(() => project.setTrackKind(trackId, 'aux')).not.toThrow();
      expect(project.tracks().find((t) => t.id === trackId)?.kind).toBe('aux');
      expect(() => project.setTrackKind(trackId, 1 as never)).toThrow(TypeError);
      expect(() => project.setTrackKind(trackId, 'bus' as never)).toThrow(RangeError);
      expect(() => project.setOverlapPolicy(1 as never)).toThrow(TypeError);
      expect(() => project.setOverlapPolicy('sometimes' as never)).toThrow(RangeError);
      project.setOverlapPolicy('allow');
      expect(project.overlapPolicy).toBe('allow');
      const { clipId } = addUriClip(project);
      expect(() => project.setClipWarpMode(clipId, 2 as never)).toThrow(TypeError);
    });
  });

  it('spells fade curves, warp modes and automation targets in lowerCamel', () => {
    withProject((project) => {
      const { trackId, clipId } = addUriClip(project);
      expect(() =>
        project.setClipFade(clipId, { fadeInPpq: 0.5, fadeInCurve: 'equalPower' }),
      ).not.toThrow();
      expect(() =>
        project.setClipFade(clipId, { fadeInPpq: 0.5, fadeInCurve: 'equal-power' as never }),
      ).toThrow(RangeError);
      for (const mode of ['off', 'repitch', 'tempoSync', 'timeStretch'] as const) {
        expect(() => project.setClipWarpMode(clipId, mode)).not.toThrow();
      }
      expect(() => project.setClipWarpMode(clipId, 'time-stretch' as never)).toThrow(RangeError);
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 20,
          targetKind: 'opaque',
          points: [{ ppq: 0, value: 0 }],
        }),
      ).not.toThrow();
      const faderLaneId = project.addAutomationLane(trackId, {
        targetKind: 'trackFaderDb',
        points: [{ ppq: 0, value: -3 }],
      });
      expect(() =>
        project.addAutomationLane(trackId, {
          targetKind: 'trackPan',
          points: [{ ppq: 0, value: 0 }],
        }),
      ).not.toThrow();
      expect(() =>
        project.editAutomationLane(trackId, faderLaneId, {
          targetKind: 'trackFaderDb',
          points: [{ ppq: 0, value: 0.5 }],
        }),
      ).not.toThrow();
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 30,
          targetKind: 'track-pan' as never,
          points: [{ ppq: 0, value: 0 }],
        }),
      ).toThrow(RangeError);
      expect(() =>
        project.editAutomationLane(trackId, faderLaneId, {
          targetKind: 'track-pan' as never,
          points: [{ ppq: 0, value: 0 }],
        }),
      ).toThrow(RangeError);
    });
  });
});

describe('edits', () => {
  it('sets a clip fade from one object whose lengths carry their unit', () => {
    withProject((project) => {
      const { clipId } = addUriClip(project);
      expect(() =>
        project.setClipFade(clipId, { fadeInPpq: 1, fadeOutPpq: 1, fadeOutCurve: 'linear' }),
      ).not.toThrow();
      expect(() => project.setClipFade(clipId, { lengthPpq: 1 } as never)).toThrow(/lengthPpq/);
      expect(() => loose(project).setClipFade(clipId, { lengthPpq: 1 }, { lengthPpq: 1 })).toThrow(
        TypeError,
      );
    });
  });

  it('sets a marker from one input object and reads every field back', () => {
    withProject((project) => {
      const id = project.setMarker({ ppq: 2, name: 'chorus', kind: 'cuePoint' });
      expect(project.markers()).toEqual([
        { id, ppq: 2, name: 'chorus', kind: 'cuePoint', keyFifths: 0, keyMinor: false },
      ]);
      const key = project.setMarker({
        ppq: 4,
        kind: 'keySignature',
        keyFifths: -3,
        keyMinor: true,
      });
      expect(key).not.toBe(id);
      const stored = project.markers().find((m) => m.id === key);
      expect(stored?.keyFifths).toBe(-3);
      expect(stored?.keyMinor).toBe(true);
      expect(project.setMarker({ id, ppq: 3, name: 'chorus' })).toBe(id);
      expect(project.markers().find((m) => m.id === id)?.ppq).toBe(3);
      const zero = caught(() => project.setMarker({ id: 0, ppq: 1 }));
      expect(zero).toBeInstanceOf(RangeError);
      expect(String((zero as Error).message)).toContain('omit it for the default');
      expect(() => loose(project).setMarker(1, 'verse')).toThrow(TypeError);
    });
  });

  it('takes optional ids in an options object and refuses an explicit zero', () => {
    withProject((project) => {
      const { trackId, clipId } = addUriClip(project);
      const other = project.addTrack({ kind: 'audio', name: 'other' });
      project.moveClip(clipId, 8, { trackId: other });
      expect(project.clips().find((c) => c.id === clipId)?.trackId).toBe(other);
      project.moveClip(clipId, 4);
      expect(project.clips().find((c) => c.id === clipId)?.trackId).toBe(other);
      expect(project.clips().find((c) => c.id === clipId)?.startPpq).toBe(4);
      expect(() => loose(project).moveClip(clipId, 4, trackId)).toThrow(TypeError);
      expect(() => project.moveClip(clipId, 4, { trackId: 0 })).toThrow(RangeError);
      expect(() => project.setClipTakes(clipId, [], { activeTakeId: 0 })).toThrow(RangeError);
      expect(() => project.setClipTakes(clipId, [])).not.toThrow();
      expect(() => loose(project).setClipTakes(clipId, [], 0)).toThrow(TypeError);
    });
  });

  it('sets clip loop, source audio and source metadata from one request object each', () => {
    withProject((project) => {
      const { clipId } = addUriClip(project);
      project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 2, loopCrossfadePpq: 0.25 });
      expect(project.clips().find((c) => c.id === clipId)?.loopMode).toBe('loop');
      expect(() => loose(project).setClipLoop(clipId, 'loop', 2)).toThrow(TypeError);

      const [sourceId] = project.unresolvedAudioSourceIds();
      const left = new Float32Array(480).fill(0.1);
      const right = new Float32Array(480).fill(-0.1);
      project.setSourceAudio({ sourceId, channels: [left, right], sampleRate: SR });
      expect(project.unresolvedAudioSourceIds()).toEqual([]);
      expect(() => loose(project).setSourceAudio(sourceId, left, 1, SR)).toThrow(TypeError);

      project.setAudioSourceMetadata({ sourceId, contentHash: 'abc', externalStemRole: 'vocals' });
      expect(project.sources().find((s) => s.id === sourceId)?.externalStemRole).toBe('vocals');
      expect(() => loose(project).setAudioSourceMetadata(sourceId, 'abc', 'vocals')).toThrow(
        TypeError,
      );
    });
  });

  it('reports the channel count of attached source audio', () => {
    withProject((project) => {
      addUriClip(project);
      const [sourceId] = project.unresolvedAudioSourceIds();
      const channel = new Float32Array(480).fill(0.1);
      project.setSourceAudio({ sourceId, channels: [channel, channel], sampleRate: SR });
      expect(project.sources().find((s) => s.id === sourceId)?.numChannels).toBe(2);
    });
  });

  it('adds an audio clip from planar channels', () => {
    withProject((project) => {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const left = new Float32Array(480).fill(0.1);
      const right = new Float32Array(480).fill(-0.1);
      const clipId = project.addClip({
        trackId,
        lengthPpq: 4,
        channels: [left, right],
        sampleRate: SR,
      });
      const clip = project.clips().find((c) => c.id === clipId);
      expect(project.sources().find((s) => s.id === clip?.sourceId)?.numChannels).toBe(2);
      expect(() =>
        project.addClip({ trackId, lengthPpq: 4, audio: left, audioSampleRate: SR } as never),
      ).toThrow(/unknown key audio \(expected /);

      const result = project.importExternalStems({
        sampleRate: SR,
        stems: [
          { name: 'vox', layout: 'stereo', channels: [left, right] },
          { name: 'bass', layout: 'mono', channels: [left] },
        ],
      });
      expect(result.trackIds).toHaveLength(2);
      expect(result.clipIds).toHaveLength(2);
      const stemSources = project
        .clips()
        .filter((c) => result.clipIds.includes(c.id))
        .map((c) => project.sources().find((s) => s.id === c.sourceId)?.numChannels);
      expect(stemSources).toEqual([2, 1]);
      expect(() =>
        project.importExternalStems({
          sampleRate: SR,
          stems: [{ name: 'x', layout: 'mono', planarSamples: [left] } as never],
        }),
      ).toThrow();
      expect(() =>
        project.importExternalStems({
          sampleRate: SR,
          stems: [{ name: 'x', layout: 'quad' as never, channels: [left] }],
        }),
      ).toThrow(RangeError);
    });
  });

  it('sets a program and a part rig from request objects with no positional form', () => {
    withProject((project) => {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() =>
        project.setProgram(clipId, 5, { group: 0, channel: 0, bank: { msb: 0, lsb: 1 } }),
      ).not.toThrow();
      expect(() => loose(project).setProgram(clipId, 5, 1)).toThrow(TypeError);
      expect(() => project.setProgram(clipId, 5, { bank: 1 as never })).toThrow(TypeError);

      expect(() => project.setPartRig({ destinationId: 1, part: 0, mode: 'none' })).not.toThrow();
      expect(project.partRig({ destinationId: 1, part: 0 })?.mode).toBe('none');
      expect(() => loose(project).setPartRig(1, 0, 'none')).toThrow(TypeError);
      project.clearPartRig({ destinationId: 1, part: 0 });
      expect(project.partRig({ destinationId: 1, part: 0 })).toBeNull();
      expect(() => loose(project).clearPartRig(1, 0)).toThrow(TypeError);

      project.setAssistSidecar({ moduleId: 'm', regionEndPpq: 4, payload: new Uint8Array([1]) });
      expect(project.assistSidecars().map((s) => s.moduleId)).toEqual(['m']);
      expect(() => loose(project).setAssistSidecar('m', 1, 0, 0, 4, new Uint8Array(1))).toThrow(
        TypeError,
      );

      const fx = project.bakeMidiFx({ clipId, config: {} });
      expect(fx).not.toHaveProperty('sourceIndex');
      const indexed = project.bakeMidiFx({ clipId, config: {}, withSourceIndex: true });
      expect(indexed.sourceIndex).toBeDefined();
      expect(() => loose(project).bakeMidiFx(clipId, '{}')).toThrow(TypeError);

      expect(project.rehydrateVocalEdits([], { cancel: () => false })).toEqual([]);
      expect(() => loose(project).rehydrateVocalEdits([], () => false)).toThrow(TypeError);
    });
  });
});

describe('compile and bounce', () => {
  it('returns compile diagnostics as typed records only', () => {
    withProject((project) => {
      project.addMidiClip(0, 4);
      const result = project.compile();
      expect(Object.keys(result)).toEqual(['diagnostics']);
      expect(result.diagnostics).toHaveLength(1);
      const [diagnostic] = result.diagnostics;
      expect(diagnostic.code).toBe('midiClipNoInstrument');
      expect(diagnostic.severity).toBe('warning');
      expect(diagnostic.message).toContain('MIDI');

      const compiled = project.compile({ withTimeline: true });
      try {
        expect(Object.keys(compiled).sort()).toEqual(['diagnostics', 'timeline']);
        expect(compiled.diagnostics).toEqual(result.diagnostics);
      } finally {
        compiled.timeline?.destroy();
      }

      project.bounce({ totalFrames: 128 });
      const last = project.lastBounceCompileResult();
      expect(Object.keys(last ?? {})).toEqual(['diagnostics']);
      expect(last?.diagnostics.map((d) => d.code)).toEqual(['midiClipNoInstrument']);
    });
  });

  it('compile carries the timeline only when withTimeline is set', () => {
    withProject((project) => {
      project.addMidiClip(0, 4);
      expect(project.compile()).not.toHaveProperty('timeline');
      expect(project.compile({ withTimeline: false })).not.toHaveProperty('timeline');
      const withTimeline = project.compile({ withTimeline: true });
      try {
        expect(withTimeline.timeline).toBeDefined();
      } finally {
        withTimeline.timeline?.destroy();
      }
      expect(() => project.compile({ withTimeline: 'yes' as never })).toThrow(TypeError);
      expect(() => project.compile({ other: true } as never)).toThrow(TypeError);
    });
  });

  it('bounces to planar channels', () => {
    withProject((project) => {
      const result = project.bounce({ totalFrames: 256, numChannels: 2 });
      expect(result).not.toHaveProperty('numChannels');
      expect(result.sampleRate).toBe(SR);
      expect(result.channels).toHaveLength(2);
      expect(result.channels[0]).toBeInstanceOf(Float32Array);
      expect(result.channels[0].length).toBe(256);
      expect(result.channels[1].length).toBe(256);
      expect(() => project.bounce({ totalFrames: 128, sampleRate: SR } as never)).toThrow(
        /unknown key sampleRate \(expected /,
      );
    });
  });

  it('installs a tempo from options with no positional boolean', () => {
    withProject((project) => {
      const samples = clicks(120, 4);
      expect(typeof project.autoTempo(samples, SR, { applyTimeSignatures: true })).toBe('number');
      expect(typeof project.autoTempo({ samples, sampleRate: SR, candidateIndex: 0 })).toBe(
        'number',
      );
      expect(() => loose(project).autoTempo(samples, SR, 0, true)).toThrow(TypeError);
      const candidates = project.analyzeTempo(samples, SR);
      expect(candidates.length).toBeGreaterThan(0);
      expect(project.analyzeTempo({ samples, sampleRate: SR })).toEqual(candidates);
      expect(() => loose(project).analyzeTempo(samples, SR, 0)).toThrow(TypeError);
    });
  });
});
