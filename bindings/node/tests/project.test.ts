import { describe, expect, it } from 'vitest';
import type { BuiltinSynthConfig } from '../src/index.js';
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
import { EXPECTED_PROJECT_ABI_VERSION } from '../src/types_project.js';

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
      loopLengthPpq: 0,
    });
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
  project.addClip({ trackId, lengthPpq: 4, sourceUri: 'asset://metadata.wav' });
  const sourceId = project.clips()[0].sourceId;
  const { clipId: midiClipId } = project.addMidiClip(0, 1);
  const midiSourceId = project.clips()[1].sourceId;
  const metadata = { contentHash: 'sha256:deadbeef', externalStemRole: 'lead' };

  try {
    expect(project.sources()[0]).toMatchObject({
      id: sourceId,
      kind: 'audio',
      nameOrUri: 'asset://metadata.wav',
      contentHash: '',
      externalStemRole: '',
    });

    project.setAudioSourceMetadata({ sourceId, ...metadata });
    expect(metadata).toEqual({ contentHash: 'sha256:deadbeef', externalStemRole: 'lead' });
    expect(project.sources()[0]).toMatchObject(metadata);

    project.setAudioSourceMetadata({ sourceId, contentHash: '', externalStemRole: '' });
    expect(project.sources()[0]).toMatchObject({
      id: sourceId,
      nameOrUri: 'asset://metadata.wav',
      contentHash: '',
      externalStemRole: '',
    });
    project.undo();
    expect(project.sources()[0]).toMatchObject(metadata);
    project.redo();
    expect(project.sources()[0]).toMatchObject({ contentHash: '', externalStemRole: '' });

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
      contentHash: '',
      externalStemRole: '',
    });
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
    expect(() => Project.fromJson(document)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidFormat }),
    );
    expect(() => Project.fromJsonWithDiagnostics(document)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidFormat }),
    );
    const project = new Project();
    expect(() => project.setSampleRate(44100.5)).toThrow(/whole number of hertz/);
    expect(() => project.setSampleRate(44100.5)).toThrow(RangeError);
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
    expect(after).toContain('"warpMode":1');
    const restored = Project.fromJson(after);
    expect(restored.toJson()).toBe(after);
    restored.destroy();

    project.undo();
    expect(project.toJson()).toContain('"warpMode":0');
    project.undo();
    expect(project.toJson()).toContain('"warpRefId":0');
    project.undo();
    expect(project.toJson()).toBe(before);

    project.setClipWarpMode(clipId, 'tempoSync');
    expect(project.toJson()).toContain('"warpMode":2');

    project.destroy();
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
    const emptyChord = expect.objectContaining({
      code: ErrorCode.InvalidFormat,
      message: 'chordIntervals must hold 1 to 8 intervals',
    });
    expect(() => bare.bakeMidiFx({ clipId: bareClip, config: { chordIntervals: [] } })).toThrow(
      emptyChord,
    );
    expect(() =>
      request.bakeMidiFx({
        clipId: requestClip,
        config: { chordIntervals: [] },
        withSourceIndex: true,
      }),
    ).toThrow(emptyChord);
    expect(() =>
      request.previewMidiFxCount({ clipId: requestClip, config: { chordIntervals: [] } }),
    ).toThrow(emptyChord);

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
    project.setProgramOnChannel(clipId, {
      group: 0,
      channel: 3,
      program: 24,
      bank: { msb: 0x0123 >> 7, lsb: 0x0123 & 0x7f },
    });
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
    const cases: ReadonlyArray<[string | undefined, number]> = [
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
        project.setMixerSceneJson(
          JSON.stringify({ version: 1, buses: [{ id: 'master', role: 'master', layout }] }),
        );
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
    const audio = project.bounceWithBuiltinInstruments(
      [{ destinationId: 0, instrument: { waveform: 'saw', gain: 0.5 } }],
      { totalFrames: 4096, numChannels: 2 },
    );
    expect(audio.channels).toHaveLength(2);
    expect(audio.channels[0]).toHaveLength(4096);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('keeps the base patch when builtin synth numeric fields are explicitly zero', () => {
    const project = buildMidiOnlyProject();
    const options = { totalFrames: 4096, numChannels: 2 };
    // This facade binds the minimal oscillator synth, whose numeric fields all
    // use "0 / omit => default". The NativeSynth patch vocabulary (preset,
    // ampSustain, filterSustain) is not read here, so only the oscillator
    // fields are exercised.
    const base = project.bounceWithBuiltinInstruments(
      [{ instrument: { waveform: 'saw' } }],
      options,
    );
    const explicitZero = project.bounceWithBuiltinInstruments(
      [
        {
          instrument: {
            waveform: 'saw',
            gain: 0,
            attackMs: 0,
            decayMs: 0,
            sustain: 0,
            releaseMs: 0,
            polyphony: 0,
          },
        },
      ],
      options,
    );
    expect(explicitZero).toEqual(base);
    // Guard against a vacuous pass: a non-default value must move the render,
    // otherwise the equality above would hold for an unrelated reason.
    const moved = project.bounceWithBuiltinInstruments(
      [{ instrument: { waveform: 'saw', sustain: 0.1 } }],
      options,
    );
    expect(moved).not.toEqual(base);
    project.destroy();
  });

  it('accepts a waveform alias in a binding and an explicit bindings array', () => {
    const project = buildMidiOnlyProject();
    const byName = project.bounceWithBuiltinInstruments([{ instrument: { waveform: 'sine' } }], {
      totalFrames: 2048,
      numChannels: 1,
    });
    expect(peak(byName)).toBeGreaterThan(0);

    const byAlias = project.bounceWithBuiltinInstruments(
      [{ instrument: { waveform: 'sawtooth' } }],
      { totalFrames: 2048, numChannels: 1 },
    );
    expect(peak(byAlias)).toBeGreaterThan(0);

    const byArray = project.bounceWithBuiltinInstruments(
      [{ destinationId: 0, instrument: { waveform: 'square' } }],
      { totalFrames: 2048, numChannels: 1 },
    );
    expect(peak(byArray)).toBeGreaterThan(0);

    // A bare waveform name or a bare config is not a binding.
    const loose = project as unknown as {
      bounceWithBuiltinInstruments(...args: unknown[]): unknown;
    };
    expect(() => loose.bounceWithBuiltinInstruments('sine')).toThrow(TypeError);
    expect(() => loose.bounceWithBuiltinInstruments([{ waveform: 'sine' }])).toThrow(TypeError);
    project.destroy();
  });

  it('auto-derives the render length when options/totalFrames is omitted (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    // Pass only the instrument; options defaults to {} and length auto-derives.
    const audio = project.bounceWithBuiltinInstruments([{ instrument: { waveform: 'triangle' } }]);
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
    const audio = project.bounceWithBuiltinInstruments([{ instrument: patch }]);
    expect(audio.channels[0].length).toBeGreaterThan(0);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('rejects an unknown built-in synth waveform name', () => {
    const project = buildMidiOnlyProject();
    expect(() =>
      project.bounceWithBuiltinInstruments([
        {
          instrument: { waveform: 'noise' as unknown as 'sine' },
        },
      ]),
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
          bounceWithBuiltinInstruments(
            bindings: unknown[],
            settings: object,
          ): { interleaved: Float32Array };
        };
      }
    ).handle.bounceWithBuiltinInstruments([{ waveform }], {}).interleaved;

  it('refuses a numeric waveform at the facade, in range or not', () => {
    for (const waveform of [0, 3, 4, -1, Number.NaN]) {
      const project = buildMidiOnlyProject();
      expect(() =>
        project.bounceWithBuiltinInstruments([
          {
            instrument: { waveform: waveform as unknown as 'sine' },
          },
        ]),
      ).toThrow(new TypeError('bounceWithBuiltinInstruments: waveform must be a name'));
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
      ).toThrow(/32-bit integer range/);
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
    project.destroy();
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
      names.push(m.name);
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

  it('accepts a mixer scene JSON without throwing', () => {
    const project = new Project();
    expect(() => project.setMixerSceneJson('{}')).not.toThrow();
    project.destroy();
  });

  it('reports no last bounce compile result on a project no bounce has run on', () => {
    const project = new Project();
    expect(project.lastBounceCompileResult()).toBeUndefined();
    project.destroy();
  });

  it('reports empty diagnostics after a clean bounce', () => {
    const project = new Project();
    project.setSampleRate(48000);
    project.bounce({ totalFrames: 256 });
    expect(project.lastBounceCompileResult()).toEqual({ diagnostics: [] });
    project.destroy();
  });

  it('reports the error diagnostics of a failed bounce', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const trackId = project.addTrack({ kind: 'audio' });
    // A clip whose audio source was never supplied cannot be compiled.
    project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://missing.wav' });
    expect(() => project.bounce({ totalFrames: 256 })).toThrow();
    const result = project.lastBounceCompileResult();
    expect(result).toBeDefined();
    expect(Object.keys(result ?? {})).toEqual(['diagnostics']);
    expect(result?.diagnostics.some((diagnostic) => diagnostic.severity === 'error')).toBe(true);
    project.destroy();
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
      new RangeError("gmProgramForName: unknown instrument name 'No Such Instrument'"),
    );
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
      ['gm2DrumSetName', () => gm2DrumSetName(127)],
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
        new TypeError(`${name}: unknown request key 'bogus'`),
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
    expect(() => bad('{}')).toThrow(TypeError);
    expect(() => bad(null)).toThrow(TypeError);
    expect(() => bad({ transposeSemitones: '12' })).toThrow(/transposeSemitones/);
    for (const config of [
      { transposeSemitones: '12' },
      { chordIntervals: [0, 'x'] },
      { velocityScale: Number.NaN },
      { transpose_semitones: 1 },
      { nope: 1 },
    ]) {
      expect(() => bad(config)).toThrow(expect.objectContaining({ code: ErrorCode.InvalidFormat }));
    }
    expect(() => bad({ transpose_semitones: 1 })).toThrow(/write transposeSemitones/);
    expect(() => project.previewMidiFxCount({ clipId, config: { chordIntervals: [] } })).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: 'chordIntervals must hold 1 to 8 intervals',
      }),
    );
    project.destroy();
  });
});
