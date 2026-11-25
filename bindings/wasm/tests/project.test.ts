/**
 * Headless DAW project WASM binding tests.
 *
 * The WASM `Project` binding mirrors the Node/Python project surface and drives
 * the same C ABI through embind.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type {
  ChannelLayout,
  ProjectBounceResult,
  ProjectChordSymbol,
  ProjectKeySegment,
} from '../dist/index.js';
import {
  controllerProfileNames,
  ErrorCode,
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmDrumNoteForName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  gmProgramForName,
  init,
  isSonareError,
  midi2Cc,
  midiBankProgram,
  midiCc,
  midiCcIndexForName,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiChannelPressure,
  midiNoteOff,
  midiNoteOn,
  midiParamToCc,
  midiPitchBend,
  midiPolyPressure,
  midiProgram,
  midiRouteEvents,
  Project,
  perNoteControllerName,
  projectAbiVersion,
  synthGsDrumKitIsVoicedApart,
  synthGsDrumKitName,
  synthGsVariationIsVoicedApart,
} from '../dist/index.js';
import { EXPECTED_PROJECT_ABI_VERSION } from '../src/project_types';
import { expectArgumentRefusal, expectRefusalOf, unknownKeyText } from './_helpers';

describe('Sonare WASM Project', () => {
  beforeAll(async () => {
    await init();
  });

  /** Interleave planar bounce channels so frame-order comparisons read as one buffer. */
  function interleaved(result: ProjectBounceResult): Float32Array {
    const frames = result.channels[0]?.length ?? 0;
    const out = new Float32Array(frames * result.channels.length);
    result.channels.forEach((plane, c) => {
      for (let i = 0; i < frames; i++) {
        out[i * result.channels.length + c] = plane[i];
      }
    });
    return out;
  }

  function buildProject(): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const track = project.addTrack({ kind: 'audio', name: 'lead' });
    const audio = new Float32Array(480);
    for (let i = 0; i < audio.length; i++) {
      audio[i] = Math.sin(i * 0.05) * 0.25;
    }
    project.addClip({
      trackId: track,
      startPpq: 0,
      lengthPpq: 4,
      channels: [audio],
      sampleRate: 48000,
    });
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    return project;
  }

  it('requires the start of a key annotation and accepts explicit zero', () => {
    const project = new Project();
    try {
      const key = { endPpq: 4, tonicPc: 0, mode: 'major' as const };
      const error = expectRefusalOf(() => project.annotateKeys([key] as never), TypeError);
      expect(error.message).toContain('keys[0].startPpq');
      expect(project.keyAnnotations()).toEqual([]);
      project.annotateKeys([{ ...key, startPpq: 0 }]);
      expect(project.keyAnnotations()).toEqual([{ ...key, startPpq: 0 }]);
    } finally {
      project.destroy();
    }
  });

  it('reads PPQ, seconds, bar and beat from the project time map', () => {
    const project = new Project();
    try {
      project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
      project.setTimeSignatures([{ startPpq: 0, numerator: 4, denominator: 4 }]);

      expect(project.ppqToSec(7.25)).toBeCloseTo(3.625, 12);
      expect(project.secToPpq(3.625)).toBeCloseTo(7.25, 12);
      expect(project.barStartPpq(1)).toBe(4);
      expect(project.positionAtPpq(7.25)).toEqual({
        bpm: 120,
        timeSignature: { startPpq: 0, numerator: 4, denominator: 4 },
        barStartPpq: 4,
        barIndex: 1,
        beat: 4,
        beatFraction: 0.25,
      });
    } finally {
      project.destroy();
    }
  });

  it('reads the effective end PPQ of audible project content', () => {
    const project = new Project();
    try {
      expect(project.endPpq()).toBe(0);

      const first = project.addMidiClip(1.25, 2.5);
      project.setMidiEvents(first.clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 2.5, group: 0, channel: 0, note: 60, velocity: 0 }),
      ]);
      expect(project.endPpq()).toBe(3.75);

      const later = project.addMidiClip(6, 2);
      project.setMidiEvents(later.clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 64, velocity: 100 }),
        midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 64, velocity: 0 }),
      ]);
      expect(project.endPpq()).toBe(8);
    } finally {
      project.destroy();
    }
  });

  it('refuses invalid time coordinates and unsafe bar indexes', () => {
    const project = new Project();
    try {
      expect(expectRefusalOf(() => project.ppqToSec(-1), RangeError).message).toContain('ppq');
      expect(expectRefusalOf(() => project.secToPpq(-1), RangeError).message).toContain('sec');
      expect(expectRefusalOf(() => project.positionAtPpq(1e12 + 1), RangeError).message).toContain(
        'ppq',
      );
      expect(expectRefusalOf(() => project.barStartPpq(1.5), TypeError).message).toContain(
        'barIndex',
      );
      expect(expectRefusalOf(() => project.barStartPpq(-1), RangeError).message).toContain(
        'barIndex',
      );
      expect(
        expectRefusalOf(() => project.barStartPpq(Number.MAX_SAFE_INTEGER + 1), RangeError).message,
      ).toContain('barIndex');
    } finally {
      project.destroy();
    }
  });

  it('refuses a position whose bar index is lossy as a JavaScript number', () => {
    const project = new Project();
    try {
      project.setTimeSignatures([
        { startPpq: 0, numerator: 1, denominator: 1_073_741_824 },
        { startPpq: 33_554_432, numerator: 4, denominator: 4 },
      ]);
      let caught: unknown;
      try {
        project.positionAtPpq(33_554_436);
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({ code: ErrorCode.InvalidFormat });
    } finally {
      project.destroy();
    }
  });

  it('round-trips MIDI SysEx bytes in detached event arrays', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      const payload = Uint8Array.of(0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7);
      const original = payload.slice();
      const note = midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 });
      project.setMidiEvents(clipId, [
        { ppq: 1, word0: 0x30000000, word1: 0, sysex: original },
        note,
      ]);
      original.fill(0);

      const expected = [note, { ppq: 1, word0: 0x30000000, word1: 0, sysex: payload }];
      const events = project.midiEvents(clipId);
      expect(events).toStrictEqual(expected);
      expect(events[0]).not.toHaveProperty('sysex');

      project.setMidiEvents(clipId, events);
      expect(project.midiEvents(clipId)).toStrictEqual(expected);

      if (events[1]?.sysex === undefined) {
        throw new Error('SysEx bytes were not returned');
      }
      events[1].sysex[0] = 0;
      expect(project.midiEvents(clipId)).toStrictEqual(expected);

      expect(
        expectRefusalOf(
          () =>
            project.setMidiEvents(clipId, [{ ppq: 0, word0: 0x30000000, sysex: new Uint8Array() }]),
          RangeError,
        ).message,
      ).toContain('events[0].sysex');
    } finally {
      project.destroy();
    }
  });

  it('reads section annotations in stored order and restores them with undo', () => {
    const project = new Project();
    try {
      const sections = [
        { startPpq: 4, endPpq: 12, label: '東京' },
        { startPpq: 0, endPpq: 8, label: 'Intro' },
      ];
      expect(project.sectionAnnotations()).toEqual([]);
      project.annotateSections(sections);
      expect(project.sectionAnnotations()).toEqual(sections);

      project.undo();
      expect(project.sectionAnnotations()).toEqual([]);
      project.redo();
      expect(project.sectionAnnotations()).toEqual(sections);
    } finally {
      project.destroy();
    }
  });

  it('names invalid section labels and spans', () => {
    const project = new Project();
    try {
      expect(
        expectRefusalOf(
          () => project.annotateSections([{ startPpq: 4, endPpq: 4, label: 'Verse' }]),
          RangeError,
        ).message,
      ).toContain('sections[0].endPpq');
      expect(
        expectRefusalOf(
          () => project.annotateSections([{ startPpq: 0, endPpq: 4, label: '  \t' }]),
          RangeError,
        ).message,
      ).toContain('sections[0].label');
    } finally {
      project.destroy();
    }
  });

  it('names an undeclared section field by its indexed path', () => {
    const project = new Project();
    try {
      const error = expectRefusalOf(
        () =>
          project.annotateSections([
            { startPpq: 0, endPpq: 4, label: 'Verse', unexpected: true } as never,
          ]),
        TypeError,
      );
      expect(error.message).toContain('sections[0].unexpected');
    } finally {
      project.destroy();
    }
  });

  it('round-trips key annotations in stored order', () => {
    const project = new Project();
    try {
      const keys: ProjectKeySegment[] = [
        { startPpq: 0, endPpq: 4, tonicPc: 7, mode: 'major' },
        { startPpq: 4, endPpq: 8, mode: 'minor' },
      ];
      expect(project.keyAnnotations()).toEqual([]);
      project.annotateKeys(keys);
      expect(project.keyAnnotations()).toStrictEqual(keys);
    } finally {
      project.destroy();
    }
  });

  it('round-trips absent Roman numerals and refuses a specified empty numeral', () => {
    const project = new Project();
    try {
      const chords: ProjectChordSymbol[] = [
        {
          startPpq: 0,
          endPpq: 4,
          rootPc: 0,
          quality: 'dominant',
          extensions: [7, 9],
          slashBassPc: 5,
          modulationBoundary: true,
        },
        {
          startPpq: 4,
          endPpq: 8,
          rootPc: 2,
          quality: 'minor',
          extensions: [],
          modulationBoundary: false,
        },
        {
          startPpq: 8,
          endPpq: 12,
          rootPc: 7,
          quality: 'major',
          extensions: [7],
          romanNumeral: 'V',
          modulationBoundary: false,
        },
      ];
      const annotateEmpty = () => project.annotateChords([{ ...chords[0], romanNumeral: '' }]);
      expect(annotateEmpty).toThrow(RangeError);
      expect(annotateEmpty).toThrow(/chords\[0\]\.romanNumeral.*empty/);
      expect(project.chordAnnotations()).toEqual([]);
      project.annotateChords(chords);
      const read = project.chordAnnotations();
      expect(read).toStrictEqual(chords);
      expect(read[0]).not.toHaveProperty('romanNumeral');
      expect(read[1]).not.toHaveProperty('romanNumeral');
      expect(read[2].romanNumeral).toBe('V');
      expect(project.chordAnnotations()[0].extensions).not.toBe(read[0].extensions);
    } finally {
      project.destroy();
    }
  });

  it('refuses undeclared key and chord annotation fields by indexed path', () => {
    const project = new Project();
    try {
      const keys = [
        { startPpq: 0, endPpq: 4, mode: 'major' },
        { startPpq: 4, endPpq: 8, mode: 'minor', unexpected: true },
      ] as never;
      const keyError = expectRefusalOf(() => project.annotateKeys(keys), TypeError);
      expect(keyError.message).toBe(
        'Project.annotateKeys: unknown key keys[1].unexpected (expected endPpq, mode, startPpq, tonicPc)',
      );

      const functionRow = Object.assign(() => undefined, {
        startPpq: 4,
        endPpq: 8,
        unexpected: true,
      });
      const functionKeys = [{ startPpq: 0, endPpq: 4 }, functionRow] as never;
      const functionError = expectRefusalOf(() => project.annotateKeys(functionKeys), TypeError);
      expect(functionError.message).toBe(
        'Project.annotateKeys: unknown key keys[1].unexpected (expected endPpq, mode, startPpq, tonicPc)',
      );

      const chords = [
        { startPpq: 0, endPpq: 4, quality: 'major' },
        { startPpq: 4, endPpq: 8, quality: 'minor', unexpected: true },
      ] as never;
      const chordError = expectRefusalOf(() => project.annotateChords(chords), TypeError);
      expect(chordError.message).toBe(
        'Project.annotateChords: unknown key chords[1].unexpected (expected endPpq, extensions, modulationBoundary, quality, romanNumeral, rootPc, slashBassPc, startPpq)',
      );
    } finally {
      project.destroy();
    }
  });

  it('refuses section reads after the project is destroyed', () => {
    const project = new Project();
    project.destroy();
    let caught: unknown;
    try {
      project.sectionAnnotations();
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect(caught).toMatchObject({ code: ErrorCode.InvalidState });
  });

  it('adds unresolved audio sources and relinks by stable id', () => {
    const project = new Project();
    try {
      const first = project.addAudioSource('same.wav');
      const second = project.addAudioSource('same.wav');
      expect(second).not.toBe(first);
      expect(project.unresolvedAudioSourceIds()).toEqual([first, second]);
      expect(project.relinkSource(first, 'replacement.wav')).toBe(first);
      expect(project.sources().find((source) => source.id === first)?.nameOrUri).toBe(
        'replacement.wav',
      );
      expect(project.unresolvedAudioSourceIds()).toEqual([first, second]);

      project.undo();
      expect(project.sources().find((source) => source.id === first)?.nameOrUri).toBe('same.wav');
      expect(expectRefusalOf(() => project.relinkSource(first, ''), RangeError).message).toContain(
        'uri',
      );
      expect(expectRefusalOf(() => project.addAudioSource(''), RangeError).message).toContain(
        'uri',
      );
      expect(expectRefusalOf(() => project.addAudioSource('\ud800'), RangeError).message).toContain(
        'uri',
      );
      expect(
        expectRefusalOf(() => project.relinkSource(999, 'missing.wav'), RangeError).message,
      ).toContain('sourceId');
    } finally {
      project.destroy();
    }
  });

  it('refuses to read MIDI events from an audio clip', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 1 });
      expect(expectRefusalOf(() => project.midiEvents(clipId), RangeError).message).toContain(
        'clipId',
      );
    } finally {
      project.destroy();
    }
  });

  it('reads, edits, clears, undoes, redoes, and serializes audio source metadata', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'metadata' });
      project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 1,
        sourceUri: 'asset://metadata.wav',
      });
      const sourceId = project.clips()[0].sourceId;

      expect(project.sources()[0]).toMatchObject({ id: sourceId });
      expect(project.sources()[0]).not.toHaveProperty('contentHash');
      expect(project.sources()[0]).not.toHaveProperty('externalStemRole');

      project.setAudioSourceMetadata({
        sourceId,
        contentHash: 'sha256:deadbeef',
        externalStemRole: 'lead',
      });
      expect(project.sources()[0]).toMatchObject({
        id: sourceId,
        nameOrUri: 'asset://metadata.wav',
        contentHash: 'sha256:deadbeef',
        externalStemRole: 'lead',
      });

      expect(() => project.setAudioSourceMetadata({ sourceId, contentHash: '' })).toThrow(
        /contentHash must not be empty/,
      );
      project.setAudioSourceMetadata({ sourceId });
      expect(project.sources()[0]).not.toHaveProperty('contentHash');
      expect(project.sources()[0]).not.toHaveProperty('externalStemRole');

      project.undo();
      expect(project.sources()[0]).toMatchObject({
        contentHash: 'sha256:deadbeef',
        externalStemRole: 'lead',
      });
      project.redo();
      expect(project.sources()[0]).not.toHaveProperty('contentHash');
      expect(project.sources()[0]).not.toHaveProperty('externalStemRole');

      project.setAudioSourceMetadata({
        sourceId,
        contentHash: 'sha256:roundtrip',
        externalStemRole: 'vocals',
      });
      const restored = Project.fromJson(project.toJson());
      try {
        expect(restored.sources()[0]).toMatchObject({
          id: sourceId,
          contentHash: 'sha256:roundtrip',
          externalStemRole: 'vocals',
        });
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('rejects audio source metadata edits for MIDI and unknown sources', () => {
    const project = new Project();
    try {
      const audioTrackId = project.addTrack({ kind: 'audio', name: 'audio' });
      project.addClip({
        trackId: audioTrackId,
        startPpq: 0,
        lengthPpq: 1,
        sourceUri: 'asset://audio.wav',
      });
      project.addMidiClip(0, 1);
      const audioSourceId = project.clips()[0].sourceId;
      const midiSourceId = project.clips()[1].sourceId;
      project.setAudioSourceMetadata({
        sourceId: audioSourceId,
        contentHash: 'sha256:valid',
        externalStemRole: 'lead',
      });

      const expectInvalidParameter = (action: () => void) => {
        let caught: unknown;
        try {
          action();
        } catch (error) {
          caught = error;
        }
        expectArgumentRefusal(caught, RangeError);
      };

      expectInvalidParameter(() =>
        project.setAudioSourceMetadata({
          sourceId: midiSourceId,
          contentHash: 'sha256:bad',
          externalStemRole: 'lead',
        }),
      );
      expectInvalidParameter(() =>
        project.setAudioSourceMetadata({
          sourceId: 999999,
          contentHash: 'sha256:bad',
          externalStemRole: 'lead',
        }),
      );
      expect(project.sources()[0]).toMatchObject({
        contentHash: 'sha256:valid',
        externalStemRole: 'lead',
      });
      expect(project.sources()[1]).not.toHaveProperty('contentHash');
      expect(project.sources()[1]).not.toHaveProperty('externalStemRole');
    } finally {
      project.destroy();
    }
  });

  it('creates projects and reads canonical assist sidecars in stable order', () => {
    const project = new Project();
    const constructed = new Project();
    try {
      expect(project.toJson()).toBe(constructed.toJson());

      project.setAssistSidecar({
        moduleId: 'project.module',
        payload: new Uint8Array([1, 2]),
      });
      project.setAssistSidecar({
        moduleId: 'region.module',
        schemaVersion: 3,
        targetTrackId: 7,
        regionStartPpq: 2,
        regionEndPpq: 8,
        payload: new Uint8Array([3, 4]),
      });

      expect(project.assistSidecars().length).toBe(2);
      expect(project.assistSidecars()).toEqual([
        project.assistSidecars()[0],
        project.assistSidecars()[1],
      ]);
      expect(project.assistSidecars()[0]).toMatchObject({
        moduleId: 'project.module',
        schemaVersion: 0,
        targetTrackId: 0,
        regionStartPpq: 0,
        regionEndPpq: 0,
      });
      expect(Array.from(project.assistSidecars()[0].payload)).toEqual([1, 2]);
      expect(project.assistSidecars()[1]).toMatchObject({
        moduleId: 'region.module',
        schemaVersion: 3,
        targetTrackId: 7,
        regionStartPpq: 2,
        regionEndPpq: 8,
      });
      expect(Array.from(project.assistSidecars()[1].payload)).toEqual([3, 4]);
    } finally {
      project.destroy();
      constructed.destroy();
    }
  });

  it('validates assist sidecar descriptors before calling embind', () => {
    const project = new Project();
    try {
      expect(() => project.setAssistSidecar(null as never)).toThrow(/request must be an object/);
      expect(() => project.setAssistSidecar({ moduleId: '' })).toThrow(/moduleId/);
      expect(() => project.setAssistSidecar({ moduleId: 'bad', schemaVersion: -1 })).toThrow(
        /schemaVersion/,
      );
      expect(() =>
        project.setAssistSidecar({ moduleId: 'bad', payload: new Uint8Array(0) }),
      ).not.toThrow();
      expect(() => project.setAssistSidecar({ moduleId: 'bad', payload: [1, 2] as never })).toThrow(
        /payload/,
      );
      expect(() =>
        (project as unknown as { setAssistSidecar: (...args: unknown[]) => void }).setAssistSidecar(
          'legacy',
          0,
          0,
          0,
          0,
        ),
      ).toThrow(/request must be an object/);
    } finally {
      project.destroy();
    }
  });

  it('rejects unknown track kinds at both the facade and embind boundaries', () => {
    const project = new Project();
    try {
      expect(() => project.addTrack({ kind: 'bus' as never })).toThrow(RangeError);
      expect(() => project.addTrack({ kind: 'bus' as never })).toThrow(/kind must be one of/);
      expect(() => project.addTrack({ kind: 3 as never })).toThrow(TypeError);
      expect(() => project.addTrack({ kind: 1.5 as never })).toThrow(TypeError);

      const native = (project as unknown as { native: { addTrack: (desc: object) => number } })
        .native;
      // One rejection each, named by what is wrong with it: the numeric path
      // narrows through the shared reader before the ordinal range is asked.
      expect(() => native.addTrack({ kind: 'bus' })).toThrow(/unknown project track kind/);
      expect(expectRefusalOf(() => native.addTrack({ kind: 3 })).message).toBe(
        'Project.addTrack: project track kind must be a name or an ordinal in [0, 3)',
      );
      expect(() => native.addTrack({ kind: 1.5 })).toThrow(/project track kind must be an integer/);
    } finally {
      project.destroy();
    }
  });

  function makeSysexSmf(): Uint8Array {
    const payload = [0x7e, 0x7f, 0x09, 0x01, 0xf7];
    const body = new Uint8Array([
      0x00,
      0xf0,
      payload.length,
      ...payload,
      0x00,
      0x90,
      0x3c,
      0x40,
      0x83,
      0x60,
      0x80,
      0x3c,
      0x00,
      0x00,
      0xff,
      0x2f,
      0x00,
    ]);
    const smf = new Uint8Array(22 + body.length);
    smf.set([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0], 0);
    smf.set([0x4d, 0x54, 0x72, 0x6b], 14);
    new DataView(smf.buffer).setUint32(18, body.length, false);
    smf.set(body, 22);
    return smf;
  }

  function makeTruncatedSmf(): Uint8Array {
    const body = new Uint8Array([
      0x00,
      0x90,
      0x3c,
      0x64, // valid note-on
      0x00,
      0xff,
      0x01,
      0x7f, // text meta claims 127 missing payload bytes
    ]);
    const smf = new Uint8Array(22 + body.length);
    smf.set([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0], 0);
    smf.set([0x4d, 0x54, 0x72, 0x6b], 14);
    new DataView(smf.buffer).setUint32(18, body.length, false);
    smf.set(body, 22);
    return smf;
  }

  /** A current document whose one clip names a source the document does not carry. */
  function danglingSourceJson(): string {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://dangling.wav' });
      const document = JSON.parse(project.toJson());
      document.clips[0].sourceId = 99;
      return JSON.stringify(document);
    } finally {
      project.destroy();
    }
  }

  it('reports the expected project ABI version', () => {
    expect(projectAbiVersion()).toBe(EXPECTED_PROJECT_ABI_VERSION);
    expect(projectAbiVersion()).toBeGreaterThan(0);
  });

  it('imports external planar stems through the project C ABI', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const result = project.importExternalStems({
      sampleRate: 48000,
      stems: [
        {
          name: 'vocals',
          role: 'lead',
          layout: 'stereo',
          channels: [new Float32Array([1, 0, 0]), new Float32Array([-0.5, 0, 0])],
          startFrame: 113,
        },
        {
          name: 'drums',
          layout: 'mono',
          channels: [new Float32Array([0.25, 0, 0])],
        },
      ],
    });
    expect(result.trackIds).toHaveLength(2);
    expect(result.clipIds).toHaveLength(2);
    expect(project.tracks().length).toBe(2);
    expect(project.clips().length).toBe(2);
    expect(() =>
      project.importExternalStems({
        sampleRate: 48000,
        stems: [
          { name: 'duplicate', layout: 'mono', channels: [new Float32Array([0])] },
          { name: 'duplicate', layout: 'mono', channels: [new Float32Array([0])] },
        ],
      }),
    ).toThrow();
    expect(project.tracks().length).toBe(2);
    expect(project.clips().length).toBe(2);
    project.destroy();
  });

  it('round-trips toJson -> fromJson -> toJson byte-for-byte', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const json = project.toJson();

      const restored = Project.fromJson(json);
      try {
        const restoredJson = restored.toJson();
        // Exact byte equality (not just structural) — the serializer is
        // deterministic and the round-trip must reproduce identical bytes.
        expect(restoredJson).toBe(json);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('round-trips a non-trivial project and undo restores serialized bytes', () => {
    const project = buildProject();
    try {
      const before = project.toJson();
      const extra = project.addTrack({ kind: 'midi', name: 'extra' });
      expect(extra).toBeGreaterThan(0);
      expect(project.toJson()).not.toBe(before);
      project.undo();
      expect(project.toJson()).toBe(before);
      const restored = Project.fromJson(before);
      try {
        expect(restored.toJson()).toBe(before);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('addMidiClip undoes and redoes track, source, and clip as one edit', () => {
    const project = new Project();
    try {
      const before = project.toJson();
      project.addMidiClip(2, 4);
      const added = project.toJson();
      expect(project.tracks().length).toBe(1);
      expect(project.sources().length).toBe(1);
      expect(project.clips().length).toBe(1);

      project.undo();
      expect(project.toJson()).toBe(before);
      expect(project.tracks().length).toBe(0);
      expect(project.sources().length).toBe(0);
      expect(project.clips().length).toBe(0);

      project.redo();
      expect(project.toJson()).toBe(added);
      expect(project.tracks().length).toBe(1);
      expect(project.sources().length).toBe(1);
      expect(project.clips().length).toBe(1);
    } finally {
      project.destroy();
    }
  });

  it('routes a track to a MIDI destination and undoes it', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'midi', name: 'lead' });
      const before = project.toJson();
      project.setTrackMidiDestination(trackId, 7);
      const after = project.toJson();
      expect(after).not.toBe(before);
      expect(after).toContain('"midiDestinationId":7');
      project.undo();
      expect(project.toJson()).toBe(before);
    } finally {
      project.destroy();
    }
  });

  it('sets a clip warp reference and undoes it', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
      const before = project.toJson();

      // A warp ref must name a registered warp map.
      expect(() => project.setClipWarpRef(clipId, 123)).toThrow();

      project.setWarpMap({
        id: 123,
        anchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 4, sourceSample: 4 },
        ],
      });
      project.setClipWarpRef(clipId, 123);
      project.setClipWarpMode(clipId, 'repitch');
      const after = project.toJson();
      expect(after).not.toBe(before);
      expect(after).toContain('"warpRefId":123');
      expect(after).toContain('"warpMode":"repitch"');
      const restored = Project.fromJson(after);
      expect(restored.toJson()).toBe(after);
      restored.destroy();
      project.undo();
      expect(project.toJson()).toContain('"warpMode":"off"');
      project.undo();
      expect(project.toJson()).toContain('"warpRefId":0');
      project.undo();
      expect(project.toJson()).toBe(before);
      project.setClipWarpMode(clipId, 'tempoSync');
      expect(project.toJson()).toContain('"warpMode":"tempoSync"');
      expect(() => project.setClipWarpMode(clipId, 'typo' as 'repitch')).toThrow();
      expect(() => project.setClipWarpMode(clipId, 99 as never)).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('refuses a warp mode ordinal or a retired spelling in a document by its tokens', () => {
    const project = new Project();
    let document: string;
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
      document = project.toJson();
    } finally {
      project.destroy();
    }
    expect(document).toContain('"warpMode":"off"');
    for (const stale of ['0', '2', '"tempo-sync"']) {
      expect(() =>
        Project.fromJson(document.replace('"warpMode":"off"', `"warpMode":${stale}`)),
      ).toThrow(
        expect.objectContaining({
          code: ErrorCode.InvalidFormat,
          message: expect.stringMatching(
            /warpMode must be one of off, repitch, tempoSync, timeStretch$/,
          ),
        }),
      );
    }
  });

  it('clears clip warp references when removing a warp map and restores them on undo', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
      project.setWarpMap({
        id: 123,
        anchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 4, sourceSample: 4 },
        ],
      });
      project.setClipWarpRef(clipId, 123);
      const mapped = project.toJson();

      project.removeWarpMap(123);
      expect(project.toJson()).toContain('"warpRefId":0');
      project.undo();
      expect(project.toJson()).toBe(mapped);
      expect(() =>
        project.setWarpMap({ id: 124, anchors: [{ warpSample: 0, sourceSample: 0 }] }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('requires chord endpoints and names invalid nested fields', () => {
    const project = new Project();
    try {
      const chord = { startPpq: 0, endPpq: 4, extensions: [7] } as const;
      project.annotateChords([chord] as const);
      expect(() => project.annotateChords([{ endPpq: 4 } as never])).toThrow(
        new TypeError('Project.annotateChords: startPpq must be a number'),
      );
      expect(() => project.annotateChords([{ startPpq: 0 } as never])).toThrow(
        new TypeError('Project.annotateChords: endPpq must be a number'),
      );
      expect(() => project.annotateChords([chord, { ...chord, extensions: [7, 256] }])).toThrow(
        new RangeError(
          'Project.annotateChords: chords[1].extensions[1] must be an integer in [0, 255]',
        ),
      );
      expect(() => project.annotateChords([chord, { ...chord, romanNumeral: 'I\0V' }])).toThrow(
        new RangeError('Project.annotateChords: chords[1].romanNumeral must not contain NUL'),
      );
    } finally {
      project.destroy();
    }
  });

  it('compiles an empty project into a renderable timeline', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const result = project.compile();
      expect(result).not.toHaveProperty('hasTimeline');
      expect(result).not.toHaveProperty('diagnosticCount');
      expect(result).not.toHaveProperty('messages');
      expect(Array.isArray(result.diagnostics)).toBe(true);
      expect(result.diagnostics).toHaveLength(0);
      const { timeline } = project.compile({ withTimeline: true });
      try {
        expect(timeline).not.toBeNull();
      } finally {
        timeline?.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('surfaces compile diagnostic messages per diagnostic', () => {
    const project = buildProject();
    try {
      const result = project.compile();
      expect(result.diagnostics.length).toBe(1);
      expect(result.diagnostics[0].code).toBe('midiClipNoInstrument');
      expect(result.diagnostics[0].severity).toBe('warning');
      expect(result.diagnostics[0].message).toContain('project contains MIDI clips');
    } finally {
      project.destroy();
    }
  });

  it('bounces deterministically (same-build repeatability)', () => {
    const project = buildProject();
    try {
      project.setSampleRate(48000);
      // An explicit totalFrames pins the render length; the project bounces to
      // silence here (MIDI tracks have no instrument bound), which is enough to
      // assert same-build repeatability. We compare within a small tolerance
      // rather than requiring cross-platform bit-exactness.
      const options = { totalFrames: 256, blockSize: 128, numChannels: 2 };

      const first = interleaved(project.bounce(options));
      const second = interleaved(project.bounce(options));

      expect(first.length).toBe(256 * 2);
      expect(second.length).toBe(first.length);
      for (let i = 0; i < first.length; i++) {
        expect(second[i]).toBeCloseTo(first[i], 6);
      }
    } finally {
      project.destroy();
    }
  });

  it('refuses an out-of-domain bounce option as a RangeError on every project bounce mapper', () => {
    const project = buildProject();
    const expectInvalidParameter = (call: () => unknown): void => {
      let caught: unknown;
      try {
        call();
      } catch (error) {
        caught = error;
      }
      expectArgumentRefusal(caught, RangeError);
    };

    try {
      const invalidOptions = { totalFrames: 128, blockSize: -1 };
      expectInvalidParameter(() => project.bounce(invalidOptions));
      expectInvalidParameter(() =>
        project.bounce({ ...invalidOptions, builtinInstruments: [{ instrument: {} }] }),
      );
      expectInvalidParameter(() =>
        project.bounce({ ...invalidOptions, synthInstruments: [{ instrument: {} }] }),
      );
      expectInvalidParameter(() =>
        project.bounce({ ...invalidOptions, sf2Instruments: [{ instrument: {} }] }),
      );
    } finally {
      project.destroy();
    }
  });

  function buildMidiOnlyProject(): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    // A short held note so the built-in synth produces sustained tone.
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 3, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    return project;
  }

  function buildGmProgramProject(program: number): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 1);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      midiProgram({ ppq: 0, group: 0, channel: 0, program }),
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 0.5, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    return project;
  }

  function maxAbs(buffer: Float32Array): number {
    let peak = 0;
    for (let i = 0; i < buffer.length; i++) {
      const v = Math.abs(buffer[i]);
      if (v > peak) {
        peak = v;
      }
    }
    return peak;
  }

  function maxAbsDifference(left: Float32Array, right: Float32Array): number {
    expect(left).toHaveLength(right.length);
    let peak = 0;
    for (let i = 0; i < left.length; i++) {
      peak = Math.max(peak, Math.abs(left[i] - right[i]));
    }
    return peak;
  }

  it('refuses sub-floor tempo segments before storing them', () => {
    const project = new Project();
    try {
      const valid = [{ startPpq: 0, bpm: 120 }];
      project.setTempoSegments(valid);
      const before = project.tempoSegments();
      expect(() => project.setTempoSegments([{ startPpq: 0, bpm: 1e-8 }])).toThrow(
        new RangeError(
          'Project.setTempoSegments: segments[0].bpm must be finite and in [1e-6, 100000]',
        ),
      );
      expect(project.tempoSegments()).toEqual(before);
      project.setTempoSegments([{ startPpq: 0, bpm: 1e-6, endBpm: 0 }]);
      expect(project.tempoSegments()).toEqual([{ startPpq: 0, bpm: 1e-6, endBpm: 0 }]);
    } finally {
      project.destroy();
    }
  });

  it('counts and renders a pre-roll prefix at the initial tempo and meter', () => {
    const project = buildMidiOnlyProject();
    try {
      project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
      project.setTimeSignatures([{ startPpq: 0, numerator: 4, denominator: 4 }]);
      const countInFrames = project.countInFrames(1);
      expect(countInFrames).toBe(96000);
      expect(project.countInFrames(0)).toBe(0);

      const instrument = [{ destinationId: 0, instrument: { waveform: 'sine' as const } }];
      const base = interleaved(
        project.bounce({ totalFrames: 12000, builtinInstruments: instrument }),
      );
      const noClick = project.bounce({
        totalFrames: 108000,
        countInBars: 1,
        builtinInstruments: instrument,
      });
      const disabledClick = project.bounce({
        totalFrames: 108000,
        countInBars: 1,
        metronome: { enabled: false },
        builtinInstruments: instrument,
      });
      const withClick = project.bounce({
        totalFrames: 108000,
        countInBars: 1,
        metronome: { enabled: true },
        builtinInstruments: instrument,
      });

      const noClickSamples = interleaved(noClick);
      expect(interleaved(disabledClick)).toEqual(noClickSamples);
      expect(noClickSamples.slice(0, countInFrames * 2).every((sample) => sample === 0)).toBe(true);
      expect(noClickSamples.slice(countInFrames * 2)).toEqual(base);
      expect(maxAbs(interleaved(withClick).slice(0, countInFrames * 2))).toBeGreaterThan(0);

      const noCountIn = project.bounce({
        totalFrames: 12000,
        builtinInstruments: instrument,
      });
      const disabledNoCountIn = project.bounce({
        totalFrames: 12000,
        countInBars: 0,
        metronome: { enabled: false },
        builtinInstruments: instrument,
      });
      expect(interleaved(disabledNoCountIn)).toEqual(interleaved(noCountIn));

      expect(expectRefusalOf(() => project.countInFrames(-1), RangeError).message).toContain(
        'countInBars',
      );
      expect(
        expectRefusalOf(() => project.bounce({ totalFrames: 128, countInBars: 17 }), RangeError)
          .message,
      ).toContain('countInBars');
      expect(
        expectRefusalOf(
          () => project.bounce({ totalFrames: 128, metronome: { enabled: false, beatGain: -1 } }),
          RangeError,
        ).message,
      ).toContain('metronome.beatGain');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              metronome: { enabled: false, clickSamples: 0 },
            }),
          RangeError,
        ).message,
      ).toContain('metronome.clickSamples');
      expect(
        expectRefusalOf(
          () => project.bounce({ totalFrames: 128, metronome: { enabled: false, clickSec: 2 } }),
          RangeError,
        ).message,
      ).toContain('metronome.clickSec');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              metronome: { enabled: false, clickSamples: 64, clickSec: 0.01 },
            }),
          RangeError,
        ).message,
      ).toContain('metronome.clickSamples or metronome.clickSec');
    } finally {
      project.destroy();
    }
  });

  it('reports an unsafe count-in frame result as InvalidFormat', () => {
    const project = new Project();
    try {
      for (const bad of [0.5, 17, Number.MAX_SAFE_INTEGER]) {
        expect(() => project.countInFrames(bad)).toThrow(
          new RangeError('Project.countInFrames: countInBars must be an integer in [0, 16]'),
        );
      }
      project.setSampleRate(384000);
      project.setTimeSignatures([{ startPpq: 0, numerator: 32, denominator: 4 }]);
      project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
      expect(project.countInFrames(16)).toBe(98_304_000);

      project.setTempoSegments([{ startPpq: 0, bpm: 1e-6 }]);
      let caught: unknown;
      try {
        project.countInFrames(16);
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({
        code: ErrorCode.InvalidFormat,
        message: 'Project.countInFrames: frames is not exactly representable as a number',
      });
    } finally {
      project.destroy();
    }
  });

  it('applies bounce controller profiles and articulations to a hosted synth', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const { clipId } = project.addMidiClip(0, 2);
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiCc({ ppq: 0.125, group: 0, channel: 0, controller: 2, value: 127 }),
        midiNoteOn({ ppq: 0.25, group: 0, channel: 0, note: 69, velocity: 100 }),
        midiCc({ ppq: 0.375, group: 0, channel: 0, controller: 2, value: 32 }),
        midiNoteOff({ ppq: 0.75, group: 0, channel: 0, note: 60, velocity: 0 }),
        midiNoteOff({ ppq: 0.875, group: 0, channel: 0, note: 69, velocity: 0 }),
      ]);
      expect(controllerProfileNames()).toContain('breath');
      const synthInstruments = [{ destinationId: 0, instrument: { engineMode: 'reed' } as never }];
      const plain = interleaved(project.bounce({ totalFrames: 48000, synthInstruments }));
      const withProfile = interleaved(
        project.bounce({
          totalFrames: 48000,
          synthInstruments,
          controllerProfiles: [{ destinationId: 0, presetName: 'breath' }],
        }),
      );
      const withArticulation = interleaved(
        project.bounce({
          totalFrames: 48000,
          synthInstruments,
          controllerProfiles: [{ destinationId: 0, presetName: 'breath' }],
          articulations: [{ destinationId: 0, channel: 0, articulation: 'monoLegato' }],
        }),
      );
      expect(maxAbsDifference(plain, withProfile)).toBeGreaterThan(1e-3);
      expect(maxAbsDifference(withProfile, withArticulation)).toBeGreaterThan(1e-3);

      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              synthInstruments,
              controllerProfiles: [
                { destinationId: 0, presetName: 'breath' },
                { destinationId: 0, presetName: 'breath' },
              ],
            }),
          RangeError,
        ).message,
      ).toContain('controllerProfiles[1].destinationId');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              synthInstruments,
              articulations: [
                { destinationId: 0, channel: 0, articulation: 'poly' },
                { destinationId: 0, channel: 0, articulation: 'poly' },
              ],
            }),
          RangeError,
        ).message,
      ).toContain('articulations[1]');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              controllerProfiles: [{ destinationId: 0, presetName: 'not-a-profile' }],
            }),
          RangeError,
        ).message,
      ).toContain('controllerProfiles[0].presetName');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              articulations: [{ destinationId: 99, channel: 16, articulation: 'poly' }],
            }),
          RangeError,
        ).message,
      ).toContain('articulations[0].channel');
      expect(
        expectRefusalOf(
          () =>
            project.bounce({
              totalFrames: 128,
              synthInstruments,
              controllerProfiles: [{ destinationId: 99, presetName: 'breath' }],
            }),
          RangeError,
        ).message,
      ).toContain('controllerProfiles[0].destinationId');
    } finally {
      project.destroy();
    }
  });

  it('bounces at the width of the scene master when numChannels is omitted', () => {
    const cases: ReadonlyArray<[ChannelLayout | undefined, number]> = [
      [undefined, 2],
      ['mono', 1],
      ['stereo', 2],
      ['5.1', 6],
      ['7.1', 8],
    ];
    for (const [layout, width] of cases) {
      const project = new Project();
      project.setSampleRate(48000);
      if (layout !== undefined) {
        project.setMixerScene({ version: 2, buses: [{ id: 'master', role: 'master', layout }] });
      }
      project.addMidiClip(0, 1);
      const audio = project.bounce({ totalFrames: 128 });
      expect(audio.channels, layout ?? 'none').toHaveLength(width);
      for (const plane of audio.channels) {
        expect(plane).toHaveLength(128);
      }
      project.destroy();
    }
  });

  it('bounces a MIDI-only project through the built-in instrument to non-silent audio', () => {
    const project = buildMidiOnlyProject();
    try {
      // Without an instrument the same MIDI project bounces to silence.
      const silent = interleaved(project.bounce({ totalFrames: 48000, numChannels: 2 }));
      expect(maxAbs(silent)).toBe(0);

      // Routing the built-in synth makes it audible.
      const audible = interleaved(
        project.bounce({
          totalFrames: 48000,
          numChannels: 2,
          builtinInstruments: [{ instrument: { waveform: 'saw', gain: 0.5 } }],
        }),
      );
      expect(audible.length).toBe(48000 * 2);
      expect(maxAbs(audible)).toBeGreaterThan(0.01);
    } finally {
      project.destroy();
    }
  });

  it('defines omitted, undefined, empty, and null instrument binding semantics', () => {
    const project = buildMidiOnlyProject();
    const options = { totalFrames: 4096, numChannels: 1 };
    const pair = { instrument: {} };
    try {
      // No binding, undefined, an empty list and null all bind nothing: silence.
      const omitted = interleaved(project.bounce());
      const explicitUndefined = interleaved(
        project.bounce({ ...options, builtinInstruments: undefined }),
      );
      const empty = interleaved(project.bounce({ ...options, builtinInstruments: [] }));
      const runtimeNull = interleaved(
        project.bounce({ ...options, builtinInstruments: null as never }),
      );
      expect(maxAbs(omitted)).toBe(0);
      expect(maxAbs(explicitUndefined)).toBe(0);
      expect(maxAbs(empty)).toBe(0);
      expect(maxAbs(runtimeNull)).toBe(0);
      // A single pair outside a list is refused, not read as a one-element list.
      expect(() => project.bounce({ ...options, builtinInstruments: pair as never })).toThrow(
        TypeError,
      );
      // An empty-config pair in a list binds the default instrument.
      expect(
        maxAbs(interleaved(project.bounce({ ...options, builtinInstruments: [pair] }))),
      ).toBeGreaterThan(0.01);

      expect(maxAbs(interleaved(project.bounce({ ...options, synthInstruments: undefined })))).toBe(
        0,
      );
      expect(maxAbs(interleaved(project.bounce({ ...options, synthInstruments: [] })))).toBe(0);
      expect(
        maxAbs(interleaved(project.bounce({ ...options, synthInstruments: [pair] }))),
      ).toBeGreaterThan(0.01);
      expect(maxAbs(interleaved(project.bounce({ ...options, sf2Instruments: undefined })))).toBe(
        0,
      );
      expect(maxAbs(interleaved(project.bounce({ ...options, sf2Instruments: [] })))).toBe(0);
      expect(
        maxAbs(interleaved(project.bounce({ ...options, sf2Instruments: [pair] }))),
      ).toBeGreaterThan(0.01);
    } finally {
      project.destroy();
    }
  });

  it('treats an omitted synth patch field as keep and an explicit zero as the value zero', () => {
    const project = buildMidiOnlyProject();
    try {
      // The patch carries a present-field bitmask, so it can tell "unset" from
      // "set to zero" and does not need the zero-means-default convention the
      // bitmask-less config structs use. The render window has to be long
      // enough for the note to reach its sustain stage; a few thousand frames
      // is still inside attack/decay, where no sustain value can matter and
      // any comparison would agree for the wrong reason.
      const options = { totalFrames: 48000, numChannels: 2 };
      const tailEnergy = (audio: Float32Array): number => {
        let sum = 0;
        for (let i = Math.floor(audio.length * 0.75); i < audio.length; i++) {
          sum += audio[i] * audio[i];
        }
        return sum;
      };

      // A short envelope pinned in all three renders keeps the tail window past the
      // decay stage however the preset's own envelope is voiced.
      const envelope = { preset: 'warmPad', ampAttackMs: 20, ampDecayMs: 150 };
      const base = interleaved(
        project.bounce({ ...options, synthInstruments: [{ instrument: envelope }] }),
      );
      const explicitZero = interleaved(
        project.bounce({
          ...options,
          synthInstruments: [{ instrument: { ...envelope, ampSustain: 0 } }],
        }),
      );
      const explicitLow = interleaved(
        project.bounce({
          ...options,
          synthInstruments: [{ instrument: { ...envelope, ampSustain: 0.2 } }],
        }),
      );

      expect(maxAbs(base)).toBeGreaterThan(0.01);
      // An explicit zero applies zero instead of falling back to the preset,
      // so the note decays away and the sustained tail collapses. Comparing
      // tail energy rather than peak level is what makes this discriminating:
      // the attack transient is identical in all three renders.
      expect(explicitZero).not.toEqual(base);
      expect(tailEnergy(explicitZero)).toBeLessThan(tailEnergy(base) * 0.5);
      // A non-zero override is neither the preset value nor the zero case, so
      // the field is carried through as a value rather than as a flag.
      expect(explicitLow).not.toEqual(base);
      expect(explicitLow).not.toEqual(explicitZero);
      expect(tailEnergy(explicitLow)).toBeGreaterThan(tailEnergy(explicitZero));
    } finally {
      project.destroy();
    }
  });

  it('bounce with builtinInstruments auto-derives length when totalFrames is omitted', () => {
    const project = buildMidiOnlyProject();
    try {
      // No totalFrames: the C ABI auto-derives the render length from the
      // arrangement (musical end + synth release tail).
      const audio = interleaved(
        project.bounce({ numChannels: 2, builtinInstruments: [{ instrument: {} }] }),
      );
      expect(audio.length).toBeGreaterThan(0);
      expect(maxAbs(audio)).toBeGreaterThan(0.01);
    } finally {
      project.destroy();
    }
  });

  it('bounce auto-derives length for a MIDI project when totalFrames is omitted', () => {
    const project = buildMidiOnlyProject();
    try {
      // The README WASM quick-start shape: bounce({ numChannels }) with no
      // totalFrames must now produce a buffer (auto-derived length), not error.
      const audio = interleaved(project.bounce({ numChannels: 2 }));
      expect(audio.length).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('bounce with builtinInstruments accepts a default patch and per-destination array', () => {
    const project = buildMidiOnlyProject();
    try {
      // Default ({}) sine patch.
      const def = interleaved(
        project.bounce({
          totalFrames: 24000,
          numChannels: 1,
          builtinInstruments: [{ instrument: {} }],
        }),
      );
      expect(def.length).toBe(24000);
      expect(maxAbs(def)).toBeGreaterThan(0.01);

      // Array of bindings (single destination 0 here).
      const arr = interleaved(
        project.bounce({
          totalFrames: 24000,
          numChannels: 1,
          builtinInstruments: [{ destinationId: 0, instrument: { waveform: 'square' } }],
        }),
      );
      expect(arr.length).toBe(24000);
      expect(maxAbs(arr)).toBeGreaterThan(0.01);
    } finally {
      project.destroy();
    }
  });

  it('forwards useGmPrograms to NativeSynth while preserving fixed-patch defaults', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiProgram({ ppq: 0, group: 0, channel: 0, program: 4 }), // Electric Piano 1 in the GM map.
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    const options = { totalFrames: 24000, numChannels: 1 };
    try {
      const omitted = interleaved(
        project.bounce({ ...options, synthInstruments: [{ instrument: { preset: 'sine' } }] }),
      );
      const fixed = interleaved(
        project.bounce({
          ...options,
          synthInstruments: [{ instrument: { preset: 'sine', useGmPrograms: false } }],
        }),
      );
      const gm = interleaved(
        project.bounce({
          ...options,
          synthInstruments: [{ instrument: { preset: 'sine', useGmPrograms: true } }],
        }),
      );

      expect(fixed).toEqual(omitted);
      expect(gm).not.toEqual(fixed);
      expect(maxAbs(gm)).toBeGreaterThan(0.01);

      for (const invalid of [1, 'true']) {
        let caught: unknown;
        try {
          project.bounce({
            ...options,
            synthInstruments: [{ instrument: { preset: 'sine', useGmPrograms: invalid as never } }],
          });
        } catch (error) {
          caught = error;
        }
        expectArgumentRefusal(caught, TypeError);
      }
    } finally {
      project.destroy();
    }
  });

  it('renders GM programs 4 and 40 as finite, audible, distinct outputs', () => {
    const render = (program: number, useGmPrograms: boolean): Float32Array => {
      const project = buildGmProgramProject(program);
      try {
        const audio = interleaved(
          project.bounce({
            totalFrames: 12000,
            blockSize: 128,
            numChannels: 1,
            synthInstruments: [{ instrument: { preset: 'sine', useGmPrograms } }],
          }),
        );
        expect(audio.length).toBe(12000);
        for (const sample of audio) {
          expect(Number.isFinite(sample)).toBe(true);
        }
        expect(maxAbs(audio)).toBeGreaterThan(0);
        return audio;
      } finally {
        project.destroy();
      }
    };

    const disabled = render(4, false);
    const gm4 = render(4, true);
    const gm40 = render(40, true);
    const maxDelta = (lhs: Float32Array, rhs: Float32Array): number => {
      expect(lhs.length).toBe(rhs.length);
      let delta = 0;
      for (let i = 0; i < lhs.length; i++) {
        delta = Math.max(delta, Math.abs(lhs[i] - rhs[i]));
      }
      return delta;
    };
    expect(maxDelta(disabled, gm4)).toBeGreaterThan(1e-6);
    expect(maxDelta(gm4, gm40)).toBeGreaterThan(1e-6);
  });

  it('exports MIDI, applies program/MIDI FX, and preserves imported SysEx payloads', () => {
    const project = buildProject();
    try {
      const { clipId } = project.addMidiClip(0, 2);
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0.1, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiPolyPressure({ ppq: 0.2, group: 0, channel: 0, note: 60, pressure: 70 }),
        midiChannelPressure({ ppq: 0.3, group: 0, channel: 0, pressure: 80 }),
        midiPitchBend({ ppq: 0.4, group: 0, channel: 0, bend: 8192 }),
        midiNoteOff({ ppq: 1.1, group: 0, channel: 0, note: 60, velocity: 0 }),
      ]);
      project.bakeMidiFx({
        clipId,
        config: { transposeSemitones: 12, quantizePpq: 0.25, quantizeStrength: 1 },
      });
      project.setProgram(clipId, 42);
      project.setProgram(clipId, 24, { channel: 3, bank: { msb: 2, lsb: 0x23 } });
      const smf = project.exportSmf();
      expect(smf[0]).toBe(0x4d);
      // The export goes straight into a Blob, which is how a browser consumer
      // offers it as a download. A Uint8Array over an unspecified buffer is not
      // a BlobPart, so this assignment is what stops the declared return type
      // losing its buffer parameter and sending every caller through a copy.
      const downloadable: BlobPart = smf;
      expect(new Blob([downloadable]).size).toBe(smf.byteLength);
      const bytes = Array.from(smf).join(',');
      expect(bytes).toContain('192,42');
      expect(bytes).toContain('195,24');
      // Pressure and bend survive the export. Poly pressure carries the
      // transposed note (60 + 12), because aftertouch left on the source note
      // would address a note the shifted voice never played.
      expect(bytes).toContain('160,72,70');
      expect(bytes).not.toContain('160,60,70');
      expect(bytes).toContain('208,80');
      expect(bytes).toContain('224,0,64');
    } finally {
      project.destroy();
    }

    const sysexProject = new Project();
    try {
      const firstClip = sysexProject.importSmf(makeSysexSmf());
      expect(firstClip).toBeGreaterThan(0);
      const json = sysexProject.toJson();
      expect(json).toContain('__sysexPayloads');
      const restored = Project.fromJson(json);
      try {
        expect(Array.from(restored.exportSmf()).join(',')).toContain('240,5,126,127,9,1,247');
      } finally {
        restored.destroy();
      }
    } finally {
      sysexProject.destroy();
    }
  });

  it('setProgram takes bank, group and channel as options', () => {
    const exportBytes = (apply: (project: Project, clipId: number) => void): string => {
      const project = buildProject();
      try {
        const { clipId } = project.addMidiClip(0, 2);
        apply(project, clipId);
        return Array.from(project.exportSmf()).join(',');
      } finally {
        project.destroy();
      }
    };
    const plain = exportBytes((p, id) => p.setProgram(id, 42));
    expect(plain).toContain('192,42');
    // Without a bank option no Bank Select controller (CC 0 / CC 32) precedes the program.
    expect(plain).not.toContain('176,0,');
    const banked = exportBytes((p, id) => p.setProgram(id, 42, { bank: { msb: 2, lsb: 0x23 } }));
    expect(banked).toContain('192,42');
    expect(banked.length).toBeGreaterThan(plain.length);
    expect(banked).toContain('176,0,2');
    const routed = exportBytes((p, id) => p.setProgram(id, 24, { group: 1, channel: 3 }));
    expect(routed).toContain('195,24');
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 2);
      // @ts-expect-error a bare bank number is no longer accepted
      expect(() => project.setProgram(clipId, 24, 5)).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('reports MIDI-FX provenance and matches the plain bake', () => {
    const seed = (project: Project, clip: number) => {
      project.setMidiEvents(clip, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
        midiNoteOn({ ppq: 2, group: 0, channel: 0, note: 64, velocity: 100 }),
        midiNoteOff({ ppq: 3, group: 0, channel: 0, note: 64, velocity: 0 }),
      ]);
    };
    const config = { chordIntervals: [0, 4, 7] };

    const request = new Project();
    const positional = new Project();
    const bare = new Project();
    try {
      const { clipId: requestClip } = request.addMidiClip(0, 4);
      seed(request, requestClip);
      expect(request.previewMidiFxCount({ clipId: requestClip, config })).toBe(12);
      const result = request.bakeMidiFx({
        clipId: requestClip,
        config,
        withSourceIndex: true,
      });
      expect(result.sourceIndex).toBeInstanceOf(Int32Array);
      expect(result.sourceIndex).toHaveLength(12);
      const perInput = [0, 0, 0, 0];
      for (const source of result.sourceIndex ?? []) {
        expect(source).toBeGreaterThanOrEqual(0);
        expect(source).toBeLessThan(4);
        perInput[source] += 1;
      }
      expect(perInput).toEqual([3, 3, 3, 3]);

      // The bake with provenance must land on the same bytes as the plain
      // bake, and the positional form is refused.
      const { clipId: positionalClip } = positional.addMidiClip(0, 4);
      seed(positional, positionalClip);
      positional.bakeMidiFx({ clipId: positionalClip, config });
      expect(() =>
        (positional as never as Record<string, (...a: unknown[]) => unknown>).bakeMidiFx(
          positionalClip,
          config,
        ),
      ).toThrow(TypeError);
      expect(Array.from(request.exportSmf())).toEqual(Array.from(positional.exportSmf()));

      const { clipId: bareClip } = bare.addMidiClip(0, 4);
      seed(bare, bareClip);
      expect(bare.bakeMidiFx({ clipId: bareClip, config })).toEqual({});
      expect(Array.from(bare.exportSmf())).toEqual(Array.from(positional.exportSmf()));

      // Errors travel the same path in both forms.
      expect(() =>
        positional.bakeMidiFx({ clipId: positionalClip, config: { quantizePpq: 0 } }),
      ).toThrow();
      expect(() =>
        request.bakeMidiFx({
          clipId: requestClip,
          config: { quantizePpq: 0 },
          withSourceIndex: true,
        }),
      ).toThrow();
      expect(() =>
        request.previewMidiFxCount({ clipId: requestClip, config: { quantizePpq: 0 } }),
      ).toThrow();
    } finally {
      request.destroy();
      positional.destroy();
      bare.destroy();
    }
  });

  it('refuses a non-object MIDI-FX config and leaves its content to the core', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      const bake = (config: unknown) => () =>
        project.bakeMidiFx({ clipId, config: config as never });
      const entry = 'Project.bakeMidiFx: ';
      const refused = (config: unknown, text: string) => {
        expect(expectRefusalOf(bake(config), TypeError).message).toBe(`${entry}${text}`);
      };
      refused('{}', 'config must be an object');
      refused(null, 'config must be an object');
      const typeRefusals: [unknown, string][] = [
        [{ transposeSemitones: '12' }, 'config.transposeSemitones must be a number'],
        [{ chordIntervals: [0, '4'] }, 'config.chordIntervals[1] must be a number'],
        [{ seed: Number.NaN }, 'config.seed must be a number'],
      ];
      for (const [config, text] of typeRefusals) {
        refused(config, text);
      }
      for (const key of ['transpose_semitones', 'nope']) {
        expect(expectRefusalOf(bake({ [key]: 1 }), TypeError).message).toMatch(
          new RegExp(`^${unknownKeyText(`${entry}unknown key config.${key}`).source}$`),
        );
      }
      expect(
        expectRefusalOf(() => project.previewMidiFxCount({ clipId, config: 1 as never }), TypeError)
          .message,
      ).toBe('Project.previewMidiFxCount: config must be an object');
    } finally {
      project.destroy();
    }
  });

  it('preserves more than 512 events when baking MIDI FX', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 8);
      const events = Array.from({ length: 600 }, (_, i) =>
        midiCc({ ppq: i / 80, group: 0, channel: 0, controller: i % 128, value: (i * 3) % 128 }),
      );
      project.setMidiEvents(clipId, events);
      project.bakeMidiFx({ clipId, config: {} });

      const payload = JSON.parse(project.toJson()) as {
        midiContent: Record<string, Array<{ ppq: number; word0: number; word1: number }>>;
      };
      expect(payload.midiContent[String(clipId)]).toEqual(events);

      const roundtrip = new Project();
      try {
        const roundClip = roundtrip.importSmf(project.exportSmf());
        roundtrip.bakeMidiFx({ clipId: roundClip, config: {} });
        const roundPayload = JSON.parse(roundtrip.toJson()) as {
          midiContent: Record<string, Array<{ ppq: number; word0: number; word1: number }>>;
        };
        expect(roundPayload.midiContent[String(roundClip)]).toHaveLength(600);
      } finally {
        roundtrip.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('imports the valid prefix of a truncated SMF', () => {
    const project = new Project();
    try {
      expect(project.importSmf(makeTruncatedSmf())).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('rejects MIDI loop export that exceeds the expansion budget', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      ]);
      project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 1e-9 });
      expect(() => project.exportSmf()).toThrow();
      expect(() => project.exportClipFile()).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('round-trips a MIDI 2.0 Clip File through the binding', () => {
    const project = buildProject();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      // A MIDI 2.0 note-on (message type 0x4) with a full 16-bit velocity.
      project.setMidiEvents(clipId, [
        { ppq: 0, word0: 0x40903c00, word1: 0xbeef0000 },
        { ppq: 1, word0: 0x40803c00, word1: 0 },
      ]);
      const clipFile = project.exportClipFile();
      expect(clipFile.length).toBeGreaterThan(8);
      // MIDI 2.0 Clip File header magic "SMF2CLIP".
      expect(Array.from(clipFile.subarray(0, 8))).toEqual([
        0x53, 0x4d, 0x46, 0x32, 0x43, 0x4c, 0x49, 0x50,
      ]);

      const reimported = new Project();
      try {
        const firstClip = reimported.importClipFile(clipFile);
        expect(firstClip).toBeGreaterThan(0);
        expect(Array.from(reimported.exportClipFile().subarray(0, 8))).toEqual([
          0x53, 0x4d, 0x46, 0x32, 0x43, 0x4c, 0x49, 0x50,
        ]);
      } finally {
        reimported.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('validates MIDI event inputs before native conversion', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 1);

      expect(() =>
        project.setMidiEvents(clipId, [{ ppq: Number.NaN, word0: 0, word1: 0 }]),
      ).toThrow(/ppq/);
      expect(() => project.setMidiEvents(clipId, [{ ppq: 0, word0: -1, word1: 0 }])).toThrow(
        /word0/,
      );
    } finally {
      project.destroy();
    }
  });

  it('refuses a tuple event and the data0/data1 keys', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 1);
      const word = 0x20903c64;

      expect(() =>
        project.setMidiEvents(clipId, [[0, word, 0] as unknown as { ppq: number; word0: number }]),
      ).toThrow(TypeError);
      expect(() =>
        project.setMidiEvents(clipId, [
          { ppq: 0, data0: word } as unknown as { ppq: number; word0: number },
        ]),
      ).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('validates MIDI note pairing and detects hanging notes', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      // A well-paired note-on / note-off.
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
      ]);
      const paired = project.validateMidiNotes(clipId);
      expect(paired.ok).toBe(true);
      expect(paired.unmatchedNoteOns).toBe(0);
      expect(paired.unmatchedNoteOffs).toBe(0);

      // A single hanging note-on (no matching note-off).
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      ]);
      const hanging = project.validateMidiNotes(clipId);
      expect(hanging.ok).toBe(false);
      expect(hanging.unmatchedNoteOns).toBe(1);
      expect(hanging.unmatchedNoteOffs).toBe(0);

      // UMP groups are independent endpoint namespaces.
      project.setMidiEvents(clipId, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 1, channel: 0, note: 60, velocity: 0 }),
      ]);
      const crossGroup = project.validateMidiNotes(clipId);
      expect(crossGroup.ok).toBe(false);
      expect(crossGroup.unmatchedNoteOns).toBe(1);
      expect(crossGroup.unmatchedNoteOffs).toBe(1);

      // The clip window is half-open, so an off at exactly lengthPpq lies outside
      // it; export closes the note at the clip end, so it still validates.
      const { clipId: boundaryClip } = project.addMidiClip(0, 1);
      project.setMidiEvents(boundaryClip, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 61, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 61, velocity: 0 }),
      ]);
      const boundary = project.validateMidiNotes(boundaryClip);
      expect(boundary.ok).toBe(true);
      expect(boundary.unmatchedNoteOns).toBe(0);
      expect(boundary.unmatchedNoteOffs).toBe(0);
    } finally {
      project.destroy();
    }
  });

  it('throws when validateMidiNotes targets a non-MIDI clip', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
      expect(() => project.validateMidiNotes(clipId)).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('throws cleanly on malformed fromJson input', () => {
    expect(() => Project.fromJson('{ not valid project json')).toThrow();
  });

  it('refuses a fractional project rate with the C code, on load and on set', () => {
    const current = new Project();
    const parsed = JSON.parse(current.toJson());
    current.destroy();
    const document = JSON.stringify({ ...parsed, sampleRate: 44100.5 });
    for (const load of [
      () => Project.fromJson(document),
      () => Project.fromJsonWithDiagnostics(document),
    ]) {
      let caught: unknown;
      try {
        load();
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidFormat);
      // The diagnostic code stays in the diagnostics; the refusal carries the rule alone.
      expect(String((caught as Error).message)).toMatch(
        /^Project\.fromJson(WithDiagnostics)?: sampleRate must be an integer in \[8000, 384000\]$/,
      );
    }
    const project = new Project();
    try {
      let caught: unknown;
      try {
        project.setSampleRate(44100.5);
      } catch (error) {
        caught = error;
      }
      expectArgumentRefusal(caught, RangeError);
      expect(String((caught as Error).message)).toContain(
        'sampleRate must be an integer in [8000, 384000]',
      );
    } finally {
      project.destroy();
    }
  });

  it('refuses sub-floor tempo values in both project JSON loaders', () => {
    const current = new Project();
    const parsed = JSON.parse(current.toJson()) as Record<string, unknown>;
    current.destroy();
    const badSegments = [[{ startPpq: 0, bpm: 1e-8 }], [{ startPpq: 0, bpm: 120, endBpm: 1e-8 }]];
    const paths = ['tempoSegments[0].bpm', 'tempoSegments[0].endBpm'];
    for (let i = 0; i < badSegments.length; i++) {
      const document = JSON.stringify({ ...parsed, tempoSegments: badSegments[i] });
      for (const [entry, load] of [
        ['fromJson', () => Project.fromJson(document)],
        ['fromJsonWithDiagnostics', () => Project.fromJsonWithDiagnostics(document)],
      ] as const) {
        let caught: unknown;
        try {
          load();
        } catch (error) {
          caught = error;
        }
        expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidFormat);
        expect(String((caught as Error).message)).toBe(
          `Project.${entry}: ${paths[i]} must be finite and in [1e-6, 100000]`,
        );
      }
    }

    const atFloor = Project.fromJson(
      JSON.stringify({ ...parsed, tempoSegments: [{ startPpq: 0, bpm: 1e-6, endBpm: 0 }] }),
    );
    expect(atFloor.tempoSegments()[0]?.bpm).toBe(1e-6);
    atFloor.destroy();
  });

  it('reports clip count after add, import, and remove', () => {
    const project = new Project();
    try {
      expect(project.clips().length).toBe(0);
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
      expect(project.clips().length).toBe(1);
      const restored = Project.fromJson(project.toJson());
      try {
        expect(restored.clips().length).toBe(1);
      } finally {
        restored.destroy();
      }
      project.removeClip(clipId);
      expect(project.clips().length).toBe(0);
    } finally {
      project.destroy();
    }
  });

  it('fromJsonWithDiagnostics returns warnings from successful loads', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(danglingSourceJson());
    try {
      expect(diagnostics).toContain('dangling_clip_source');
      expect(project.tracks().length).toBe(1);
    } finally {
      project.destroy();
    }
  });

  // Golden vectors for the hand-written UMP MIDI-1.0 channel-voice packing
  // (midiNoteOn/Off/Cc/... build word0 in TS rather than via the native
  // packer). These exact words are what sonare::midi::make_midi1_* — and thus
  // the C-ABI sonare_midi_* packers Python delegates to — produce, so pinning
  // them here (with the identical vectors mirrored in the Node suite) makes any
  // silent drift between the two hand-written JS copies a CI failure.
  describe('UMP MIDI-1.0 packing matches the canonical word layout', () => {
    it('packs channel-voice messages into the spec word', () => {
      expect(
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }).word0 >>> 0,
      ).toBe(0x20903c64);
      expect(midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }).word0 >>> 0).toBe(
        0x20803c00,
      );
      expect(midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 127 }).word0 >>> 0).toBe(
        0x20b0077f,
      );
      expect(
        midiPolyPressure({ ppq: 0, group: 0, channel: 0, note: 60, pressure: 64 }).word0 >>> 0,
      ).toBe(0x20a03c40);
      expect(midiProgram({ ppq: 0, group: 0, channel: 0, program: 5 }).word0 >>> 0).toBe(
        0x20c00500,
      );
      expect(midiChannelPressure({ ppq: 0, group: 0, channel: 0, pressure: 90 }).word0 >>> 0).toBe(
        0x20d05a00,
      );
      // Group and channel nibbles land in their own fields.
      expect(
        midiNoteOn({ ppq: 0, group: 0xa, channel: 0x3, note: 60, velocity: 100 }).word0 >>> 0,
      ).toBe(0x2a933c64);
    });

    it('exposes ProgramMap names and bank/program lowering', () => {
      expect(gmInstrumentName(0)).toBe('Acoustic Grand Piano');
      expect(gmInstrumentName(40)).toBe('Violin');
      expect(gmProgramForName('Violin')).toBe(40);
      expect(gmFamilyName(4)).toBe('Bass');
      expect(gmFamilyFirstProgram(4)).toBe(32);
      expect(gm2InstrumentName({ bankLsb: 1, program: 24 })).toBe('Ukulele');
      expect(gmDrumName(38)).toBe('Acoustic Snare');
      expect(gmDrumNoteForName('Open Triangle')).toBe(81);
      expect(gm2DrumSetName(40)).toBe('Brush');
      expect(gm2DrumName({ bankLsb: 40, note: 40 })).toBe('Brush Swirl');
      expect(midiCcName(74)).toBe('Brightness');
      expect(midiCcIndexForName('Pan (MSB)')).toBe(10);
      expect(perNoteControllerName(11)).toBe('Expression');

      const events = midiBankProgram({
        ppq: 0,
        group: 0,
        channel: 3,
        program: 24,
        bank: { msb: 0x79, lsb: 1 },
      });
      expect(events.map((event) => event.word0 >>> 0)).toEqual([
        0x20b30079, 0x20b32001, 0x20c31800,
      ]);
      expect(events.every((event) => event.ppq === 0 && event.word1 === 0)).toBe(true);
    });

    it('refuses bank/program domains in declaration order after checking shape', () => {
      const request = {
        ppq: -1,
        group: 16,
        channel: 16,
        bank: { msb: 128, lsb: 128 },
        program: 128,
      };
      expect(() => midiBankProgram(request)).toThrow(
        new RangeError('midiBankProgram: ppq must be finite and in [0, 1e12]'),
      );
      expect(() => midiBankProgram({ ...request, ppq: 0 })).toThrow(
        new RangeError('midiBankProgram: group must be an integer in [0, 15]'),
      );
      expect(() => midiBankProgram({ ...request, ppq: 0, group: 0 })).toThrow(
        new RangeError('midiBankProgram: channel must be an integer in [0, 15]'),
      );
      expect(() => midiBankProgram({ ...request, ppq: 0, group: 0, channel: 0 })).toThrow(
        new RangeError('midiBankProgram: bank.msb must be an integer in [0, 127]'),
      );
      const validHead = { ...request, ppq: 0, group: 0, channel: 0 };
      expect(() => midiBankProgram({ ...validHead, bank: { msb: 0, lsb: 128 } })).toThrow(
        new RangeError('midiBankProgram: bank.lsb must be an integer in [0, 127]'),
      );
      expect(() => midiBankProgram({ ...validHead, bank: { msb: 0, lsb: 0 } })).toThrow(
        new RangeError('midiBankProgram: program must be an integer in [0, 127]'),
      );
      expect(() => midiBankProgram({ ...request, program: '128' as never })).toThrow(TypeError);
    });

    it('refuses an unknown name or an out-of-range number from every lookup', () => {
      expect(() => gmInstrumentName(128)).toThrow(RangeError);
      expect(() => gmInstrumentName(-1)).toThrow(RangeError);
      expect(() => gmInstrumentName('0' as never)).toThrow(TypeError);
      expect(() => gmProgramForName('No Such Instrument')).toThrow(
        new RangeError('gmProgramForName: name must be a GM instrument name'),
      );
      expect(() => gmProgramForName(5 as never)).toThrow(TypeError);
      expect(() => gmFamilyName(16)).toThrow(RangeError);
      expect(() => gmFamilyFirstProgram(16)).toThrow(RangeError);
      expect(() => gm2InstrumentName({ bankLsb: 128, program: 0 })).toThrow(RangeError);
      expect(() => gm2InstrumentName({ bankLsb: 1, program: 0.5 })).toThrow(RangeError);
      expect(() => gmDrumName(128)).toThrow(RangeError);
      // An in-range number that names nothing in the table is absence, not a refusal.
      expect(gmDrumName(0)).toBeUndefined();
      expect(gm2DrumSetName(1)).toBeUndefined();
      expect(() => gmDrumNoteForName('No Such Drum')).toThrow(RangeError);
      expect(() => gm2DrumSetName(128)).toThrow(RangeError);
      expect(() => gm2DrumName({ bankLsb: 40, note: 128 })).toThrow(RangeError);
      expect(() => midiCcName(128)).toThrow(RangeError);
      expect(() => midiCcIndexForName('No Such Controller')).toThrow(RangeError);
      expect(() => perNoteControllerName(256)).toThrow(RangeError);
      expect(() => gm2InstrumentName({ bankLsb: 1 } as never)).toThrow(TypeError);
    });

    it('refuses an unknown request key and a wrong-typed field on a packer', () => {
      expect(() =>
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100, extra: 1 } as never),
      ).toThrow(TypeError);
      expect(() =>
        midi2Cc({ ppq: 0, group: 0, channel: 0, controller: 7, value32: 0, extra: 1 } as never),
      ).toThrow(TypeError);
      expect(() =>
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: '60', velocity: 100 } as never),
      ).toThrow(TypeError);
      expect(() => midiNoteOn([] as never)).toThrow(TypeError);
      expect(() => midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 128, velocity: 100 })).toThrow(
        RangeError,
      );
    });

    it('routes MIDI events through the native MidiRouter', () => {
      const routed = midiRouteEvents({
        events: [
          midiNoteOn({ ppq: 0, group: 0, channel: 3, note: 60, velocity: 100 }),
          midiNoteOff({ ppq: 0.5, group: 0, channel: 3, note: 60, velocity: 0 }),
          midiNoteOn({ ppq: 1, group: 0, channel: 2, note: 61, velocity: 100 }),
        ],
        filterChannel: 3,
        remapChannel: 7,
      });
      expect(routed.overflowed).toBe(false);
      expect(routed.overflowCount).toBe(0);
      expect(routed.events).toHaveLength(2);
      expect(routed.events.map((event) => event.ppq)).toEqual([0, 0.5]);
      expect(routed.events.map((event) => (event.word0 >>> 16) & 0xf)).toEqual([7, 7]);
      expect((routed.events[1].word0 >>> 20) & 0xf).toBe(0x8);

      const muted = midiRouteEvents({
        events: [midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 })],
        thru: false,
      });
      expect(muted.events).toHaveLength(0);
      expect(muted.overflowed).toBe(false);
    });

    it('midiRouteEvents copies SysEx bytes and leaves absent payloads absent', () => {
      const payload = Uint8Array.of(0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7);
      const result = midiRouteEvents({
        events: [
          { ppq: 0, word0: 0x30000000, word1: 0, sysex: payload },
          midiNoteOn({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 100 }),
        ],
      });

      expect(result.events).toStrictEqual([
        { ppq: 0, word0: 0x30000000, word1: 0, sysex: payload },
        midiNoteOn({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 100 }),
      ]);
      expect(result.events[1]).not.toHaveProperty('sysex');
      expect(result.events[0].sysex).not.toBe(payload);

      payload.fill(0);
      expect(Array.from(result.events[0].sysex ?? [])).toEqual([
        0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7,
      ]);
      expect(
        expectRefusalOf(
          () =>
            midiRouteEvents({
              events: [{ ppq: 0, word0: 0x30000000, word1: 0, sysex: [0xf0, 0xf7] as never }],
            }),
          TypeError,
        ).message,
      ).toContain('events[0].sysex');
      expect(
        expectRefusalOf(
          () =>
            midiRouteEvents({ events: [{ ppq: 0, word0: 0x30000000, sysex: new Uint8Array() }] }),
          RangeError,
        ).message,
      ).toContain('events[0].sysex');
    });

    it('takes the MIDI route, learn and breakpoint helpers as one request and refuses the positional form', () => {
      const events = [midiCc({ ppq: 0, group: 0, channel: 2, controller: 74, value: 64 })];
      const binding = {
        ccNumber: 74,
        channel: 2,
        kind: 'controlChange7' as const,
        paramId: 9,
        minValue: 0,
        maxValue: 1,
      };
      expect(midiRouteEvents({ events, filterChannel: 2 }).events).toHaveLength(1);
      expect(midiCcLearn({ events, paramId: 9 })).toMatchObject({
        ccNumber: 74,
        paramId: 9,
        consume: false,
      });
      expect(midiCcToBreakpoint({ bindings: [binding], event: events[0] })).not.toBeNull();
      const loose = (fn: unknown) => fn as (...args: unknown[]) => unknown;
      for (const [name, call] of [
        ['midiRouteEvents', () => loose(midiRouteEvents)(events, { filterChannel: 2 })],
        ['midiCcLearn', () => loose(midiCcLearn)(events, 9, {})],
        ['midiCcToBreakpoint', () => loose(midiCcToBreakpoint)([binding], events[0])],
      ] as const) {
        expect(call, name).toThrow(new TypeError(`${name}: request must be an object`));
      }
      expect(() => midiRouteEvents({ events, channel: 2 } as never)).toThrow(TypeError);
      expect(() => midiCcLearn({ events, paramId: 9, min: 0 } as never)).toThrow(TypeError);
      expect(() =>
        midiCcToBreakpoint({ bindings: [binding], event: events[0], ppq: 0 } as never),
      ).toThrow(TypeError);
    });

    it('reads a null route-config field as absent', () => {
      const events = [
        midiNoteOn({ ppq: 0, group: 0, channel: 3, note: 60, velocity: 100 }),
        midiNoteOn({ ppq: 1, group: 0, channel: 2, note: 61, velocity: 100 }),
      ];
      const nulls = { filterGroup: null, filterChannel: null, remapChannel: null };
      expect(midiRouteEvents({ events, ...nulls } as never)).toEqual(midiRouteEvents({ events }));
      expect(midiRouteEvents({ events, ...nulls, filterChannel: 3 } as never).events).toHaveLength(
        1,
      );
    });

    it('takes the CC binding kind as a token and refuses an ordinal', () => {
      const learned = midiCcLearn({
        events: [
          midiCc({ ppq: 0, group: 0, channel: 2, controller: 1, value: 64 }),
          midiCc({ ppq: 0.1, group: 0, channel: 2, controller: 33, value: 12 }),
        ],
        paramId: 77,
      });
      expect(learned?.kind).toBe('controlChange14');

      const binding = {
        ccNumber: 1,
        ccLsbNumber: 33,
        channel: 2,
        paramId: 77,
        minValue: 0,
        maxValue: 1,
      };
      const event = midiCc({ ppq: 1, group: 0, channel: 2, controller: 1, value: 64 });
      expect(() =>
        midiCcToBreakpoint({ bindings: [{ ...binding, kind: 'controlChange14' }], event }),
      ).not.toThrow();
      expect(() =>
        midiCcToBreakpoint({ bindings: [{ ...binding, kind: 1 as never }], event }),
      ).toThrow(TypeError);
    });

    it('exposes native CcMap learn and CC/automation conversion helpers', () => {
      const learned14 = midiCcLearn({
        events: [
          midiCc({ ppq: 0, group: 0, channel: 2, controller: 1, value: 64 }),
          midiCc({ ppq: 0.1, group: 0, channel: 2, controller: 33, value: 12 }),
        ],
        paramId: 77,
        minValue: -1,
        maxValue: 1,
      });
      expect(learned14).toMatchObject({
        kind: 'controlChange14',
        ccNumber: 1,
        ccLsbNumber: 33,
        channel: 2,
        paramId: 77,
        minValue: -1,
        maxValue: 1,
      });

      const learnedRpn = midiCcLearn({
        events: [
          midiCc({ ppq: 0, group: 0, channel: 3, controller: 101, value: 0 }),
          midiCc({ ppq: 0.1, group: 0, channel: 3, controller: 100, value: 1 }),
          midiCc({ ppq: 0.2, group: 0, channel: 3, controller: 6, value: 64 }),
        ],
        paramId: 78,
      });
      expect(learnedRpn).toMatchObject({
        kind: 'rpn',
        selectorMsb: 0,
        selectorLsb: 1,
        paramId: 78,
      });

      const binding = {
        ccNumber: 74,
        channel: 4,
        kind: 'controlChange7' as const,
        paramId: 88,
        minValue: -60,
        maxValue: 0,
      };
      const point = midiCcToBreakpoint({
        bindings: [binding],
        event: midiCc({ ppq: 2, group: 0, channel: 4, controller: 74, value: 127 }),
      });
      expect(point).toEqual({ ppq: 2, value: 0, curveToNext: 'linear' });

      const event = midiParamToCc({
        bindings: [binding],
        paramId: 88,
        unitValue: -60,
        group: 0,
        ppq: 3,
      });
      expect(event?.ppq).toBe(3);
      expect(((event?.word0 ?? 0) >>> 16) & 0xf).toBe(4);
      expect(((event?.word0 ?? 0) >>> 8) & 0x7f).toBe(74);
      expect((event?.word0 ?? 0) & 0x7f).toBe(0);
    });
  });

  describe('GS voicing queries', () => {
    it('reports the sets that render as Standard as the ones that say so', () => {
      // The three states stay distinct: null where no set sits, false where a set
      // renders exactly as Standard, true where it is voiced apart. A truthiness
      // check collapses the first two: `!voicedApart` is the 102 programs holding
      // no set plus the 5 answering false, so 107 entries read as placeholders
      // where there are four -- Standard answers false about itself and is a real
      // choice, so the count is wrong at both ends.
      const named = new Map<number, string>();
      for (let program = 0; program < 128; program++) {
        const name = synthGsDrumKitName(program);
        if (name === null) {
          expect(synthGsDrumKitIsVoicedApart(program)).toBeNull();
        } else {
          named.set(program, name);
        }
      }
      expect(named.size).toBe(26);
      const sameAsStandard = [...named]
        .filter(([program]) => synthGsDrumKitIsVoicedApart(program) === false)
        .map(([program, name]) => `${program}:${name}`);
      expect(sameAsStandard).toEqual([
        '0:Standard',
        '53:Cymbal & Claps',
        '56:SFX',
        '57:Rhythm FX',
        '58:Rhythm FX 2',
      ]);
    });

    it('reports whether a variation bank is voiced or falls back to the capital', () => {
      expect(synthGsVariationIsVoicedApart(0, 0)).toBe(false);
      expect(synthGsVariationIsVoicedApart(8, 0)).toBe(true);
      expect(synthGsVariationIsVoicedApart(1, 0)).toBe(true);
      // GS resolving a variation this build does not voice to the capital tone.
      expect(synthGsVariationIsVoicedApart(24, 16)).toBe(false);
      // The bank's upper end was bounded by what a uint16_t holds rather than by
      // what a Bank Select means, so 128 and up answered false -- indistinguishable
      // from a real capital-tone resolution.
      expect(synthGsVariationIsVoicedApart(128, 0)).toBeNull();
      expect(synthGsVariationIsVoicedApart(0xffff, 0)).toBeNull();
      expect(synthGsVariationIsVoicedApart(127, 0)).not.toBeNull();
      expect(synthGsVariationIsVoicedApart(0, -1)).toBeNull();
      expect(synthGsVariationIsVoicedApart(0, 128)).toBeNull();
    });
  });
});

