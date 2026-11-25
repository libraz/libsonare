import { describe, expect, expectTypeOf, it } from 'vitest';
import type { BuiltinSynthConfig, ProjectMidiEvent, ProjectTrack } from '../src/index.js';
import {
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
  isSonareError,
  midi2Cc,
  midi2PerNoteManagement,
  midi2Program,
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
} from '../src/index.js';
import { refusalOf } from '../src/internal/entry.js';
import { EXPECTED_PROJECT_ABI_VERSION } from '../src/types_project.js';
import { expectRefusal, unknownKeyError, unknownKeyText } from './_helpers.js';

/** Asserts the empty-chord refusal a MIDI-FX config gets at `entry`. */
const expectEmptyChord = (run: () => unknown, entry: string): void => {
  expectRefusal(
    run,
    RangeError,
    new RegExp(`^Project\\.${entry}: config\\.chordIntervals must hold 1 to 8 intervals$`),
  );
};

/** A byte view for subsequence search and ASCII reads on an exported file. */
function bytes(data: Uint8Array): Buffer {
  return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
}

/** Largest absolute sample over every channel of a bounce. */
function peak(result: { channels: Float32Array[] }): number {
  let p = 0;
  for (const channel of result.channels) {
    for (let i = 0; i < channel.length; i++) {
      const a = Math.abs(channel[i]);
      if (a > p) {
        p = a;
      }
    }
  }
  return p;
}

/** Build a small deterministic project: a track + an audio clip + a MIDI clip + tempo. */
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
    gain: 0.8,
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

it('accepts a metadata-only audio clip with a source URI', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'audio' });
  expect(() =>
    project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'asset://lead.wav' }),
  ).not.toThrow();
  expect(project.toJson()).toContain('asset://lead.wav');
  project.destroy();
});

it('reads stored tracks, clips, and sources as arrays', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'audio', name: 'readback' });
  const clipId = project.addClip({
    trackId,
    startPpq: 2,
    lengthPpq: 4,
    sourceOffsetPpq: 1,
    gain: 0.75,
    sourceUri: 'asset://readback.wav',
  });
  try {
    expect(project.tracks()).toHaveLength(1);
    expect(project.tracks()[0]).toMatchObject({
      id: trackId,
      kind: 'audio',
      name: 'readback',
      gain: 1,
      pan: 0,
      mute: false,
      solo: false,
    });
    expect(project.clips()).toHaveLength(1);
    expect(project.clips()[0]).toMatchObject({
      id: clipId,
      trackId,
      sourceKind: 'audio',
      startPpq: 2,
      lengthPpq: 4,
      sourceOffsetPpq: 1,
      gain: 0.75,
      loopMode: 'off',
    });
    expect(project.clips()[0]).not.toHaveProperty('loopLengthPpq');
    expect(project.sources()).toHaveLength(1);
    expect(project.sources()[0]).toMatchObject({
      id: project.clips()[0].sourceId,
      kind: 'audio',
      nameOrUri: 'asset://readback.wav',
    });
    expect(project.tracks()[1]).toBeUndefined();
    expect(project.clips()[1]).toBeUndefined();
    expect(project.sources()[1]).toBeUndefined();
  } finally {
    project.destroy();
  }
});

it('reads and edits audio source metadata without mutating input', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'audio' });
  project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'asset://metadata.wav' });
  const sourceId = project.clips()[0].sourceId;
  const { clipId: midiClipId } = project.addMidiClip(0, 1);
  const midiSourceId = project.clips()[1].sourceId;
  const metadata = { contentHash: 'sha256:deadbeef', externalStemRole: 'lead' };

  try {
    expect(project.sources()[0]).toMatchObject({
      id: sourceId,
      kind: 'audio',
      nameOrUri: 'asset://metadata.wav',
    });
    expect(project.sources()[0]).not.toHaveProperty('contentHash');
    expect(project.sources()[0]).not.toHaveProperty('externalStemRole');

    project.setAudioSourceMetadata({ sourceId, ...metadata });
    expect(metadata).toEqual({ contentHash: 'sha256:deadbeef', externalStemRole: 'lead' });
    expect(project.sources()[0]).toMatchObject(metadata);

    expect(() => project.setAudioSourceMetadata({ sourceId, contentHash: '' })).toThrow(
      /contentHash must not be empty/,
    );
    project.setAudioSourceMetadata({ sourceId });
    expect(project.sources()[0]).toMatchObject({
      id: sourceId,
      nameOrUri: 'asset://metadata.wav',
    });
    expect(project.sources()[0]).not.toHaveProperty('contentHash');
    expect(project.sources()[0]).not.toHaveProperty('externalStemRole');
    project.undo();
    expect(project.sources()[0]).toMatchObject(metadata);
    project.redo();
    expect(project.sources()[0]).not.toHaveProperty('contentHash');

    project.setAudioSourceMetadata({ sourceId, ...metadata });
    const restored = Project.fromJson(project.toJson());
    try {
      expect(restored.sources()[0]).toMatchObject(metadata);
    } finally {
      restored.destroy();
    }

    expect(() => project.setAudioSourceMetadata({ sourceId: 999999, ...metadata })).toThrow();
    expect(() => project.setAudioSourceMetadata({ sourceId: midiSourceId, ...metadata })).toThrow();
    expect(project.clips()[1].id).toBe(midiClipId);
    expect(project.sources()[0]).toMatchObject(metadata);
    expect(project.sources()[1]).toMatchObject({
      id: midiSourceId,
      kind: 'midi',
    });
    expect(project.sources()[1]).not.toHaveProperty('contentHash');
    expect(project.sources()[1]).not.toHaveProperty('externalStemRole');
  } finally {
    project.destroy();
  }
});

it('rebinds deserialized source audio before bouncing', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'audio' });
  const audio = new Float32Array(480).fill(0.25);
  project.addClip({
    trackId,
    startPpq: 0,
    lengthPpq: 1,
    channels: [audio],
    sampleRate: 48000,
    sourceUri: 'asset://lead.wav',
  });
  const restored = Project.fromJson(project.toJson());
  const sourceIds = restored.unresolvedAudioSourceIds();
  expect(sourceIds).toHaveLength(1);
  restored.setSourceAudio({ sourceId: sourceIds[0], channels: [audio], sampleRate: 48000 });
  expect(restored.unresolvedAudioSourceIds()).toEqual([]);
  const bounced = restored.bounce({ totalFrames: 480, numChannels: 1 });
  expect(bounced.channels).toHaveLength(1);
  expect(bounced.channels[0]).toHaveLength(480);
  restored.destroy();
  project.destroy();
});

it('describes unresolved audio sources with the full untruncated URI', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'audio' });
  const longUri = `asset://${'a'.repeat(300)}.wav`;
  project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: longUri });
  project.addClip({ trackId, startPpq: 2, lengthPpq: 1, sourceUri: 'asset://stem.wav' });
  const restored = Project.fromJson(project.toJson());
  const ids = restored.unresolvedAudioSourceIds();
  expect(ids).toHaveLength(2);
  restored.setAudioSourceMetadata({
    sourceId: ids[1],
    contentHash: 'sha256:abc',
    externalStemRole: 'vocals',
  });
  const sources = restored.unresolvedAudioSources();
  expect(sources.map((source) => source.id)).toEqual(ids);
  expect(sources[0].nameOrUri).toBe(longUri);
  expect(sources[0].kind).toBe('audio');
  expect(sources[1].nameOrUri).toBe('asset://stem.wav');
  expect(sources[1].contentHash).toBe('sha256:abc');
  expect(sources[1].externalStemRole).toBe('vocals');
  expect(restored.sources()[0].nameOrUri).toBe(longUri);
  restored.setSourceAudio({
    sourceId: ids[0],
    channels: [new Float32Array(480).fill(0.25)],
    sampleRate: 48000,
  });
  expect(restored.unresolvedAudioSources().map((source) => source.id)).toEqual([ids[1]]);
  restored.destroy();
  project.destroy();
});

it('adds and relinks an audio source without changing its id and restores it on undo', () => {
  const project = new Project();
  try {
    expect(() => project.addAudioSource('asset://bad\0uri')).toThrow(RangeError);
    expect(() => project.addAudioSource('asset://\ud800.wav')).toThrow(RangeError);
    expect(() => project.addAudioSource('')).toThrow(RangeError);
    const uri = `asset://${'音'.repeat(180)}.wav`;
    const sourceId = project.addAudioSource(uri);
    expect(project.addAudioSource(uri)).not.toBe(sourceId);
    project.setAudioSourceMetadata({
      sourceId,
      contentHash: 'sha256:before',
      externalStemRole: 'vocals',
    });
    project.setSourceAudio({
      sourceId,
      channels: [new Float32Array(64).fill(0.25)],
      sampleRate: 48000,
    });
    const before = project.sources().find((source) => source.id === sourceId);
    expect(before?.nameOrUri).toBe(uri);
    expect(project.unresolvedAudioSourceIds()).not.toContain(sourceId);

    expect(project.relinkSource(sourceId, 'asset://replacement.wav')).toBe(sourceId);
    expect(project.unresolvedAudioSourceIds()).toContain(sourceId);
    const relinked = project.sources().find((source) => source.id === sourceId);
    expect(relinked?.nameOrUri).toBe('asset://replacement.wav');
    expect(relinked).not.toHaveProperty('contentHash');
    expect(relinked?.externalStemRole).toBe('vocals');

    project.undo();
    expect(project.unresolvedAudioSourceIds()).not.toContain(sourceId);
    expect(project.sources().find((source) => source.id === sourceId)).toEqual(before);
    project.redo();
    expect(project.unresolvedAudioSourceIds()).toContain(sourceId);
  } finally {
    project.destroy();
  }
});

