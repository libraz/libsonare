import { describe, expect, it } from 'vitest';
import {
  midiNoteOff,
  midiNoteOn,
  Project,
  ProjectTimeline,
  RealtimeEngine,
} from '../../dist/index.js';

const SR = 48000;

function newProject(): Project {
  const project = new Project();
  project.setSampleRate(SR);
  return project;
}

function withProject(fn: (project: Project) => void): void {
  const project = newProject();
  try {
    fn(project);
  } finally {
    project.destroy();
  }
}

/** The project as an untyped bag, for removed names and wrong-typed arguments. */
function loose(value: unknown): Record<string, (...args: unknown[]) => unknown> {
  return value as Record<string, (...args: unknown[]) => unknown>;
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

/** One sustained note routed to destination 0; returns the clip id. */
function addMidiNote(project: Project): number {
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
  ]);
  return clipId;
}

describe('construction', () => {
  it('constructs with new and has no static create', () => {
    const project = new Project();
    try {
      expect(typeof project.toJson()).toBe('string');
    } finally {
      project.destroy();
    }
    expect('create' in Project).toBe(false);
  });

  it('compiles a timeline that callers cannot construct and that exposes no native id', () => {
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

describe('reads', () => {
  it('reads the sample rate and overlap policy as getters', () => {
    withProject((project) => {
      expect(Object.getOwnPropertyDescriptor(Project.prototype, 'sampleRate')?.get).toBeTypeOf(
        'function',
      );
      expect(Object.getOwnPropertyDescriptor(Project.prototype, 'overlapPolicy')?.get).toBeTypeOf(
        'function',
      );
      expect(project.sampleRate).toBe(SR);
      expect(project.overlapPolicy).toBe('disallow');
      project.setOverlapPolicy('allow');
      expect(project.overlapPolicy).toBe('allow');
      for (const name of ['getSampleRate', 'getOverlapPolicy', 'getPartRig', 'getAssistSidecar']) {
        expect(name in project).toBe(false);
      }
      expect('getVocalEditDependencies' in project).toBe(false);
      expect(project.vocalEditDependencies()).toEqual([]);
    });
  });

  it('lists every collection as an array with no count or index accessors', () => {
    withProject((project) => {
      const trackId = project.addTrack({ kind: 'midi', name: 'keys' });
      project.setMarker({ ppq: 1, name: 'verse' });
      expect(project.tracks().map((t) => t.id)).toEqual([trackId]);
      expect(project.markers()).toHaveLength(1);
      expect(Array.isArray(project.clips())).toBe(true);
      expect(Array.isArray(project.sources())).toBe(true);
      expect(Array.isArray(project.tempoSegments())).toBe(true);
      expect(Array.isArray(project.timeSignatures())).toBe(true);
      expect(Array.isArray(project.assistSidecars())).toBe(true);
      for (const name of [
        'trackCount',
        'trackByIndex',
        'clipCount',
        'clipByIndex',
        'markerCount',
        'markerByIndex',
        'sourceCount',
        'sourceByIndex',
        'tempoSegmentCount',
        'tempoSegmentByIndex',
        'timeSignatureCount',
        'timeSignatureByIndex',
        'assistSidecarCount',
      ]) {
        expect(name in project).toBe(false);
      }
    });
  });

  it('reports track kind, clip loop mode, source kind and channel count by name', () => {
    withProject((project) => {
      const { trackId, clipId } = project.addMidiClip(0, 4);
      const track = project.tracks().find((t) => t.id === trackId);
      expect(track?.kind).toBe('midi');
      const clip = project.clips().find((c) => c.id === clipId);
      expect(clip?.loopMode).toBe('off');
      expect(clip?.sourceKind).toBe('midi');
      const source = project.sources().find((s) => s.id === clip?.sourceId);
      expect(source?.kind).toBe('midi');
      expect(source).toHaveProperty('numChannels');
      expect(source).not.toHaveProperty('channelCount');
    });
  });
});

describe('closed sets', () => {
  it('refuses an ordinal with TypeError and an unknown name with RangeError', () => {
    withProject((project) => {
      expect(() => project.addTrack({ kind: 1 as never })).toThrow(TypeError);
      const { trackId } = addUriClip(project);
      expect(() => project.setTrackKind(trackId, 1 as never)).toThrow(TypeError);
      expect(() => project.setTrackKind(trackId, 'bus' as never)).toThrow(RangeError);
      project.setTrackKind(trackId, 'audio');
      expect(() => project.addTrack({ kind: 'bus' as never })).toThrow(RangeError);
      expect(() => project.setOverlapPolicy(1 as never)).toThrow(TypeError);
      expect(() => project.setOverlapPolicy('sometimes' as never)).toThrow(RangeError);
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
      expect(() =>
        project.annotateKeys([{ startPpq: 0, endPpq: 4, tonicPc: 0, mode: 'dorian' }]),
      ).not.toThrow();
      expect(() => project.annotateKeys([{ startPpq: 0, endPpq: 4, mode: 1 as never }])).toThrow(
        TypeError,
      );
      expect(() =>
        project.annotateChords([{ startPpq: 0, endPpq: 4, rootPc: 0, quality: 'halfDiminished' }]),
      ).not.toThrow();
      expect(() =>
        project.annotateChords([{ startPpq: 0, endPpq: 4, quality: 'half-diminished' as never }]),
      ).toThrow(RangeError);
      expect(() => project.setClipWarpMode(clipId, 'tempoSync')).not.toThrow();
      expect(() => project.setClipWarpMode(clipId, 'time-stretch' as never)).toThrow(RangeError);
      expect(() =>
        project.addAutomationLane(trackId, {
          targetKind: 'trackFaderDb',
          points: [{ ppq: 0, value: -3 }],
        }),
      ).not.toThrow();
      expect(() =>
        project.addAutomationLane(trackId, {
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
      expect(() => project.setClipFade(clipId, { lengthPpq: 1 } as never)).toThrow(TypeError);
      expect(() => project.setClipFade(clipId, { lengthPpq: 1 } as never)).toThrow(/lengthPpq/);
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
      expect(project.markers().find((m) => m.id === key)?.keyFifths).toBe(-3);
      expect(project.setMarker({ id, ppq: 3, name: 'chorus' })).toBe(id);
      const zero = caught(() => project.setMarker({ id: 0, ppq: 1 }));
      expect(zero).toBeInstanceOf(RangeError);
      expect(String((zero as Error).message)).toContain('omit it for the default');
      expect('setMarkerEx' in project).toBe(false);
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
      expect(() => loose(project).moveClip(clipId, 4, trackId)).toThrow(TypeError);
      expect(() => project.setClipTakes(clipId, [], { activeTakeId: 0 })).toThrow(RangeError);
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
      const stem = (extra: object) => ({
        sampleRate: SR,
        stems: [{ name: 'vox', layout: 'stereo', channels: [left, right], ...extra }],
      });
      const imported = project.importExternalStems(stem({ role: 'vocals' }) as never);
      expect(imported.trackIds).toHaveLength(1);
      expect(imported.clipIds).toHaveLength(1);
      expect(() =>
        project.importExternalStems(
          stem({ channels: undefined, planarSamples: [left, right] }) as never,
        ),
      ).toThrow();
      expect(() => project.importExternalStems(stem({ layout: 2 }) as never)).toThrow(TypeError);
      expect(() =>
        project.addClip({ trackId, lengthPpq: 4, audio: left, audioSampleRate: SR } as never),
      ).toThrow(/unknown key audio \(expected /);
    });
  });

  it('sets a program and a part rig from request objects with no positional form', () => {
    withProject((project) => {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, 5, { bank: { msb: 0, lsb: 1 } })).not.toThrow();
      expect(() => loose(project).setProgram(clipId, 5, 1)).toThrow(TypeError);
      expect(() => loose(project).setPartRig(0, 0, 'none')).toThrow(TypeError);
      project.setPartRig({ destinationId: 3, part: 1, mode: 'none' });
      expect(project.partRig({ destinationId: 3, part: 1 })).toEqual({ mode: 'none' });
      expect(() => loose(project).clearPartRig(3, 1)).toThrow(TypeError);
      project.clearPartRig({ destinationId: 3, part: 1 });
      expect(project.partRig({ destinationId: 3, part: 1 })).toBeNull();

      project.setAssistSidecar({
        moduleId: 'm',
        schemaVersion: 2,
        payload: new Uint8Array([1, 2]),
      });
      const [sidecar] = project.assistSidecars();
      expect(sidecar.moduleId).toBe('m');
      expect(sidecar.schemaVersion).toBe(2);
      expect(Array.from(sidecar.payload)).toEqual([1, 2]);
      expect(() => loose(project).setAssistSidecar('m', 2)).toThrow(TypeError);
    });
  });
});

describe('compile and bounce', () => {
  it('returns compile diagnostics as typed records only', () => {
    withProject((project) => {
      project.addMidiClip(0, 4);
      const result = project.compile();
      expect(result).not.toHaveProperty('messages');
      expect(result).not.toHaveProperty('diagnosticCount');
      expect(result).not.toHaveProperty('hasTimeline');
      expect(result.diagnostics).toHaveLength(1);
      const [diagnostic] = result.diagnostics;
      expect(diagnostic.code).toBe('midiClipNoInstrument');
      expect(diagnostic.severity).toBe('warning');
      expect(diagnostic.message).toContain('MIDI');

      const compiled = project.compile({ withTimeline: true });
      try {
        expect(Object.keys(compiled).sort()).toEqual(['diagnostics', 'timeline']);
        expect(compiled.diagnostics.map((d) => d.code)).toEqual(['midiClipNoInstrument']);
      } finally {
        compiled.timeline?.destroy();
      }

      project.bounce({ totalFrames: 128 });
      const last = project.lastBounceCompileResult();
      expect(Object.keys(last ?? {})).toEqual(['diagnostics']);
      expect(last?.diagnostics[0].severity).toBe('warning');
    });
  });

  it('bounces with no sample-rate option and refuses an explicit zero setting', () => {
    withProject((project) => {
      expect(() => project.bounce({ totalFrames: 128, sampleRate: SR } as never)).toThrow(
        /unknown key sampleRate \(expected /,
      );
      const zeroBlock = caught(() => project.bounce({ totalFrames: 128, blockSize: 0 }));
      expect(zeroBlock).toBeInstanceOf(RangeError);
      expect(String((zeroBlock as Error).message)).toContain('omit it for the default');
      expect(() => project.bounce({ totalFrames: 128, numChannels: 0 })).toThrow(RangeError);
    });
  });

  it('bounces to planar channels', () => {
    withProject((project) => {
      const result = project.bounce({ totalFrames: 256, numChannels: 2 });
      expect(result.sampleRate).toBe(SR);
      expect(result.channels).toHaveLength(2);
      expect(result.channels[0]).toBeInstanceOf(Float32Array);
      expect(result.channels[0].length).toBe(256);
      expect(result).not.toHaveProperty('samples');
      expect(result).not.toHaveProperty('numChannels');
      expect(project.bounce({ totalFrames: 64 }).channels.length).toBeGreaterThan(0);
    });
  });

  it('bounces to planar channels from a baked MIDI clip and from an engine', () => {
    withProject((project) => {
      const { clipId } = project.addMidiClip(0, 4);
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
      ]);
      const config = { chordIntervals: [0, 4, 7] };
      expect(project.bakeMidiFx({ clipId, config })).toEqual({});
      expect(() => loose(project).bakeMidiFx(clipId, config)).toThrow(TypeError);
      const clipId2 = project.addMidiClip(0, 4).clipId;
      project.setMidiEvents(clipId2, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
      ]);
      const baked = project.bakeMidiFx({
        clipId: clipId2,
        config,
        withSourceIndex: true,
      });
      expect(baked.sourceIndex).toBeInstanceOf(Int32Array);
      expect(baked.sourceIndex).toHaveLength(6);
    });
    const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: 128 });
    try {
      const result = engine.bounceOffline({ totalFrames: 256, blockSize: 128, numChannels: 2 });
      expect(result.sampleRate).toBe(SR);
      expect(result.channels).toHaveLength(2);
      expect(result.channels[0]).toBeInstanceOf(Float32Array);
      expect(result.channels[0].length).toBe(256);
      expect(typeof result.integratedLufs).toBe('number');
      expect(result).not.toHaveProperty('interleaved');
      expect(result).not.toHaveProperty('numChannels');
    } finally {
      engine.destroy();
    }
  });

  it('binds each instrument kind through bounce options taking destination and instrument pairs', () => {
    withProject((project) => {
      for (const name of [
        'bounceWithBuiltinInstruments',
        'bounceWithSynthInstruments',
        'bounceWithSf2Instruments',
        'bounceWithInstruments',
      ]) {
        expect(name in project).toBe(false);
      }
      const pair = { destinationId: 0, instrument: { waveform: 'saw' as const } };
      expect(() => project.bounce({ totalFrames: 128, builtinInstruments: pair as never })).toThrow(
        TypeError,
      );
      expect(() => project.bounce({ totalFrames: 128, builtinInstruments: [pair] })).not.toThrow();
      expect(() =>
        project.bounce({
          totalFrames: 128,
          builtinInstruments: [pair, { destinationId: 1, instrument: {} }],
        }),
      ).not.toThrow();
      const peak = (r: { channels: Float32Array[] }) =>
        Math.max(...r.channels.map((c) => c.reduce((m, v) => Math.max(m, Math.abs(v)), 0)));
      const midi = addMidiNote(project);
      expect(midi).toBeGreaterThan(0);
      const options = { totalFrames: 4096, numChannels: 2 };
      const builtin = project.bounce({ ...options, builtinInstruments: [pair] });
      expect(builtin.channels).toHaveLength(2);
      expect(peak(builtin)).toBeGreaterThan(0);
      expect(
        peak(project.bounce({ ...options, synthInstruments: [{ instrument: {} }] })),
      ).toBeGreaterThan(0);
      const sf2 = project.bounce({
        ...options,
        sf2Instruments: [{ destinationId: 0, instrument: { gain: 0.8 } }],
      });
      expect(sf2.channels).toHaveLength(2);
      const external = {
        prepare() {},
        onEvent() {},
        render(outputs: Float32Array[], frames: number) {
          for (const channel of outputs) {
            channel.fill(0.25, 0, frames);
          }
        },
      };
      const called = project.bounce({
        ...options,
        instruments: [{ destinationId: 0, instrument: external }],
      });
      expect(called.channels).toHaveLength(2);
      expect(called.sampleRate).toBe(SR);
      expect(peak(called)).toBeCloseTo(0.25, 6);
      expect(() =>
        project.bounce({
          totalFrames: 128,
          builtinInstruments: [
            { destinationId: 0, instrument: { destinationId: 1, waveform: 'saw' } },
          ] as never,
        }),
      ).toThrow(
        new TypeError(
          'Project.bounce: unknown key builtinInstruments[0].instrument.destinationId (expected builtinInstruments[0].destinationId)',
        ),
      );
    });
  });

  it('refuses tempo option shape before rate or sample content in either call form', () => {
    withProject((project) => {
      const samples = new Float32Array([Number.NaN]);
      for (const key of [
        'candidateIndex',
        'applyTimeSignatures',
        'adaptiveTempo',
        'tempoUpdateIntervalBeats',
        'rampThreshold',
        'includeOctaveCandidates',
      ] as const) {
        const options = { [key]: 'wrong' } as never;
        for (const call of [
          () => project.autoTempo({ samples, sampleRate: 1, [key]: 'wrong' } as never),
          () => project.autoTempo(samples, SR, options),
        ]) {
          expect(call).toThrow(TypeError);
          expect(call).toThrow(new RegExp(`^Project\\.autoTempo: ${key} must be `));
        }
      }
    });
  });

  it('installs a tempo from options with no positional boolean, in either call form', () => {
    withProject((project) => {
      const samples = clicks(120, 4);
      const stored = [{ startPpq: 0, numerator: 7, denominator: 8 }];
      project.setTimeSignatures(stored);
      const [candidate] = project.analyzeTempo(samples, SR);
      const detected = project.autoTempo({ samples, sampleRate: SR, candidateIndex: 0 });
      expect(project.timeSignatures()).toEqual(stored);
      expect(detected.bpm).toBeCloseTo(candidate.bpm);
      expect(detected.bpmConfidence).toBeCloseTo(candidate.confidence);
      expect(detected.timeSignature).toMatchObject({
        numerator: candidate.firstTimeSignature.numerator,
        denominator: candidate.firstTimeSignature.denominator,
        confidence: expect.any(Number),
      });
      const applied = project.autoTempo(samples, SR, { applyTimeSignatures: true });
      expect(applied).toEqual(detected);
      expect(project.timeSignatures()[0]).toMatchObject({
        numerator: applied.timeSignature.numerator,
        denominator: applied.timeSignature.denominator,
      });
      expect(() => loose(project).autoTempo(samples, SR, 0, true)).toThrow(TypeError);
      expect(candidate).not.toHaveProperty('timeSignature');
      expect(project.tempoSegments()[0]).not.toHaveProperty('startSample');
    });
  });
});

describe('files and names', () => {
  it('exports MIDI files as plain bytes', () => {
    withProject((project) => {
      project.addMidiClip(0, 4);
      for (const bytes of [project.exportSmf(), project.exportClipFile()]) {
        expect(bytes).toBeInstanceOf(Uint8Array);
        expect(Buffer.isBuffer(bytes)).toBe(false);
        expect(bytes.length).toBeGreaterThan(8);
      }
    });
  });

  it('requires a name to rename a track', () => {
    withProject((project) => {
      const { trackId } = project.addMidiClip(0, 4);
      expect(() => loose(project).renameTrack(trackId)).toThrow(TypeError);
      project.renameTrack(trackId, 'lead');
      expect(project.tracks().find((t) => t.id === trackId)?.name).toBe('lead');
    });
  });
});