describe('Sonare WASM Project meter plumbing', () => {
  beforeAll(async () => {
    await init();
  });

  /** Click track at 120 BPM: a quiet click every beat, an accented one on every fourth beat from `downbeatIndex`. */
  function accentedClicks(
    sampleRate: number,
    firstBeatSec: number,
    downbeatIndex: number,
    seconds: number,
  ): Float32Array {
    const audio = new Float32Array(Math.round(sampleRate * seconds));
    const beatSec = 0.5;
    for (let k = 0; firstBeatSec + k * beatSec < seconds - 0.2; k++) {
      const at = Math.round((firstBeatSec + k * beatSec) * sampleRate);
      const accented = k >= downbeatIndex && (k - downbeatIndex) % 4 === 0;
      const amp = accented ? 1 : 0.35;
      const freq = accented ? 1500 : 900;
      for (let i = 0; i < 256 && at + i < audio.length; i++) {
        audio[at + i] += amp * Math.exp(-i / 40) * Math.sin((2 * Math.PI * freq * i) / sampleRate);
      }
    }
    return audio;
  }

  /** Format-0 SMF at 480 PPQN: 120 BPM, a Time Signature (denominator 4) at each `[tick, numerator]`, one note. */
  function smfWithSignatures(sigs: Array<[number, number]>): Uint8Array {
    const vlq = (value: number): number[] => {
      const out = [value & 0x7f];
      for (let rest = value >>> 7; rest > 0; rest >>>= 7) {
        out.unshift((rest & 0x7f) | 0x80);
      }
      return out;
    };
    const body: number[] = [0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20];
    let tick = 0;
    for (const [at, numerator] of sigs) {
      body.push(...vlq(at - tick), 0xff, 0x58, 0x04, numerator, 0x02, 0x18, 0x08);
      tick = at;
    }
    body.push(0x00, 0x90, 0x3c, 0x64, ...vlq(480), 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00);
    const head = [0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0];
    const track = [0x4d, 0x54, 0x72, 0x6b, 0, 0, (body.length >> 8) & 0xff, body.length & 0xff];
    return new Uint8Array([...head, ...track, ...body]);
  }

  it('autoTempo applies a first time signature on the first downbeat bar phase', () => {
    // The first downbeat is click 9 at 5 s = ppq 10; the lead-in is 2.5 bars, so the signature
    // starts at ppq 10 mod 4 = 2 and the bar lines run 2, 6, 10. The tracker places beats to one
    // chroma hop (512 / 22050 s = 23.2 ms = 0.046 beats at 120 BPM), so 0.05 ppq.
    const tolerance = 0.05;
    const project = new Project();
    try {
      project.autoTempo(accentedClicks(22050, 0.5, 9, 16), 22050, { applyTimeSignatures: true });
      const signatures = project.timeSignatures();
      expect(signatures[0].numerator).toBe(4);
      expect(signatures[0].denominator).toBe(4);
      expect(Math.abs(signatures[0].startPpq - 2)).toBeLessThan(tolerance);
      expect(Math.abs(project.snapToGrid(10.2, 1.0, 0) - 10)).toBeLessThan(tolerance);
      expect(Math.abs(project.snapToGrid(6.2, 1.0, 0) - 6)).toBeLessThan(tolerance);
    } finally {
      project.destroy();
    }
  });

  it('imports a late first SMF time signature with the format 4/4 stored at ppq 0', () => {
    const project = new Project();
    try {
      project.importSmf(smfWithSignatures([[1920, 3]]));
      expect(project.timeSignatures()).toEqual([
        { startPpq: 0, numerator: 4, denominator: 4 },
        { startPpq: 4, numerator: 3, denominator: 4 },
      ]);
    } finally {
      project.destroy();
    }
  });

  it('folds an SMF tick-0 time signature that the next one repeats unchanged', () => {
    const project = new Project();
    try {
      project.importSmf(
        smfWithSignatures([
          [0, 3],
          [2880, 3],
        ]),
      );
      expect(project.timeSignatures()).toEqual([{ startPpq: 6, numerator: 3, denominator: 4 }]);
    } finally {
      project.destroy();
    }
  });

  it('keeps a late first time signature through SMF export then import', () => {
    const stored = [{ startPpq: 2, numerator: 3, denominator: 4 }];
    const source = new Project();
    const target = new Project();
    try {
      source.importSmf(smfWithSignatures([[960, 3]]));
      source.setTimeSignatures(stored);
      target.importSmf(source.exportSmf());
      expect(target.timeSignatures()).toEqual(stored);
    } finally {
      source.destroy();
      target.destroy();
    }
  });
});