it('names a non-finite source sample by its planar channel coordinate', () => {
  const project = new Project();
  try {
    const sourceId = project.addAudioSource('asset://source.wav');
    expectRefusal(
      () =>
        project.setSourceAudio({
          sourceId,
          channels: [new Float32Array([0.25, 0.5]), new Float32Array([0.75, Number.NaN])],
          sampleRate: 48000,
        }),
      RangeError,
      /^Project\.setSourceAudio: channels\[1\]\[1\] must be finite$/,
    );
  } finally {
    project.destroy();
  }
});

it('reads key annotations in stored order and restores them with redo', () => {
  const project = new Project();
  try {
    const keys = [
      { startPpq: 4, endPpq: 8, tonicPc: 9, mode: 'mixolydian' as const },
      { startPpq: 0, endPpq: 4, mode: 'unknown' as const },
    ];
    project.annotateKeys(keys);
    expect(project.keyAnnotations()).toEqual(keys);
    project.undo();
    expect(project.keyAnnotations()).toEqual([]);
    project.redo();
    expect(project.keyAnnotations()).toEqual(keys);
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
    expect(() => project.annotateKeys(keys)).toThrow(
      new TypeError(
        'Project.annotateKeys: unknown key keys[1].unexpected (expected endPpq, mode, startPpq, tonicPc)',
      ),
    );

    const functionRow = Object.assign(() => undefined, {
      startPpq: 4,
      endPpq: 8,
      unexpected: true,
    });
    const functionKeys = [{ startPpq: 0, endPpq: 4 }, functionRow] as never;
    expect(() => project.annotateKeys(functionKeys)).toThrow(
      new TypeError(
        'Project.annotateKeys: unknown key keys[1].unexpected (expected endPpq, mode, startPpq, tonicPc)',
      ),
    );

    const chords = [
      { startPpq: 0, endPpq: 4, quality: 'major' },
      { startPpq: 4, endPpq: 8, quality: 'minor', unexpected: true },
    ] as never;
    expect(() => project.annotateChords(chords)).toThrow(
      new TypeError(
        'Project.annotateChords: unknown key chords[1].unexpected (expected endPpq, extensions, modulationBoundary, quality, romanNumeral, rootPc, slashBassPc, startPpq)',
      ),
    );
  } finally {
    project.destroy();
  }
});

it('reads chord annotations with owned extensions and no unset numeral', () => {
  const project = new Project();
  try {
    const chords = [
      {
        startPpq: 4,
        endPpq: 8,
        rootPc: 7,
        quality: 'dominant' as const,
        extensions: [7, 9],
        slashBassPc: 0,
        modulationBoundary: true,
        romanNumeral: 'V7',
      },
      {
        startPpq: 0,
        endPpq: 4,
        rootPc: 0,
        quality: 'major' as const,
        extensions: [3, 5],
        modulationBoundary: false,
      },
      {
        startPpq: 8,
        endPpq: 12,
        rootPc: 2,
        quality: 'minor' as const,
        extensions: [3, 5],
        modulationBoundary: false,
      },
    ];
    project.annotateChords(chords);
    const annotations = project.chordAnnotations();
    expect(annotations).toEqual(chords);
    expect(Object.hasOwn(annotations[0], 'modulationBoundary')).toBe(true);
    expect(typeof annotations[0].modulationBoundary).toBe('boolean');
    expect(annotations[0].romanNumeral).toBe('V7');
    expect(annotations[1]).not.toHaveProperty('romanNumeral');
    expect(annotations[2]).not.toHaveProperty('romanNumeral');
    project.undo();
    expect(project.chordAnnotations()).toEqual([]);
    project.redo();
    expect(project.chordAnnotations()).toEqual(chords);
  } finally {
    project.destroy();
  }
});

it('refuses an empty Roman numeral without changing chord annotations', () => {
  const project = new Project();
  try {
    const chord = { startPpq: 0, endPpq: 4, quality: 'major' as const, romanNumeral: 'I' };
    project.annotateChords([chord]);
    const before = project.chordAnnotations();
    expect(() => project.annotateChords([{ ...chord, romanNumeral: '' }])).toThrow(
      new RangeError('Project.annotateChords: chords[0].romanNumeral must not be empty'),
    );
    expect(project.chordAnnotations()).toEqual(before);
  } finally {
    project.destroy();
  }
});

it('requires chord endpoints and names invalid nested fields', () => {
  using project = new Project();
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
});

it('writes overlapping section annotations as one undoable edit', () => {
  const project = new Project();
  try {
    const sections = [
      { startPpq: 4, endPpq: 12, label: `verse ${'x'.repeat(300)}` },
      { startPpq: 0, endPpq: 8, label: '序章' },
    ];
    project.annotateSections(sections);
    expect(project.sectionAnnotations()).toEqual(sections);
    const annotated = JSON.parse(project.toJson()) as {
      annotation: { sections: unknown[] };
    };
    expect(annotated.annotation.sections).toEqual(sections);
    project.undo();
    expect((JSON.parse(project.toJson()) as typeof annotated).annotation.sections).toEqual([]);
    project.redo();
    expect(project.sectionAnnotations()).toEqual(sections);
    expect((JSON.parse(project.toJson()) as typeof annotated).annotation.sections).toEqual(
      sections,
    );
    project.annotateSections([]);
    expect(project.sectionAnnotations()).toEqual([]);
    expect((JSON.parse(project.toJson()) as typeof annotated).annotation.sections).toEqual([]);
  } finally {
    project.destroy();
  }
});

it('refuses malformed section records and text that cannot reach UTF-8 intact', () => {
  const project = new Project();
  try {
    expect(() =>
      project.annotateSections([{ startPpq: 0, endPpq: 1, label: 'bad\0label' }]),
    ).toThrow(RangeError);
    expect(() => project.annotateSections([{ startPpq: 0, endPpq: 1, label: '\ud800' }])).toThrow(
      RangeError,
    );
    expect(() =>
      project.annotateSections([{ startPpq: 0, endPpq: 1, label: 'ok', extra: 1 } as never]),
    ).toThrow(TypeError);
    expectRefusal(
      () => project.annotateSections([{ startPpq: '0', endPpq: 1, label: 'ok' } as never]),
      TypeError,
      /sections\[0\]\.startPpq/,
    );
    expectRefusal(
      () => project.annotateSections([{ startPpq: 0, endPpq: 1e12 + 1, label: 'ok' }]),
      RangeError,
      /sections\[0\]\.endPpq/,
    );
    expectRefusal(
      () => project.annotateSections([{ startPpq: 1, endPpq: 1, label: 'ok' }]),
      RangeError,
      /sections\[0\]\.endPpq/,
    );
  } finally {
    project.destroy();
  }
});

it('reports no unresolved audio sources for a MIDI-only project', () => {
  const project = new Project();
  const trackId = project.addTrack({ kind: 'midi' });
  project.addClip({ trackId, startPpq: 0, lengthPpq: 1, isMidi: true });
  expect(project.unresolvedAudioSources()).toEqual([]);
  project.destroy();
});

function makeSysexSmf(): Buffer {
  const payload = Buffer.from([0x7e, 0x7f, 0x09, 0x01, 0xf7]);
  const body = Buffer.from([
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
  const out = Buffer.alloc(14 + 8 + body.length);
  out.write('MThd', 0, 'ascii');
  out.writeUInt32BE(6, 4);
  out.writeUInt16BE(0, 8);
  out.writeUInt16BE(1, 10);
  out.writeUInt16BE(480, 12);
  out.write('MTrk', 14, 'ascii');
  out.writeUInt32BE(body.length, 18);
  body.copy(out, 22);
  return out;
}

function makeTruncatedSmf(): Buffer {
  const body = Buffer.from([
    0x00,
    0x90,
    0x3c,
    0x64, // valid note-on
    0x00,
    0xff,
    0x01,
    0x7f, // text meta claims 127 missing payload bytes
  ]);
  const out = Buffer.alloc(22 + body.length);
  out.write('MThd', 0, 'ascii');
  out.writeUInt32BE(6, 4);
  out.writeUInt16BE(0, 8);
  out.writeUInt16BE(1, 10);
  out.writeUInt16BE(480, 12);
  out.write('MTrk', 14, 'ascii');
  out.writeUInt32BE(body.length, 18);
  body.copy(out, 22);
  return out;
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

function accelerando(sampleRate = 22050, startBpm = 100, endBpm = 160, seconds = 24): Float32Array {
  const audio = new Float32Array(Math.round(sampleRate * seconds));
  let t = 0;
  while (t < seconds) {
    const at = Math.round(t * sampleRate);
    for (let i = 0; i < 64 && at + i < audio.length; i++) {
      audio[at + i] += Math.exp(-i / 12) * (i % 2 === 0 ? 1 : -1);
    }
    t += 60 / (startBpm + (endBpm - startBpm) * (t / seconds));
  }
  return audio;
}

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

describe('Project native binding', () => {
  it('reports the expected project ABI version', () => {
    expect(projectAbiVersion()).toBe(EXPECTED_PROJECT_ABI_VERSION);
    expect(projectAbiVersion() ?? 0).toBeGreaterThan(0);
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

  it('refuses a fractional project rate with the C code, on load and on set', () => {
    const current = new Project();
    const parsed = JSON.parse(current.toJson());
    current.destroy();
    const document = JSON.stringify({ ...parsed, sampleRate: 44100.5 });
    // The diagnostic code stays in the diagnostics; the refusal carries the rule alone.
    expect(() => Project.fromJson(document)).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: 'Project.fromJson: sampleRate must be an integer in [8000, 384000]',
      }),
    );
    expect(() => Project.fromJsonWithDiagnostics(document)).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: 'Project.fromJsonWithDiagnostics: sampleRate must be an integer in [8000, 384000]',
      }),
    );
    const project = new Project();
    expect(() => project.setSampleRate(44100.5)).toThrow(
      /sampleRate must be an integer in \[8000, 384000\]/,
    );
    expect(() => project.setSampleRate(44100.5)).toThrow(RangeError);
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
        expect((caught as Error).message).toBe(
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

  it('round-trips toJson -> fromJson -> toJson byte-for-byte', () => {
    const project = buildProject();
    const json = project.toJson();

    const restored = Project.fromJson(json);
    const restoredJson = restored.toJson();

    expect(restoredJson).toBe(json);

    project.destroy();
    restored.destroy();
  });

  it('surfaces compile diagnostic messages per diagnostic', () => {
    const project = buildProject();
    const result = project.compile();

    expect(result).not.toHaveProperty('hasTimeline');
    expect(result).not.toHaveProperty('messages');
    expect(result.diagnostics.length).toBe(1);
    expect(result.diagnostics[0].code).toBe('midiClipNoInstrument');
    expect(result.diagnostics[0].severity).toBe('warning');
    expect(result.diagnostics[0].message).toContain('project contains MIDI clips');

    project.destroy();
  });

  it('bounces deterministically (bit-exact across two calls)', () => {
    const project = buildProject();
    const options = { totalFrames: 1024, blockSize: 128, numChannels: 2 };

    const first = project.bounce(options);
    const second = project.bounce(options);

    expect(first.channels).toHaveLength(2);
    expect(first.channels[0].length).toBe(1024);
    expect(first.channels.map((c) => Array.from(c))).toEqual(
      second.channels.map((c) => Array.from(c)),
    );

    project.destroy();
  });

  it('queries and renders count-in with optional metronome configuration', () => {
    const project = buildProject();
    try {
      project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
      project.setTimeSignatures([{ startPpq: 0, numerator: 4, denominator: 4 }]);
      const countInFrames = project.countInFrames(1);
      expect(countInFrames).toBe(96000);
      expect(() => project.countInFrames(17)).toThrow(/countInBars/);

      const options = {
        totalFrames: countInFrames + 1024,
        blockSize: 128,
        numChannels: 2,
        countInBars: 1,
      };
      const silentPreroll = project.bounce(options);
      const musicOnly = project.bounce({ totalFrames: 1024, blockSize: 128, numChannels: 2 });
      const disabled = project.bounce({ ...options, metronome: { enabled: false } });
      expect(disabled.channels).toEqual(silentPreroll.channels);
      expect(
        silentPreroll.channels.every((channel) =>
          channel.slice(0, countInFrames).every((v) => v === 0),
        ),
      ).toBe(true);
      expect(musicOnly.channels.some((channel) => channel.some((v) => v !== 0))).toBe(true);
      expect(silentPreroll.channels.map((channel) => channel.slice(countInFrames))).toEqual(
        musicOnly.channels,
      );
      const enabled = project.bounce({ ...options, metronome: { enabled: true } });
      const prerollPeak = enabled.channels.reduce((maximum, channel) => {
        for (let i = 0; i < countInFrames; i++) {
          maximum = Math.max(maximum, Math.abs(channel[i]));
        }
        return maximum;
      }, 0);
      expect(prerollPeak).toBeGreaterThan(0);
      expectRefusal(
        () =>
          project.bounce({ ...options, metronome: { enabled: false, clickSample: 4 } as never }),
        TypeError,
        /metronome\.clickSample/,
      );
    } finally {
      project.destroy();
    }
  });

  it('uses one count-in domain and reports an unrepresentable frame result', () => {
    using project = new Project();
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
  });

  it('undo() restores the serialized bytes after an edit', () => {
    const project = buildProject();
    const before = project.toJson();

    const extra = project.addTrack({ kind: 'midi', name: 'extra' });
    expect(extra).toBeGreaterThan(0);
    expect(project.toJson()).not.toBe(before);

    project.undo();
    expect(project.toJson()).toBe(before);

    project.destroy();
  });

  it('setMaxUndoDepth caps how far undo can rewind', () => {
    const project = new Project();
    for (let i = 0; i < 5; i++) {
      project.addTrack({ kind: 'midi', name: `t${i}` });
    }
    expect(project.tracks().length).toBe(5);

    // Keep only the two most-recent edits in history.
    project.setMaxUndoDepth(2);
    project.undo();
    project.undo();
    expect(project.tracks().length).toBe(3);
    // The third undo has no surviving history entry to rewind.
    expect(() => project.undo()).toThrow();

    project.destroy();
  });

  it('setMaxUndoDepth validates input and leaves the installed cap alone', () => {
    const project = new Project();
    // Bound: an unbound method reference loses `this` and fails inside the
    // facade before the addon is ever reached, which would make every rejection
    // below pass for the wrong reason.
    const setDepth = project.setMaxUndoDepth.bind(project) as unknown as (input?: unknown) => void;

    // A cap of one keeps exactly one undoable edit, whatever comes after.
    project.setMaxUndoDepth(1);
    project.addTrack({ kind: 'midi', name: 'a' });
    project.addTrack({ kind: 'midi', name: 'b' });
    expect(project.tracks().length).toBe(2);

    // A raw double -> size_t conversion made each of these SIZE_MAX, which
    // disables the cap instead of rejecting the call.
    const outOfDomain = [
      -1,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      1.5,
      Number.MAX_SAFE_INTEGER + 1,
      Number.MAX_VALUE,
    ];
    for (const value of outOfDomain) {
      expect(() => setDepth(value), `${value}`).toThrow(RangeError);
    }
    for (const value of [undefined, null, '5', {}]) {
      expect(() => setDepth(value), `${String(value)}`).toThrow(TypeError);
    }

    // The depth-one cap installed before the rejections is still in force.
    project.undo();
    expect(project.tracks().length).toBe(1);
    expect(() => project.undo()).toThrow();

    // A valid depth is accepted and rewinds that many edits.
    project.setMaxUndoDepth(5);
    for (let i = 0; i < 3; i++) {
      project.addTrack({ kind: 'midi', name: `t${i}` });
    }
    expect(project.tracks().length).toBe(4);
    project.undo();
    project.undo();
    project.undo();
    expect(project.tracks().length).toBe(1);

    project.destroy();
  });

  it('setMaxHistoryBytes validates input and supports zero retention', () => {
    const project = new Project();
    project.addTrack({ kind: 'midi', name: 'before-validation' });

    const invalidValues: unknown[] = [
      undefined,
      null,
      '64',
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      1.5,
      -1,
      Number.MAX_SAFE_INTEGER + 1,
      Number.MAX_VALUE,
    ];
    for (const value of invalidValues) {
      expect(() =>
        (project.setMaxHistoryBytes as unknown as (input?: unknown) => void)(value),
      ).toThrow();
    }

    // Validation must not disturb an existing undo entry.
    project.undo();
    expect(project.tracks().length).toBe(0);

    // A regular edit remains undoable under a positive byte cap.
    project.setMaxHistoryBytes(Number.MAX_SAFE_INTEGER);
    project.addTrack({ kind: 'midi', name: 'undoable' });
    expect(project.tracks().length).toBe(1);
    project.undo();
    expect(project.tracks().length).toBe(0);

    // Zero is a valid no-retention policy: mutation succeeds, but has no undo.
    project.setMaxHistoryBytes(0);
    project.addTrack({ kind: 'midi', name: 'not-undoable' });
    expect(project.tracks().length).toBe(1);
    expect(() => project.undo()).toThrow();
    expect(project.tracks().length).toBe(1);

    project.destroy();
  });

  it('clearHistory leaves nothing to undo', () => {
    const project = new Project();
    project.addTrack({ kind: 'midi', name: 'a' });
    project.addTrack({ kind: 'midi', name: 'b' });
    expect(project.tracks().length).toBe(2);

    project.clearHistory();
    expect(project.tracks().length).toBe(2);
    expect(() => project.undo()).toThrow();

    project.destroy();
  });

  it('addMidiClip undoes and redoes track, source, and clip as one edit', () => {
    const project = new Project();
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
    project.destroy();
  });

  it('routes a track to a MIDI destination and undoes it', () => {
    const project = new Project();
    const trackId = project.addTrack({ kind: 'midi', name: 'lead' });
    const before = project.toJson();

    project.setTrackMidiDestination(trackId, 7);
    const after = project.toJson();
    expect(after).not.toBe(before);
    expect(after).toContain('"midiDestinationId":7');

    project.undo();
    expect(project.toJson()).toBe(before);

    project.destroy();
  });

  it('sets track gain / mute / solo / pan and undoes them', () => {
    const project = new Project();
    const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
    const before = project.toJson();

    project.setTrackGain(trackId, 0.5);
    expect(project.toJson()).toContain('"gain":0.5');
    project.setTrackMute(trackId, true);
    expect(project.toJson()).toContain('"mute":true');
    project.setTrackSolo(trackId, true);
    expect(project.toJson()).toContain('"solo":true');
    project.setTrackPan(trackId, -0.25);
    expect(project.toJson()).toContain('"pan":-0.25');

    project.undo();
    project.undo();
    project.undo();
    project.undo();
    expect(project.toJson()).toBe(before);

    expect(() => project.setTrackGain(9999, 1)).toThrow();
    expect(() => project.setTrackPan(9999, 0)).toThrow();

    project.destroy();
  });

  it('sets a clip warp reference and undoes it', () => {
    const project = new Project();
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

    project.destroy();
  });

  it('refuses a warp mode ordinal or a retired spelling in a document by its tokens', () => {
    const project = new Project();
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
    const document = project.toJson();
    project.destroy();
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
    project.destroy();
  });

  it('rejects routing an unknown track', () => {
    const project = new Project();
    expect(() => project.setTrackMidiDestination(9999, 1)).toThrow();
    project.destroy();
  });

  it('exports MIDI to an SMF Buffer', () => {
    const project = buildProject();
    const smf = project.exportSmf();
    expect(smf.length).toBeGreaterThan(0);
    // Standard MIDI File header chunk magic.
    expect(bytes(smf.subarray(0, 4)).toString('ascii')).toBe('MThd');
    project.destroy();
  });

  it('rejects MIDI loop export that exceeds the expansion budget', () => {
    const project = new Project();
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    ]);
    project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 1e-9 });
    expect(() => project.exportSmf()).toThrow();
    expect(() => project.exportClipFile()).toThrow();
    project.destroy();
  });

  it('round-trips a MIDI 2.0 Clip File through the binding', () => {
    const project = buildProject();
    const { clipId } = project.addMidiClip(0, 4);
    // A MIDI 2.0 note-on (message type 0x4) with a full 16-bit velocity.
    project.setMidiEvents(clipId, [
      { ppq: 0, word0: 0x40903c00, word1: 0xbeef0000 },
      { ppq: 1, word0: 0x40803c00, word1: 0 },
    ]);

    const clipFile = project.exportClipFile();
    expect(clipFile.length).toBeGreaterThan(8);
    // MIDI 2.0 Clip File header magic.
    expect(bytes(clipFile.subarray(0, 8)).toString('ascii')).toBe('SMF2CLIP');

    const reimported = new Project();
    const firstClip = reimported.importClipFile(clipFile);
    expect(firstClip).toBeGreaterThan(0);
    expect(bytes(reimported.exportClipFile().subarray(0, 8)).toString('ascii')).toBe('SMF2CLIP');
    reimported.destroy();
    project.destroy();
  });

  it('reports MIDI-FX provenance and matches the bare bake', () => {
    const seed = (project: Project, clip: number) => {
      project.setMidiEvents(clip, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
        midiNoteOn({ ppq: 2, group: 0, channel: 0, note: 64, velocity: 100 }),
        midiNoteOff({ ppq: 3, group: 0, channel: 0, note: 64, velocity: 0 }),
      ]);
    };
    const config = { chordIntervals: [0, 4, 7] };

    const request = buildProject();
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

    // The request without the flag must land on identical bytes.
    const bare = buildProject();
    const { clipId: bareClip } = bare.addMidiClip(0, 4);
    seed(bare, bareClip);
    expect(bare.bakeMidiFx({ clipId: bareClip, config })).toEqual({});
    expect(bare.exportSmf()).toEqual(request.exportSmf());

    // The positional form is gone.
    expect(() =>
      (bare as unknown as { bakeMidiFx(...args: unknown[]): unknown }).bakeMidiFx(bareClip, config),
    ).toThrow(TypeError);

    // Errors travel the same path with and without the flag.
    expectEmptyChord(
      () => bare.bakeMidiFx({ clipId: bareClip, config: { chordIntervals: [] } }),
      'bakeMidiFx',
    );
    expectEmptyChord(
      () =>
        request.bakeMidiFx({
          clipId: requestClip,
          config: { chordIntervals: [] },
          withSourceIndex: true,
        }),
      'bakeMidiFx',
    );
    expectEmptyChord(
      () => request.previewMidiFxCount({ clipId: requestClip, config: { chordIntervals: [] } }),
      'previewMidiFxCount',
    );

    request.destroy();
    bare.destroy();
  });

  it('applies program changes, MIDI FX, and preserves imported SysEx payloads', () => {
    const project = buildProject();
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
    expect(bytes(smf).includes(Buffer.from([0xc0, 42]))).toBe(true);
    expect(bytes(smf).includes(Buffer.from([0xc3, 24]))).toBe(true);
    // Pressure and bend survive the export. Poly pressure carries the transposed
    // note (60 + 12), because aftertouch left on the source note would address a
    // note the shifted voice never played.
    expect(bytes(smf).includes(Buffer.from([0xa0, 72, 70]))).toBe(true);
    expect(bytes(smf).includes(Buffer.from([0xa0, 60, 70]))).toBe(false);
    expect(bytes(smf).includes(Buffer.from([0xd0, 80]))).toBe(true);
    expect(bytes(smf).includes(Buffer.from([0xe0, 0x00, 0x40]))).toBe(true);
    project.destroy();

    const sysexProject = new Project();
    const firstClip = sysexProject.importSmf(makeSysexSmf());
    expect(firstClip).toBeGreaterThan(0);
    const json = sysexProject.toJson();
    expect(json).toContain('__sysexPayloads');
    const restored = Project.fromJson(json);
    const restoredSmf = restored.exportSmf();
    expect(
      bytes(restoredSmf).includes(Buffer.from([0xf0, 0x05, 0x7e, 0x7f, 0x09, 0x01, 0xf7])),
    ).toBe(true);
    sysexProject.destroy();
    restored.destroy();
  });

  it('reads, copies, and writes MIDI event records with SysEx bytes', () => {
    const project = new Project();
    try {
      const firstClip = project.addMidiClip(0, 2).clipId;
      const secondClip = project.addMidiClip(0, 2).clipId;
      const payload = Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]);
      project.setMidiEvents(firstClip, [
        { ppq: 0.75, word0: 0x30000000, word1: 0, sysex: payload },
        { ppq: 0.25, word0: 0x20903c64, sysex: null } as unknown as ProjectMidiEvent,
        { ppq: 0.5, word0: 0x40903c00, word1: 0xc0000000 },
      ]);
      payload.fill(0);

      const read = project.midiEvents(firstClip);
      expect(read).toEqual([
        { ppq: 0.25, word0: 0x20903c64, word1: 0 },
        { ppq: 0.5, word0: 0x40903c00, word1: 0xc0000000 },
        {
          ppq: 0.75,
          word0: 0x30000000,
          word1: 0,
          sysex: Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]),
        },
      ]);
      expect(Object.hasOwn(read[0], 'sysex')).toBe(false);

      read[2].sysex?.fill(0);
      const freshRead = project.midiEvents(firstClip);
      expect(freshRead[2].sysex).toEqual(Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]));
      project.setMidiEvents(secondClip, freshRead);
      expect(project.midiEvents(secondClip)).toEqual(freshRead);
    } finally {
      project.destroy();
    }
  });

  it('preserves an explicitly empty SysEx view for the C setter to refuse', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 1);
      expectRefusal(
        () =>
          project.setMidiEvents(clipId, [{ ppq: 0, word0: 0x20903c64, sysex: new Uint8Array(0) }]),
        RangeError,
      );
    } finally {
      project.destroy();
    }
  });

  it('setProgram takes bank, group and channel as options', () => {
    const exportWith = (call: (project: Project, clipId: number) => void): Buffer => {
      const project = new Project();
      try {
        const { clipId } = project.addMidiClip(0, 2);
        call(project, clipId);
        return bytes(project.exportSmf());
      } finally {
        project.destroy();
      }
    };

    // No options: a channel-0 program change and no Bank Select.
    const plain = exportWith((project, clipId) => project.setProgram(clipId, 42));
    expect(plain.includes(Buffer.from([0xc0, 42]))).toBe(true);
    expect(plain.includes(Buffer.from([0xb0, 0x00]))).toBe(false);
    expect(plain.includes(Buffer.from([0xb0, 0x20]))).toBe(false);

    // A bank emits Bank Select MSB and LSB.
    const banked = exportWith((project, clipId) =>
      project.setProgram(clipId, 42, { bank: { msb: 2, lsb: 0x23 } }),
    );
    expect(banked.includes(Buffer.from([0xb0, 0x00, 2]))).toBe(true);
    expect(banked.includes(Buffer.from([0xb0, 0x20, 0x23]))).toBe(true);
    expect(banked.includes(Buffer.from([0xc0, 42]))).toBe(true);

    // Group and channel route the program change to that channel.
    const routed = exportWith((project, clipId) =>
      project.setProgram(clipId, 24, { group: 0, channel: 5 }),
    );
    expect(routed.includes(Buffer.from([0xc5, 24]))).toBe(true);
    expect(routed.includes(Buffer.from([0xc0, 24]))).toBe(false);

    // The former positional bank is refused.
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 2);
      expect(() => project.setProgram(clipId, 24, 1 as never)).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('preserves more than 512 events when baking MIDI FX', () => {
    const project = new Project();
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

    const round = new Project();
    const roundClip = round.importSmf(project.exportSmf());
    round.bakeMidiFx({ clipId: roundClip, config: {} });
    const roundPayload = JSON.parse(round.toJson()) as {
      midiContent: Record<string, Array<{ ppq: number; word0: number; word1: number }>>;
    };
    expect(roundPayload.midiContent[String(roundClip)]).toHaveLength(600);
    round.destroy();
    project.destroy();
  });

  it('imports the valid prefix of a truncated SMF', () => {
    const project = new Project();
    expect(project.importSmf(makeTruncatedSmf())).toBeGreaterThan(0);
    project.destroy();
  });

  it('validates MIDI event inputs before native conversion', () => {
    const project = new Project();
    const { clipId } = project.addMidiClip(0, 1);

    expect(() => project.setMidiEvents(clipId, [{ ppq: Number.NaN, word0: 0, word1: 0 }])).toThrow(
      /ppq/,
    );
    expect(() => project.setMidiEvents(clipId, [{ ppq: 0, word0: -1, word1: 0 }])).toThrow(/word0/);

    project.destroy();
  });

  it('refuses a tuple event and the data0/data1 keys', () => {
    const project = new Project();
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

    project.destroy();
  });

  it.each(['fromJson', 'fromJsonWithDiagnostics'] as const)(
    '%s keeps all document diagnostics and the literal refusal path',
    (entry) => {
      const document = JSON.parse(danglingSourceJson());
      document.scene = {
        version: 2,
        strips: [{ id: 'lead', faderDB: -3 }],
        buses: [{ id: 'master', role: 'master' }],
      };
      let caught: unknown;
      try {
        const loaded = Project[entry](JSON.stringify(document));
        ('project' in loaded ? loaded.project : loaded).destroy();
      } catch (error) {
        caught = error;
      }
      const diagnostic = `clip ${document.clips[0].id} references missing source 99`;
      const subject = 'scene.strips[0].faderDB';
      const refusal = unknownKeyText(`unknown key ${subject}`);
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({
        code: ErrorCode.InvalidFormat,
        message: expect.stringMatching(
          new RegExp(`^Project\\.${entry}: ${diagnostic}\n${refusal.source}$`),
        ),
      });
      expect(refusalOf(caught)).toMatchObject({
        cEntry: 'project_deserialize',
        code: ErrorCode.InvalidFormat,
        form: 'unknownKey',
        subjects: [subject],
        text: expect.stringMatching(new RegExp(`^${diagnostic}\n${refusal.source}$`)),
      });
    },
  );

  it('throws cleanly on malformed fromJson input', () => {
    expect(() => Project.fromJson('{ not valid project json')).toThrow();
  });

  it('fromJsonWithDiagnostics returns warnings from successful loads', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(danglingSourceJson());
    expect(diagnostics).toContain('dangling_clip_source');
    expect(project.tracks().length).toBe(1);
    project.destroy();
  });

  /** A MIDI-only project: one MIDI clip holding a sustained note routed to dest 0. */
  function buildMidiOnlyProject(): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    return project;
  }

  it('bounces a MIDI-only project to silence without a bound instrument', () => {
    const project = buildMidiOnlyProject();
    const audio = project.bounce({ totalFrames: 4096, numChannels: 2 });
    expect(audio.channels).toHaveLength(2);
    expect(audio.channels[0]).toHaveLength(4096);
    expect(peak(audio)).toBe(0);
    project.destroy();
  });

  it('bounces at the width of the scene master when numChannels is omitted', () => {
    const cases = [
      [undefined, 2],
      ['mono', 1],
      ['stereo', 2],
      ['5.1', 6],
      ['7.1', 8],
    ] as const;
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

  it('bounces MIDI through the built-in synth to non-silent audio (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    const audio = project.bounce({
      totalFrames: 4096,
      numChannels: 2,
      builtinInstruments: [{ destinationId: 0, instrument: { waveform: 'saw', gain: 0.5 } }],
    });
    expect(audio.channels).toHaveLength(2);
    expect(audio.channels[0]).toHaveLength(4096);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('applies controller profiles and articulations from bounce options', () => {
    const project = buildMidiOnlyProject();
    try {
      project.setMidiEvents(project.clips()[0].id, [
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
        midiCc({ ppq: 0.125, group: 0, channel: 0, controller: 2, value: 127 }),
        midiNoteOn({ ppq: 0.25, group: 0, channel: 0, note: 69, velocity: 100 }),
        midiCc({ ppq: 0.375, group: 0, channel: 0, controller: 2, value: 32 }),
        midiNoteOff({ ppq: 0.75, group: 0, channel: 0, note: 60, velocity: 0 }),
        midiNoteOff({ ppq: 0.875, group: 0, channel: 0, note: 69, velocity: 0 }),
      ]);
      const options = {
        totalFrames: 24000,
        numChannels: 2,
        synthInstruments: [{ instrument: { engineMode: 'reed' as const } }],
      };
      const plain = project.bounce(options);
      const profiled = project.bounce({
        ...options,
        controllerProfiles: [{ destinationId: 0, presetName: 'breath' }],
      });
      const articulated = project.bounce({
        ...options,
        controllerProfiles: [{ destinationId: 0, presetName: 'breath' }],
        articulations: [{ destinationId: 0, channel: 0, articulation: 'monoLegato' }],
      });
      const maximumDifference = (left: Float32Array[], right: Float32Array[]): number => {
        let maximum = 0;
        for (let channel = 0; channel < left.length; channel++) {
          for (let frame = 0; frame < left[channel].length; frame++) {
            maximum = Math.max(maximum, Math.abs(left[channel][frame] - right[channel][frame]));
          }
        }
        return maximum;
      };
      expect(maximumDifference(plain.channels, profiled.channels)).toBeGreaterThan(1e-3);
      expect(maximumDifference(profiled.channels, articulated.channels)).toBeGreaterThan(1e-3);
    } finally {
      project.destroy();
    }
  });

  it('refuses builtin synth numeric fields that are explicitly zero', () => {
    const project = buildMidiOnlyProject();
    const options = { totalFrames: 4096, numChannels: 2 };
    // This facade binds the minimal oscillator synth. The NativeSynth patch
    // vocabulary (preset, ampSustain, filterSustain) is not read here, so only
    // the oscillator fields are exercised.
    const base = project.bounce({
      ...options,
      builtinInstruments: [{ instrument: { waveform: 'saw' } }],
    });
    for (const field of ['gain', 'attackMs', 'decayMs', 'sustain', 'releaseMs']) {
      expect(() =>
        project.bounce({
          ...options,
          builtinInstruments: [{ instrument: { waveform: 'saw', [field]: 0 } }],
        }),
      ).toThrow(RangeError);
    }
    expect(() =>
      project.bounce({
        ...options,
        builtinInstruments: [{ instrument: { waveform: 'saw', polyphony: 0 } }],
      }),
    ).toThrow(/polyphony must be an integer in \[1, 64\]/);
    // Guard against a vacuous pass: a non-default value must move the render,
    // otherwise the refusals above could hold for an unrelated reason.
    const moved = project.bounce({
      ...options,
      builtinInstruments: [{ instrument: { waveform: 'saw', sustain: 0.1 } }],
    });
    expect(moved).not.toEqual(base);
    project.destroy();
  });

  it('accepts a waveform alias in a binding and an explicit bindings array', () => {
    const project = buildMidiOnlyProject();
    const byName = project.bounce({
      totalFrames: 2048,
      numChannels: 1,
      builtinInstruments: [{ instrument: { waveform: 'sine' } }],
    });
    expect(peak(byName)).toBeGreaterThan(0);

    const byAlias = project.bounce({
      totalFrames: 2048,
      numChannels: 1,
      builtinInstruments: [{ instrument: { waveform: 'sawtooth' } }],
    });
    expect(peak(byAlias)).toBeGreaterThan(0);

    const byArray = project.bounce({
      totalFrames: 2048,
      numChannels: 1,
      builtinInstruments: [{ destinationId: 0, instrument: { waveform: 'square' } }],
    });
    expect(peak(byArray)).toBeGreaterThan(0);

    // A bare waveform name or a bare config is not a binding.
    const loose = project as unknown as {
      bounce(...args: unknown[]): unknown;
    };
    expect(() => loose.bounce({ builtinInstruments: 'sine' })).toThrow(TypeError);
    expect(() => loose.bounce({ builtinInstruments: [{ waveform: 'sine' }] })).toThrow(TypeError);
    project.destroy();
  });

  it('auto-derives the render length when options/totalFrames is omitted (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    // Pass only the instrument; options defaults to {} and length auto-derives.
    const audio = project.bounce({
      builtinInstruments: [{ instrument: { waveform: 'triangle' } }],
    });
    expect(audio.channels[0].length).toBeGreaterThan(0);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('accepts a patch typed with the shared BuiltinSynthConfig alias', () => {
    const project = buildMidiOnlyProject();
    // The Python binding names this concept BuiltinSynthConfig; the Node alias
    // lets portable code share that name (tsc validates the alias accepts the
    // same shape as BuiltinInstrumentConfig).
    const patch: BuiltinSynthConfig = { waveform: 'square', gain: 0.3 };
    const audio = project.bounce({ builtinInstruments: [{ instrument: patch }] });
    expect(audio.channels[0].length).toBeGreaterThan(0);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('rejects an unknown built-in synth waveform name', () => {
    const project = buildMidiOnlyProject();
    expect(() =>
      project.bounce({
        builtinInstruments: [
          {
            instrument: { waveform: 'noise' as unknown as 'sine' },
          },
        ],
      }),
    ).toThrow(/sawtooth/);
    project.destroy();
  });

  // The facade takes a waveform by name only and refuses every number as a
  // TypeError; the native resolver behind it still reads an ordinal, so its
  // domain checks are driven on the handle directly.
  const nativeBuiltinBounce = (project: Project, waveform: unknown): Float32Array =>
    (
      project as unknown as {
        handle: {
          bounce(settings: object, builtin: unknown[]): { interleaved: Float32Array };
        };
      }
    ).handle.bounce({}, [{ waveform }]).interleaved;

  it('refuses a numeric waveform at the facade, in range or not', () => {
    for (const waveform of [0, 3, 4, -1, Number.NaN]) {
      const project = buildMidiOnlyProject();
      expect(() =>
        project.bounce({
          builtinInstruments: [
            {
              instrument: { waveform: waveform as unknown as 'sine' },
            },
          ],
        }),
      ).toThrow(new TypeError('Project.bounce: waveform must be a name'));
      project.destroy();
    }
  });

  it('natively rejects a numeric waveform outside the enum, in the same words as a name', () => {
    // The numeric spelling used to fall through unchecked and land on sine, and
    // the first value past a four-value enum is 4 - what an off-by-one or a
    // 1-based mirror emits - so the silent fallback sat immediately beside the
    // valid range. -1 is the sentinel a generated binding reaches for. A test
    // driving only large values passes against a fix that starts checking at 5.
    for (const waveform of [4, -1, 5]) {
      const project = buildMidiOnlyProject();
      expect(
        () => nativeBuiltinBounce(project, waveform),
        `waveform ${waveform} must be rejected, not resolved to sine`,
      ).toThrow(/sawtooth/);
      project.destroy();
    }
  });

  it('natively rejects a waveform outside the integer range before naming the enum', () => {
    // A value the int cannot hold is refused as out of range, not as an unknown
    // ordinal. Asserting the enum wording here would be asserting a lie: the
    // ordinal in that message would be the wrapped -2147483648, a number the
    // caller never wrote. WASM answers in these same words.
    for (const waveform of [2 ** 31, -(2 ** 31) - 1, Number.POSITIVE_INFINITY, Number.NaN]) {
      const project = buildMidiOnlyProject();
      expect(
        () => nativeBuiltinBounce(project, waveform),
        `waveform ${waveform} must be rejected as out of range`,
      ).toThrow(/must be an integer in \[-2147483648, 2147483647\]/);
      project.destroy();
    }
  });

  it('natively accepts every in-domain waveform ordinal and they stay distinct', () => {
    // Vacuity guard for the rejection above: a fix that rejected everything
    // would satisfy it. Four ordinals, four different renders.
    const renders = [0, 1, 2, 3].map((waveform) => {
      const project = buildMidiOnlyProject();
      const audio = nativeBuiltinBounce(project, waveform);
      project.destroy();
      return audio.reduce((hash, sample) => (hash * 31 + Math.round(sample * 1e6)) | 0, 0);
    });
    expect(new Set(renders).size, 'the four waveforms must render differently').toBe(4);
  });
});

describe('Project value-model accessors', () => {
  it('round-trips track and source counts', () => {
    const project = new Project();
    expect(project.tracks().length).toBe(0);
    expect(project.clips().length).toBe(0);
    project.addTrack({ kind: 'audio', name: 'lead' });
    expect(project.tracks().length).toBe(1);
    const audio = new Float32Array(64).fill(0.1);
    const clipId = project.addClip({
      trackId: 1,
      startPpq: 0,
      lengthPpq: 4,
      channels: [audio],
      sampleRate: 48000,
    });
    expect(project.clips().length).toBe(1);
    expect(project.sources().length).toBeGreaterThanOrEqual(1);
    const restored = Project.fromJson(project.toJson());
    expect(restored.clips().length).toBe(1);
    restored.destroy();
    project.removeClip(clipId);
    expect(project.clips().length).toBe(0);
    project.destroy();
  });

  it('reads and sets the sample rate', () => {
    const project = new Project();
    project.setSampleRate(44100);
    expect(project.sampleRate).toBe(44100);
    project.destroy();
  });

  it('reads and sets the overlap policy', () => {
    const project = new Project();
    project.setOverlapPolicy('allow');
    expect(project.overlapPolicy).toBe('allow');
    project.setOverlapPolicy('disallow');
    expect(project.overlapPolicy).toBe('disallow');
    project.destroy();
  });

  it('replaces tempo segments and counts them', () => {
    const project = new Project();
    project.setTempoSegments([
      { startPpq: 0, bpm: 120 },
      { startPpq: 4, bpm: 140, endBpm: 160 },
    ]);
    expect(project.tempoSegments().length).toBe(2);
    project.setTempoSegments([]);
    expect(project.tempoSegments()).toEqual([]);
    project.destroy();
  });

  it('reads tempo segments back in the shape the setter takes', () => {
    // The count is only usable alongside a way to read what it counts, and a
    // segment that reads back in the setter's own shape can be moved between
    // projects without a translation step.
    const source = new Project();
    const segments = [
      { startPpq: 0, bpm: 120, endBpm: 0 },
      { startPpq: 4, bpm: 140, endBpm: 160 },
    ];
    source.setTempoSegments(segments);
    const readBack = source.tempoSegments();
    expect(readBack).toEqual(segments);
    // startSample is never returned: the setter ignores it and a project keeps
    // musical positions only.
    expect('startSample' in readBack[0]).toBe(false);
    expect(readBack).toHaveLength(2);

    const target = new Project();
    target.setTempoSegments(readBack);
    expect(target.tempoSegments()[1]).toEqual(readBack[1]);
    source.destroy();
    target.destroy();
  });

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

  it('replaces time signatures and counts them', () => {
    const project = new Project();
    project.setTimeSignatures([
      { startPpq: 0, numerator: 4, denominator: 4 },
      { startPpq: 8, numerator: 3, denominator: 4 },
    ]);
    expect(project.timeSignatures().length).toBe(2);
    expect(project.timeSignatures()[1]).toEqual({
      startPpq: 8,
      numerator: 3,
      denominator: 4,
    });
    expect(project.timeSignatures()[2]).toBeUndefined();
    project.setTimeSignatures([]);
    expect(project.timeSignatures()).toEqual([]);
    project.destroy();
  });

  it('reads effective tempo, seconds, meter, bar, and beat positions', () => {
    const project = new Project();
    try {
      expect(project.ppqToSec(2)).toBe(1);
      expect(project.secToPpq(1)).toBe(2);
      expect(project.positionAtPpq(5.5)).toEqual({
        bpm: 120,
        timeSignature: { startPpq: 0, numerator: 4, denominator: 4 },
        barStartPpq: 4,
        barIndex: 1,
        beat: 2,
        beatFraction: 0.5,
      });
      expect(project.barStartPpq(1)).toBe(4);
    } finally {
      project.destroy();
    }
  });

  it('reads the end PPQ of the last audible clip', () => {
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

  it('routes effective-time reader domain refusals through the C API', () => {
    const project = new Project();
    try {
      expectRefusal(() => project.ppqToSec(-1), RangeError, /ppq/);
      expectRefusal(() => project.secToPpq(-1), RangeError, /sec/);
      expectRefusal(() => project.positionAtPpq(-1), RangeError, /ppq/);
      expectRefusal(() => project.barStartPpq(-1), RangeError, /barIndex/);
      expectRefusal(() => project.midiEvents(0xffffffff), RangeError, /clipId/);
      // Past the safe-integer range the index is refused in the core's domain terms, as every
      // surface states it, not in this surface's number width.
      expectRefusal(
        () => project.barStartPpq(1.5),
        TypeError,
        /^Project\.barStartPpq: barIndex must be an integer$/,
      );
      expectRefusal(
        () => project.barStartPpq(Number.MAX_SAFE_INTEGER + 1),
        RangeError,
        /^Project\.barStartPpq: barIndex must identify a bar with a representable PPQ start in \[0, 1000000000000\]$/,
      );
      expectRefusal(
        () => project.barStartPpq(-Number.MAX_SAFE_INTEGER - 2),
        RangeError,
        /^Project\.barStartPpq: barIndex must be an integer of at least 0$/,
      );
    } finally {
      project.destroy();
    }
  });

  it('adds a marker and returns its id', () => {
    const project = new Project();
    const id = project.setMarker({ ppq: 1.5, name: 'verse' });
    expect(typeof id).toBe('number');
    expect(id).toBeGreaterThan(0);
    project.destroy();
  });

  it('round-trips a structured marker through setMarker / markers', () => {
    const project = new Project();
    const id = project.setMarker({
      ppq: 2,
      name: 'E minor',
      kind: 'keySignature',
      keyFifths: 1,
      keyMinor: true,
    });
    expect(id).toBeGreaterThan(0);
    const marker = project.markers()[0];
    expect(marker.id).toBe(id);
    expect(marker.ppq).toBe(2);
    expect(marker.name).toBe('E minor');
    expect(marker.kind).toBe('keySignature');
    expect(marker.keyFifths).toBe(1);
    expect(marker.keyMinor).toBe(true);

    // A second structured marker; markers() lists them.
    project.setMarker({ ppq: 5, name: 'chorus' });
    expect(project.markers().length).toBe(2);
    const names: string[] = [];
    for (const m of project.markers()) {
      expect(typeof m.id).toBe('number');
      names.push(m.name ?? '');
    }
    expect(names).toContain('E minor');
    expect(names).toContain('chorus');

    // A plain setMarker keeps the default Marker kind with neutral key fields.
    const plain = project.markers()[1];
    expect(plain.kind).toBe('marker');
    expect(plain.keyFifths).toBe(0);
    expect(plain.keyMinor).toBe(false);
    project.destroy();
  });

  it('round-trips a long UTF-8 marker name without splitting or truncating it', () => {
    const project = new Project();
    const name = 'あいうえお'.repeat(7);
    expect(Array.from(name).length).toBeGreaterThan(30);
    project.setMarker({ ppq: 3, name });
    expect(project.markers()[0].name).toBe(name);
    expect(JSON.parse(project.toJson()).markers[0].name).toBe(name);
    project.destroy();
  });

  it('accepts and clears a mixer scene', () => {
    const project = new Project();
    project.setMixerScene({});
    expect(project.mixerScene()?.version).toBe(2);
    project.setMixerScene();
    expect(project.mixerScene()).toBeUndefined();
    project.destroy();
  });

  it('control: has no last successful result before a bounce and retains it after a failed bounce', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      expect(project.lastBounceCompileResult()).toBeUndefined();
      project.bounce({ totalFrames: 256 });
      const lastSuccess = project.lastBounceCompileResult();
      expect(lastSuccess).toEqual({ diagnostics: [] });

      const trackId = project.addTrack({ kind: 'audio' });
      // A clip whose audio source was never supplied cannot be compiled.
      project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://missing.wav' });
      expectRefusal(
        () => project.bounce({ totalFrames: 256 }),
        ErrorCode.InvalidState,
        /^Project\.bounce: the project does not compile: audio source has no decoded samples registered in the AudioContentStore$/,
      );
      expect(project.lastBounceCompileResult()).toEqual(lastSuccess);
    } finally {
      project.destroy();
    }
  });

  it('control: retains the last successful result after a later argument refusal', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      project.bounce({ totalFrames: 256 });
      const lastSuccess = project.lastBounceCompileResult();
      expect(lastSuccess).toEqual({ diagnostics: [] });
      expect(() => project.bounce({ totalFrames: 256, countInBars: 17 })).toThrow(
        new RangeError('Project.bounce: countInBars must be an integer in [0, 16]'),
      );
      expect(project.lastBounceCompileResult()).toEqual(lastSuccess);
    } finally {
      project.destroy();
    }
  });
});

describe('Project validateMidiNotes', () => {
  it('reports fully paired note-on/note-off as ok', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(true);
    expect(result.unmatchedNoteOns).toBe(0);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });

  it('flags a hanging note-on as not ok', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(false);
    expect(result.unmatchedNoteOns).toBe(1);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });

  it('keeps note pairing separate across UMP groups', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 1, group: 1, channel: 0, note: 60, velocity: 0 }),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(false);
    expect(result.unmatchedNoteOns).toBe(1);
    expect(result.unmatchedNoteOffs).toBe(1);
    project.destroy();
  });

  it('closes a note still sounding at the half-open clip end', () => {
    const project = new Project();
    const { clipId } = project.addMidiClip(0, 1);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(true);
    expect(result.unmatchedNoteOns).toBe(0);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });
});

// Golden vectors for the hand-written UMP MIDI-1.0 channel-voice packing
// (midiNoteOn/Off/Cc/... build word0 in TS rather than via the native
// packer). These exact words are what sonare::midi::make_midi1_* — and thus the
// C-ABI sonare_midi_* packers Python delegates to — produce, so pinning them
// here (with the identical vectors mirrored in the WASM suite) makes any silent
// drift between the two hand-written JS copies a CI failure.
describe('UMP MIDI-1.0 packing matches the canonical word layout', () => {
  it('packs channel-voice messages into the spec word', () => {
    expect(midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }).word0 >>> 0).toBe(
      0x20903c64,
    );
    expect(midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }).word0 >>> 0).toBe(
      0x20803c00,
    );
    expect(midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 127 }).word0 >>> 0).toBe(
      0x20b0077f,
    );
    expect(
      midiPolyPressure({ ppq: 0, group: 0, channel: 0, note: 60, pressure: 64 }).word0 >>> 0,
    ).toBe(0x20a03c40);
    expect(midiProgram({ ppq: 0, group: 0, channel: 0, program: 5 }).word0 >>> 0).toBe(0x20c00500);
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
    expect(() => gmInstrumentName(128)).toThrow(RangeError);
    expect(gmProgramForName('Violin')).toBe(40);
    expect(() => gmProgramForName('No Such Instrument')).toThrow(
      new RangeError('gmProgramForName: name must be a GM instrument name'),
    );
    expect(gmFamilyName(4)).toBe('Bass');
    expect(gmFamilyFirstProgram(4)).toBe(32);
    expect(gm2InstrumentName({ bankLsb: 1, program: 24 })).toBe('Ukulele');
    expect(gmDrumName(38)).toBe('Acoustic Snare');
    // An in-range number that names nothing in the table is absence, not a refusal.
    expect(gmDrumName(0)).toBeUndefined();
    expect(gm2DrumSetName(1)).toBeUndefined();
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
    expect(events.map((event) => event.word0 >>> 0)).toEqual([0x20b30079, 0x20b32001, 0x20c31800]);
    expect(events.every((event) => event.ppq === 0 && event.word1 === 0)).toBe(true);
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

  it('routes SysEx payloads with owned copies and leaves absent fields absent', () => {
    const payload = Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]);
    const routed = midiRouteEvents({
      events: [
        { ppq: 0.25, word0: 0x30000000, word1: 0, sysex: payload },
        { ppq: 0.5, word0: 0x20903c64 },
      ],
    });
    payload.fill(0);
    expect(routed.events[0]).toEqual({
      ppq: 0.25,
      word0: 0x30000000,
      word1: 0,
      sysex: Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]),
    });
    expect(Object.hasOwn(routed.events[1], 'sysex')).toBe(false);
    routed.events[0].sysex?.fill(0);
    const secondPayload = Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]);
    const second = midiRouteEvents({
      events: [{ ppq: 0, word0: 0x30000000, word1: 0, sysex: secondPayload }],
    });
    second.events[0].sysex?.fill(0);
    expect(secondPayload).toEqual(Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]));
    const signedWord = midiRouteEvents({
      events: [{ ppq: 0, word0: 0x40903c00, word1: -1 }],
    });
    expect(signedWord.events[0].word1).toBe(0xffff_ffff);
    expectRefusal(
      () =>
        midiRouteEvents({
          events: [{ ppq: 0, word0: 0x30000000, word1: 0, sysex: new Uint8Array(0) }],
        }),
      RangeError,
      /events\[0\]\.sysex/,
    );
    expectRefusal(
      () =>
        midiRouteEvents({
          events: [{ ppq: 0, word0: 0x30000000, word1: 0, sysex: [0xf0] as unknown as Uint8Array }],
        }),
      TypeError,
      /events\[0\]\.sysex must be a Uint8Array/,
    );
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
    if (!learned14) {
      throw new Error('expected the two controllers to produce a binding');
    }
    expect(Object.hasOwn(learned14, 'kind')).toBe(true);
    expect(typeof learned14.kind).toBe('string');
    expect(learned14).toMatchObject({
      kind: 'controlChange14',
      ccNumber: 1,
      ccLsbNumber: 33,
      channel: 2,
      paramId: 77,
      minValue: -1,
      maxValue: 1,
      consume: false,
    });

    const learnedRpn = midiCcLearn({
      events: [
        midiCc({ ppq: 0, group: 0, channel: 3, controller: 101, value: 0 }),
        midiCc({ ppq: 0.1, group: 0, channel: 3, controller: 100, value: 1 }),
        midiCc({ ppq: 0.2, group: 0, channel: 3, controller: 6, value: 64 }),
      ],
      paramId: 78,
    });
    expect(learnedRpn).toMatchObject({ kind: 'rpn', selectorMsb: 0, selectorLsb: 1, paramId: 78 });

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

  it('follows a tempo that moves only when asked to', () => {
    // Without adaptiveTempo the tracker fits one tempo to the whole take, so the
    // map it produces describes the take's average rather than its shape.
    const audio = accelerando();
    const segmentsFor = (options?: Record<string, unknown>) => {
      const project = new Project();
      try {
        project.autoTempo(audio, 22050, options);
        return project.tempoSegments();
      } finally {
        project.destroy();
      }
    };

    const fixed = segmentsFor();
    const adaptive = segmentsFor({ adaptiveTempo: true });
    expect(adaptive.length).toBeGreaterThan(fixed.length);

    // The reported tempo has to span the sweep, not sit on its average.
    const bpms = adaptive.map((segment) => segment.bpm);
    expect(Math.max(...bpms) - Math.min(...bpms)).toBeGreaterThan(20);

    // A coarser ramp threshold merges more of the take into constant stretches.
    const coarse = segmentsFor({ adaptiveTempo: true, rampThreshold: 0.2 });
    expect(coarse.length).toBeLessThan(adaptive.length);

    // Passing nothing must not quietly enable anything.
    expect(segmentsFor({})).toEqual(fixed);
  });

  it('rejects a tempo option value the bridge cannot use', () => {
    const audio = accelerando();
    const project = new Project();
    try {
      expect(() => project.autoTempo(audio, 22050, { tempoUpdateIntervalBeats: 0 })).toThrow();
      expect(() => project.analyzeTempo(audio, 22050, { rampThreshold: -1 })).toThrow();
      expect(project.analyzeTempo(audio, 22050, { includeOctaveCandidates: false })).toHaveLength(
        1,
      );
      expect(
        project.analyzeTempo(audio, 22050, { includeOctaveCandidates: true }).length,
      ).toBeGreaterThan(1);
    } finally {
      project.destroy();
    }
  });
});

describe('MIDI message module functions', () => {
  it('refuses an unknown name and an out-of-range number in every GM and CC lookup', () => {
    const byNumber: Array<[string, () => unknown]> = [
      ['gmInstrumentName', () => gmInstrumentName(128)],
      ['gmFamilyName', () => gmFamilyName(16)],
      ['gmFamilyFirstProgram', () => gmFamilyFirstProgram(16)],
      ['gmDrumName', () => gmDrumName(200)],
      ['gm2DrumSetName', () => gm2DrumSetName(128)],
      ['gm2InstrumentName', () => gm2InstrumentName({ bankLsb: 0, program: 128 })],
      ['gm2DrumName', () => gm2DrumName({ bankLsb: 0, note: 128 })],
      ['midiCcName', () => midiCcName(128)],
      ['perNoteControllerName', () => perNoteControllerName(1000)],
    ];
    for (const [name, call] of byNumber) {
      expect(call, name).toThrow(RangeError);
      expect(call, name).toThrow(name);
    }
    const byName: Array<[string, () => unknown]> = [
      ['gmProgramForName', () => gmProgramForName('no such instrument')],
      ['gmDrumNoteForName', () => gmDrumNoteForName('no such drum')],
      ['midiCcIndexForName', () => midiCcIndexForName('no such controller')],
    ];
    for (const [name, call] of byName) {
      expect(call, name).toThrow(RangeError);
      expect(call, name).toThrow(name);
    }
    expect(() => gmInstrumentName('0' as unknown as number)).toThrow(TypeError);
    expect(() => gmProgramForName(40 as unknown as string)).toThrow(TypeError);
  });

  it('refuses an unknown request key in every MIDI message packer', () => {
    const base = { ppq: 0, group: 0, channel: 0 };
    const packers: Array<[string, (request: never) => unknown, object]> = [
      ['midiNoteOn', midiNoteOn, { ...base, note: 60, velocity: 100 }],
      ['midiCc', midiCc, { ...base, controller: 7, value: 1 }],
      ['midiProgram', midiProgram, { ...base, program: 1 }],
      ['midiPitchBend', midiPitchBend, { ...base, bend: 8192 }],
      ['midi2Cc', midi2Cc, { ...base, controller: 7, value32: 1 }],
      ['midi2Program', midi2Program, { ...base, program: 1 }],
      ['midi2PerNoteManagement', midi2PerNoteManagement, { ...base, note: 60 }],
      ['midiBankProgram', midiBankProgram, { ...base, program: 1, bank: { msb: 0, lsb: 0 } }],
    ];
    for (const [name, packer, request] of packers) {
      expect(() => packer(request as never), name).not.toThrow();
      expect(() => packer({ ...request, bogus: 1 } as never), name).toThrow(
        unknownKeyError(`${name}: unknown key bogus`),
      );
    }
    expect(() =>
      midi2Program({ ...base, program: 1, bank: { msb: 0, lsb: 0, extra: 1 } } as never),
    ).toThrow(TypeError);
    expect(() => midiNoteOn({ ...base, note: '60', velocity: 1 } as never)).toThrow(TypeError);
  });

  it('packs a MIDI 2.0 program with an optional bank and per-note flags as optional fields', () => {
    expect(midi2Program({ ppq: 0, group: 0, channel: 0, program: 5 }).word1).toBe(0x05000000);
    const banked = midi2Program({
      ppq: 0,
      group: 0,
      channel: 0,
      program: 5,
      bank: { msb: 1, lsb: 2 },
    });
    expect(banked.word0 >>> 0).toBe(0x40c00001);
    expect(banked.word1).toBe(0x05000102);
    const flags = midi2PerNoteManagement({
      ppq: 0,
      group: 0,
      channel: 0,
      note: 60,
      detach: true,
      reset: true,
    });
    expect(flags.word0 >>> 0).toBe(0x40f03c03);
  });

  it('reads a null route-config field as absent', () => {
    const events = [midiNoteOn({ ppq: 0, group: 0, channel: 3, note: 60, velocity: 100 })];
    const nulls = { filterGroup: null, filterChannel: null, remapChannel: null };
    expect(midiRouteEvents({ events, ...nulls } as never)).toEqual(midiRouteEvents({ events }));
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
    expect(midiCcLearn({ events, paramId: 9 })).toMatchObject({ ccNumber: 74, paramId: 9 });
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

  it('serializes a typed MIDI-FX config and leaves its content to the core to refuse', () => {
    const project = new Project();
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    expect(project.previewMidiFxCount({ clipId, config: { chordIntervals: [0, 4, 7] } })).toBe(6);
    expect(
      project.previewMidiFxCount({ clipId, config: { velocityScale: undefined, seed: 3 } }),
    ).toBe(2);
    const bad = (config: unknown) =>
      project.previewMidiFxCount({ clipId, config: config as never });
    const entry = 'Project\\.previewMidiFxCount: ';
    expectRefusal(() => bad('{}'), TypeError, new RegExp(`^${entry}config must be an object$`));
    expectRefusal(() => bad(null), TypeError, new RegExp(`^${entry}config must be an object$`));
    const typeRefusals: [unknown, string][] = [
      [{ transposeSemitones: '12' }, 'config\\.transposeSemitones must be a number'],
      [{ chordIntervals: [0, 'x'] }, 'config\\.chordIntervals\\[1\\] must be a number'],
      [{ velocityScale: Number.NaN }, 'config\\.velocityScale must be a number'],
      [{ transpose_semitones: 1 }, unknownKeyText('unknown key config.transpose_semitones').source],
      [{ nope: 1 }, unknownKeyText('unknown key config.nope').source],
    ];
    for (const [config, text] of typeRefusals) {
      expectRefusal(() => bad(config), TypeError, new RegExp(`^${entry}${text}$`));
    }
    expectEmptyChord(
      () => project.previewMidiFxCount({ clipId, config: { chordIntervals: [] } }),
      'previewMidiFxCount',
    );
    project.destroy();
  });
});

describe('Project meter plumbing', () => {
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

describe('project track records', () => {
  it('exposes the track kind as a token in a readonly record', () => {
    expectTypeOf<Pick<ProjectTrack, 'kind'>>().toEqualTypeOf<{
      readonly kind: 'audio' | 'midi' | 'aux';
    }>();
  });
});
