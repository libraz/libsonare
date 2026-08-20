import { describe, expect, it } from 'vitest';
import type { BuiltinSynthConfig } from '../src/index.js';
import {
  EXPECTED_PROJECT_ABI_VERSION,
  MarkerKind,
  Project,
  projectAbiVersion,
} from '../src/index.js';

/** Build a small deterministic project: a track + an audio clip + a MIDI clip + tempo. */
function buildProject(): Project {
  const project = Project.create();
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
    audio,
    audioChannels: 1,
    audioSampleRate: 48000,
  });

  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(2, 0, 0, 60, 0),
  ]);

  return project;
}

it('accepts a metadata-only audio clip with a source URI', () => {
  const project = Project.create();
  const trackId = project.addTrack({ kind: 'audio' });
  expect(() =>
    project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'asset://lead.wav' }),
  ).not.toThrow();
  expect(project.toJson()).toContain('asset://lead.wav');
  project.destroy();
});

it('reads stored tracks, clips, and sources by index', () => {
  const project = Project.create();
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
    expect(project.trackByIndex(0)).toMatchObject({
      id: trackId,
      kind: 0,
      name: 'readback',
      gain: 1,
      pan: 0,
      mute: false,
      solo: false,
    });
    expect(project.clipByIndex(0)).toMatchObject({
      id: clipId,
      trackId,
      sourceKind: 0,
      startPpq: 2,
      lengthPpq: 4,
      sourceOffsetPpq: 1,
      gain: 0.75,
      loopMode: 0,
      loopLengthPpq: 0,
    });
    expect(project.sourceByIndex(0)).toMatchObject({
      id: project.clipByIndex(0).sourceId,
      kind: 0,
      nameOrUri: 'asset://readback.wav',
    });
    expect(() => project.trackByIndex(1)).toThrow();
    expect(() => project.clipByIndex(1)).toThrow();
    expect(() => project.sourceByIndex(1)).toThrow();
  } finally {
    project.destroy();
  }
});

it('reads and edits audio source metadata without mutating input', () => {
  const project = Project.create();
  const trackId = project.addTrack({ kind: 'audio' });
  project.addClip({ trackId, lengthPpq: 4, sourceUri: 'asset://metadata.wav' });
  const sourceId = project.clipByIndex(0).sourceId;
  const { clipId: midiClipId } = project.addMidiClip(0, 1);
  const midiSourceId = project.clipByIndex(1).sourceId;
  const metadata = { contentHash: 'sha256:deadbeef', externalStemRole: 'lead' };

  try {
    expect(project.sourceByIndex(0)).toMatchObject({
      id: sourceId,
      kind: 0,
      nameOrUri: 'asset://metadata.wav',
      contentHash: '',
      externalStemRole: '',
    });

    project.setAudioSourceMetadata(sourceId, metadata);
    expect(metadata).toEqual({ contentHash: 'sha256:deadbeef', externalStemRole: 'lead' });
    expect(project.sourceByIndex(0)).toMatchObject(metadata);

    project.setAudioSourceMetadata(sourceId, '', '');
    expect(project.sourceByIndex(0)).toMatchObject({
      id: sourceId,
      nameOrUri: 'asset://metadata.wav',
      contentHash: '',
      externalStemRole: '',
    });
    project.undo();
    expect(project.sourceByIndex(0)).toMatchObject(metadata);
    project.redo();
    expect(project.sourceByIndex(0)).toMatchObject({ contentHash: '', externalStemRole: '' });

    project.setAudioSourceMetadata(sourceId, metadata);
    const restored = Project.fromJson(project.toJson());
    try {
      expect(restored.sourceByIndex(0)).toMatchObject(metadata);
    } finally {
      restored.destroy();
    }

    expect(() => project.setAudioSourceMetadata(999999, metadata)).toThrow();
    expect(() => project.setAudioSourceMetadata(midiSourceId, metadata)).toThrow();
    expect(project.clipByIndex(1).id).toBe(midiClipId);
    expect(project.sourceByIndex(0)).toMatchObject(metadata);
    expect(project.sourceByIndex(1)).toMatchObject({
      id: midiSourceId,
      kind: 1,
      contentHash: '',
      externalStemRole: '',
    });
  } finally {
    project.destroy();
  }
});

it('rebinds deserialized source audio before bouncing', () => {
  const project = Project.create();
  const trackId = project.addTrack({ kind: 'audio' });
  const audio = new Float32Array(480).fill(0.25);
  project.addClip({
    trackId,
    startPpq: 0,
    lengthPpq: 1,
    audio,
    audioChannels: 1,
    audioSampleRate: 48000,
    sourceUri: 'asset://lead.wav',
  });
  const restored = Project.fromJson(project.toJson());
  const sourceIds = restored.unresolvedAudioSourceIds();
  expect(sourceIds).toHaveLength(1);
  restored.setSourceAudio(sourceIds[0], audio, 1, 48000);
  expect(restored.unresolvedAudioSourceIds()).toEqual([]);
  expect(restored.bounce({ totalFrames: 480, numChannels: 1 })).toHaveLength(480);
  restored.destroy();
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

function danglingSourceJson(): string {
  return '{"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,"channel_strip_ref":"","output_target":"","midi_destination_id":0,"automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,"start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,"fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0,"warp_mode":0}]}';
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
    expect(projectAbiVersion()).toBeGreaterThan(0);
  });

  it('imports external planar stems through the project C ABI', () => {
    const project = Project.create();
    project.setSampleRate(48000);
    const result = project.importExternalStems({
      sampleRate: 48000,
      stems: [
        {
          name: 'vocals',
          role: 'lead',
          layout: 'stereo',
          planarSamples: [new Float32Array([1, 0, 0]), new Float32Array([-0.5, 0, 0])],
          startFrame: 113,
        },
        {
          name: 'drums',
          layout: 'mono',
          planarSamples: [new Float32Array([0.25, 0, 0])],
        },
      ],
    });
    expect(result.trackIds).toHaveLength(2);
    expect(result.clipIds).toHaveLength(2);
    expect(project.trackCount()).toBe(2);
    expect(project.clipCount()).toBe(2);
    expect(() =>
      project.importExternalStems({
        sampleRate: 48000,
        stems: [
          { name: 'duplicate', layout: 'mono', planarSamples: [new Float32Array([0])] },
          { name: 'duplicate', layout: 'mono', planarSamples: [new Float32Array([0])] },
        ],
      }),
    ).toThrow();
    expect(project.trackCount()).toBe(2);
    expect(project.clipCount()).toBe(2);
    project.destroy();
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

    expect(result.hasTimeline).toBe(true);
    expect(result.diagnostics.length).toBe(1);
    expect(result.diagnostics[0].code).toBe(10);
    expect(result.diagnostics[0].message).toContain('project contains MIDI clips');
    expect(result.messages.split('\n')[0]).toBe(result.diagnostics[0].message);

    project.destroy();
  });

  it('bounces deterministically (bit-exact across two calls)', () => {
    const project = buildProject();
    const options = { totalFrames: 1024, blockSize: 128, numChannels: 2, sampleRate: 48000 };

    const first = project.bounce(options);
    const second = project.bounce(options);

    expect(first.length).toBe(second.length);
    expect(first.length).toBeGreaterThan(0);
    expect(Array.from(first)).toEqual(Array.from(second));

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
    const project = Project.create();
    for (let i = 0; i < 5; i++) {
      project.addTrack({ kind: 'midi', name: `t${i}` });
    }
    expect(project.trackCount()).toBe(5);

    // Keep only the two most-recent edits in history.
    project.setMaxUndoDepth(2);
    project.undo();
    project.undo();
    expect(project.trackCount()).toBe(3);
    // The third undo has no surviving history entry to rewind.
    expect(() => project.undo()).toThrow();

    project.destroy();
  });

  it('setMaxHistoryBytes validates input and supports zero retention', () => {
    const project = Project.create();
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
    expect(project.trackCount()).toBe(0);

    // A regular edit remains undoable under a positive byte cap.
    project.setMaxHistoryBytes(Number.MAX_SAFE_INTEGER);
    project.addTrack({ kind: 'midi', name: 'undoable' });
    expect(project.trackCount()).toBe(1);
    project.undo();
    expect(project.trackCount()).toBe(0);

    // Zero is a valid no-retention policy: mutation succeeds, but has no undo.
    project.setMaxHistoryBytes(0);
    project.addTrack({ kind: 'midi', name: 'not-undoable' });
    expect(project.trackCount()).toBe(1);
    expect(() => project.undo()).toThrow();
    expect(project.trackCount()).toBe(1);

    project.destroy();
  });

  it('clearHistory leaves nothing to undo', () => {
    const project = Project.create();
    project.addTrack({ kind: 'midi', name: 'a' });
    project.addTrack({ kind: 'midi', name: 'b' });
    expect(project.trackCount()).toBe(2);

    project.clearHistory();
    expect(project.trackCount()).toBe(2);
    expect(() => project.undo()).toThrow();

    project.destroy();
  });

  it('addMidiClip undoes and redoes track, source, and clip as one edit', () => {
    const project = Project.create();
    const before = project.toJson();
    project.addMidiClip(2, 4);
    const added = project.toJson();
    expect(project.trackCount()).toBe(1);
    expect(project.sourceCount()).toBe(1);
    expect(project.clipCount()).toBe(1);

    project.undo();
    expect(project.toJson()).toBe(before);
    expect(project.trackCount()).toBe(0);
    expect(project.sourceCount()).toBe(0);
    expect(project.clipCount()).toBe(0);

    project.redo();
    expect(project.toJson()).toBe(added);
    expect(project.trackCount()).toBe(1);
    expect(project.sourceCount()).toBe(1);
    expect(project.clipCount()).toBe(1);
    project.destroy();
  });

  it('routes a track to a MIDI destination and undoes it', () => {
    const project = Project.create();
    const trackId = project.addTrack({ kind: 'midi', name: 'lead' });
    const before = project.toJson();

    project.setTrackMidiDestination(trackId, 7);
    const after = project.toJson();
    expect(after).not.toBe(before);
    expect(after).toContain('"midi_destination_id":7');

    project.undo();
    expect(project.toJson()).toBe(before);

    project.destroy();
  });

  it('sets track gain / mute / solo / pan and undoes them', () => {
    const project = Project.create();
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
    const project = Project.create();
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
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
    expect(after).toContain('"warp_ref_id":123');
    expect(after).toContain('"warp_mode":1');
    const restored = Project.fromJson(after);
    expect(restored.toJson()).toBe(after);
    restored.destroy();

    project.undo();
    expect(project.toJson()).toContain('"warp_mode":0');
    project.undo();
    expect(project.toJson()).toContain('"warp_ref_id":0');
    project.undo();
    expect(project.toJson()).toBe(before);

    project.setClipWarpMode(clipId, 'tempo-sync');
    expect(project.toJson()).toContain('"warp_mode":2');

    project.destroy();
  });

  it('clears clip warp references when removing a warp map and restores them on undo', () => {
    const project = Project.create();
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
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
    expect(project.toJson()).toContain('"warp_ref_id":0');
    project.undo();
    expect(project.toJson()).toBe(mapped);
    expect(() =>
      project.setWarpMap({ id: 124, anchors: [{ warpSample: 0, sourceSample: 0 }] }),
    ).toThrow();
    project.destroy();
  });

  it('rejects routing an unknown track', () => {
    const project = Project.create();
    expect(() => project.setTrackMidiDestination(9999, 1)).toThrow();
    project.destroy();
  });

  it('exports MIDI to an SMF Buffer', () => {
    const project = buildProject();
    const smf = project.exportSmf();
    expect(smf.length).toBeGreaterThan(0);
    // Standard MIDI File header chunk magic.
    expect(smf.subarray(0, 4).toString('ascii')).toBe('MThd');
    project.destroy();
  });

  it('rejects MIDI loop export that exceeds the expansion budget', () => {
    const project = Project.create();
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [Project.midiNoteOn(0, 0, 0, 60, 100)]);
    project.setClipLoop(clipId, 'loop', 1e-9);
    expect(() => project.exportSmf()).toThrow();
    expect(() => project.exportClipFile()).toThrow();
    project.destroy();
  });

  it('round-trips a MIDI 2.0 Clip File through the binding', () => {
    const project = buildProject();
    const { clipId } = project.addMidiClip(0, 4);
    // A MIDI 2.0 note-on (message type 0x4) with a full 16-bit velocity.
    project.setMidiEvents(clipId, [
      { ppq: 0, data0: 0x40903c00, data1: 0xbeef0000 },
      { ppq: 1, data0: 0x40803c00, data1: 0 },
    ]);

    const clipFile = project.exportClipFile();
    expect(clipFile.length).toBeGreaterThan(8);
    // MIDI 2.0 Clip File header magic.
    expect(clipFile.subarray(0, 8).toString('ascii')).toBe('SMF2CLIP');

    const reimported = Project.create();
    const firstClip = reimported.importClipFile(clipFile);
    expect(firstClip).toBeGreaterThan(0);
    expect(reimported.exportClipFile().subarray(0, 8).toString('ascii')).toBe('SMF2CLIP');
    reimported.destroy();
    project.destroy();
  });

  it('reports MIDI-FX provenance and matches the positional bake', () => {
    const seed = (project: Project, clip: number) => {
      project.setMidiEvents(clip, [
        Project.midiNoteOn(0, 0, 0, 60, 100),
        Project.midiNoteOff(1, 0, 0, 60, 0),
        Project.midiNoteOn(2, 0, 0, 64, 100),
        Project.midiNoteOff(3, 0, 0, 64, 0),
      ]);
    };
    const config = '{"chord_intervals":[0,4,7]}';

    const request = buildProject();
    const { clipId: requestClip } = request.addMidiClip(0, 4);
    seed(request, requestClip);
    expect(request.previewMidiFxCount({ clipId: requestClip, configJson: config })).toBe(12);
    const result = request.bakeMidiFx({
      clipId: requestClip,
      configJson: config,
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

    // The positional form must land on identical bytes, and the request form
    // without the flag must behave like it.
    const positional = buildProject();
    const { clipId: positionalClip } = positional.addMidiClip(0, 4);
    seed(positional, positionalClip);
    positional.bakeMidiFx(positionalClip, config);
    expect(request.exportSmf()).toEqual(positional.exportSmf());

    const bare = buildProject();
    const { clipId: bareClip } = bare.addMidiClip(0, 4);
    seed(bare, bareClip);
    expect(bare.bakeMidiFx({ clipId: bareClip, configJson: config })).toEqual({});
    expect(bare.exportSmf()).toEqual(positional.exportSmf());

    // Errors travel the same path in both forms.
    expect(() => positional.bakeMidiFx(positionalClip, '{bad json')).toThrow();
    expect(() =>
      request.bakeMidiFx({ clipId: requestClip, configJson: '{bad json', withSourceIndex: true }),
    ).toThrow();
    expect(() =>
      request.previewMidiFxCount({ clipId: requestClip, configJson: '{bad json' }),
    ).toThrow();

    request.destroy();
    positional.destroy();
    bare.destroy();
  });

  it('applies program changes, MIDI FX, and preserves imported SysEx payloads', () => {
    const project = buildProject();
    const { clipId } = project.addMidiClip(0, 2);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0.1, 0, 0, 60, 100),
      Project.midiPolyPressure(0.2, 0, 0, 60, 70),
      Project.midiChannelPressure(0.3, 0, 0, 80),
      Project.midiPitchBend(0.4, 0, 0, 8192),
      Project.midiNoteOff(1.1, 0, 0, 60, 0),
    ]);
    project.bakeMidiFx(
      clipId,
      '{"transpose_semitones":12,"quantize_ppq":0.25,"quantize_strength":1.0}',
    );
    project.setProgram(clipId, 42);
    project.setProgramOnChannel(clipId, 0, 3, 24, 0x0123);
    const smf = project.exportSmf();
    expect(smf.includes(Buffer.from([0xc0, 42]))).toBe(true);
    expect(smf.includes(Buffer.from([0xc3, 24]))).toBe(true);
    project.destroy();

    const sysexProject = Project.create();
    const firstClip = sysexProject.importSmf(makeSysexSmf());
    expect(firstClip).toBeGreaterThan(0);
    const json = sysexProject.toJson();
    expect(json).toContain('__sysex_payloads');
    const restored = Project.fromJson(json);
    const restoredSmf = restored.exportSmf();
    expect(restoredSmf.includes(Buffer.from([0xf0, 0x05, 0x7e, 0x7f, 0x09, 0x01, 0xf7]))).toBe(
      true,
    );
    sysexProject.destroy();
    restored.destroy();
  });

  it('preserves more than 512 events when baking MIDI FX', () => {
    const project = Project.create();
    const { clipId } = project.addMidiClip(0, 8);
    const events = Array.from({ length: 600 }, (_, i) =>
      Project.midiCc(i / 80, 0, 0, i % 128, (i * 3) % 128),
    );
    project.setMidiEvents(clipId, events);
    project.bakeMidiFx(clipId, '{}');

    const payload = JSON.parse(project.toJson()) as {
      midi_content: Record<string, Array<{ ppq: number; data0: number; data1: number }>>;
    };
    expect(payload.midi_content[String(clipId)]).toEqual(events);

    const round = Project.create();
    const roundClip = round.importSmf(project.exportSmf());
    round.bakeMidiFx(roundClip, '{}');
    const roundPayload = JSON.parse(round.toJson()) as {
      midi_content: Record<string, Array<{ ppq: number; data0: number; data1: number }>>;
    };
    expect(roundPayload.midi_content[String(roundClip)]).toHaveLength(600);
    round.destroy();
    project.destroy();
  });

  it('imports the valid prefix of a truncated SMF', () => {
    const project = Project.create();
    expect(project.importSmf(makeTruncatedSmf())).toBeGreaterThan(0);
    project.destroy();
  });

  it('validates MIDI event inputs before native conversion', () => {
    const project = Project.create();
    const { clipId } = project.addMidiClip(0, 1);

    expect(() => project.setMidiEvents(clipId, [{ ppq: Number.NaN, data0: 0, data1: 0 }])).toThrow(
      /ppq/,
    );
    expect(() => project.setMidiEvents(clipId, [{ ppq: 0, data0: -1, data1: 0 }])).toThrow(/data0/);
    expect(() =>
      project.setMidiEvents(clipId, [[0, 0] as unknown as [number, number, number]]),
    ).toThrow(/\[ppq, data0, data1\]/);

    project.destroy();
  });

  it('throws cleanly on malformed fromJson input', () => {
    expect(() => Project.fromJson('{ not valid project json')).toThrow();
  });

  it('fromJsonWithDiagnostics returns warnings from successful loads', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(danglingSourceJson());
    expect(diagnostics).toContain('dangling_clip_source');
    expect(project.trackCount()).toBe(1);
    project.destroy();
  });

  /** A MIDI-only project: one MIDI clip holding a sustained note routed to dest 0. */
  function buildMidiOnlyProject(): Project {
    const project = Project.create();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(2, 0, 0, 60, 0),
    ]);
    return project;
  }

  function peak(audio: Float32Array): number {
    let p = 0;
    for (let i = 0; i < audio.length; i++) {
      const a = Math.abs(audio[i]);
      if (a > p) {
        p = a;
      }
    }
    return p;
  }

  it('bounces a MIDI-only project to silence without a bound instrument', () => {
    const project = buildMidiOnlyProject();
    const audio = project.bounce({ totalFrames: 4096, numChannels: 2, sampleRate: 48000 });
    expect(audio.length).toBe(4096 * 2);
    expect(peak(audio)).toBe(0);
    project.destroy();
  });

  it('bounces MIDI through the built-in synth to non-silent audio (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    const audio = project.bounceWithBuiltinInstrument(
      { waveform: 'saw', destinationId: 0, gain: 0.5 },
      { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
    );
    expect(audio.length).toBe(4096 * 2);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('keeps the base patch when builtin synth numeric fields are explicitly zero', () => {
    const project = buildMidiOnlyProject();
    const options = { totalFrames: 4096, numChannels: 2, sampleRate: 48000 };
    // This facade binds the minimal oscillator synth, whose numeric fields all
    // use "0 / omit => default". The NativeSynth patch vocabulary (preset,
    // ampSustain, filterSustain) is not read here, so only the oscillator
    // fields are exercised.
    const base = project.bounceWithBuiltinInstrument({ waveform: 'saw' }, options);
    const explicitZero = project.bounceWithBuiltinInstrument(
      { waveform: 'saw', gain: 0, attackMs: 0, decayMs: 0, sustain: 0, releaseMs: 0, polyphony: 0 },
      options,
    );
    expect(explicitZero).toEqual(base);
    // Guard against a vacuous pass: a non-default value must move the render,
    // otherwise the equality above would hold for an unrelated reason.
    const moved = project.bounceWithBuiltinInstrument({ waveform: 'saw', sustain: 0.1 }, options);
    expect(moved).not.toEqual(base);
    project.destroy();
  });

  it('accepts a bare waveform name and an explicit bindings array (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    const byName = project.bounceWithBuiltinInstrument('sine', {
      totalFrames: 2048,
      numChannels: 1,
      sampleRate: 48000,
    });
    expect(peak(byName)).toBeGreaterThan(0);

    const byAlias = project.bounceWithBuiltinInstrument('sawtooth', {
      totalFrames: 2048,
      numChannels: 1,
      sampleRate: 48000,
    });
    expect(peak(byAlias)).toBeGreaterThan(0);

    const byArray = project.bounceWithBuiltinInstruments(
      [{ destinationId: 0, waveform: 'square' }],
      { totalFrames: 2048, numChannels: 1, sampleRate: 48000 },
    );
    expect(peak(byArray)).toBeGreaterThan(0);
    project.destroy();
  });

  it('auto-derives the render length when options/totalFrames is omitted (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    // Pass only the instrument; options defaults to {} and length auto-derives.
    const audio = project.bounceWithBuiltinInstrument({ waveform: 'triangle' });
    expect(audio.length).toBeGreaterThan(0);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('accepts a patch typed with the shared BuiltinSynthConfig alias', () => {
    const project = buildMidiOnlyProject();
    // The Python binding names this concept BuiltinSynthConfig; the Node alias
    // lets portable code share that name (tsc validates the alias accepts the
    // same shape as BuiltinInstrumentConfig).
    const patch: BuiltinSynthConfig = { waveform: 'square', gain: 0.3 };
    const audio = project.bounceWithBuiltinInstrument(patch);
    expect(audio.length).toBeGreaterThan(0);
    expect(peak(audio)).toBeGreaterThan(0);
    project.destroy();
  });

  it('rejects an unknown built-in synth waveform name', () => {
    const project = buildMidiOnlyProject();
    expect(() =>
      project.bounceWithBuiltinInstrument({
        waveform: 'noise' as unknown as 'sine',
      }),
    ).toThrow(/sawtooth/);
    project.destroy();
  });
});

describe('Project value-model accessors', () => {
  it('round-trips track and source counts', () => {
    const project = Project.create();
    expect(project.trackCount()).toBe(0);
    expect(project.clipCount()).toBe(0);
    project.addTrack({ kind: 'audio', name: 'lead' });
    expect(project.trackCount()).toBe(1);
    const audio = new Float32Array(64).fill(0.1);
    const clipId = project.addClip({
      trackId: 1,
      startPpq: 0,
      lengthPpq: 4,
      audio,
      audioChannels: 1,
      audioSampleRate: 48000,
    });
    expect(project.clipCount()).toBe(1);
    expect(project.sourceCount()).toBeGreaterThanOrEqual(1);
    const restored = Project.fromJson(project.toJson());
    expect(restored.clipCount()).toBe(1);
    restored.destroy();
    project.removeClip(clipId);
    expect(project.clipCount()).toBe(0);
    project.destroy();
  });

  it('reads and sets the sample rate', () => {
    const project = Project.create();
    project.setSampleRate(44100);
    expect(project.getSampleRate()).toBe(44100);
    project.destroy();
  });

  it('reads and sets the overlap policy', () => {
    const project = Project.create();
    project.setOverlapPolicy(1);
    expect(project.getOverlapPolicy()).toBe(1);
    project.setOverlapPolicy(0);
    expect(project.getOverlapPolicy()).toBe(0);
    project.destroy();
  });

  it('replaces tempo segments and counts them', () => {
    const project = Project.create();
    project.setTempoSegments([
      { startPpq: 0, bpm: 120 },
      { startPpq: 4, bpm: 140, endBpm: 160 },
    ]);
    expect(project.tempoSegmentCount()).toBe(2);
    project.destroy();
  });

  it('reads tempo segments back in the shape the setter takes', () => {
    // The count is only usable alongside a way to read what it counts, and a
    // segment that reads back in the setter's own shape can be moved between
    // projects without a translation step.
    const source = Project.create();
    const segments = [
      { startPpq: 0, bpm: 120, endBpm: 0 },
      { startPpq: 4, bpm: 140, endBpm: 160 },
    ];
    source.setTempoSegments(segments);
    const readBack = [source.tempoSegmentByIndex(0), source.tempoSegmentByIndex(1)];
    expect(readBack).toEqual(segments);
    // startSample is never returned: the setter ignores it and a project keeps
    // musical positions only.
    expect('startSample' in readBack[0]).toBe(false);
    expect(() => source.tempoSegmentByIndex(2)).toThrow();

    const target = Project.create();
    target.setTempoSegments(readBack);
    expect(target.tempoSegmentByIndex(1)).toEqual(readBack[1]);
    source.destroy();
    target.destroy();
  });

  it('replaces time signatures and counts them', () => {
    const project = Project.create();
    project.setTimeSignatures([
      { startPpq: 0, numerator: 4, denominator: 4 },
      { startPpq: 8, numerator: 3, denominator: 4 },
    ]);
    expect(project.timeSignatureCount()).toBe(2);
    expect(project.timeSignatureByIndex(1)).toEqual({
      startPpq: 8,
      numerator: 3,
      denominator: 4,
    });
    expect(() => project.timeSignatureByIndex(2)).toThrow();
    project.destroy();
  });

  it('adds a marker and returns its id', () => {
    const project = Project.create();
    const id = project.setMarker(0, 1.5, 'verse');
    expect(typeof id).toBe('number');
    expect(id).toBeGreaterThan(0);
    project.destroy();
  });

  it('round-trips a structured marker through setMarkerEx / markerByIndex', () => {
    const project = Project.create();
    const id = project.setMarkerEx({
      ppq: 2,
      name: 'E minor',
      kind: MarkerKind.KeySignature,
      keyFifths: 1,
      keyMinor: true,
    });
    expect(id).toBeGreaterThan(0);
    const marker = project.markerByIndex(0);
    expect(marker.id).toBe(id);
    expect(marker.ppq).toBe(2);
    expect(marker.name).toBe('E minor');
    expect(marker.kind).toBe(MarkerKind.KeySignature);
    expect(marker.keyFifths).toBe(1);
    expect(marker.keyMinor).toBe(true);

    // A second structured marker; markerCount lets a host iterate by index.
    project.setMarkerEx({ ppq: 5, name: 'chorus' });
    expect(project.markerCount()).toBe(2);
    const names: string[] = [];
    for (let i = 0; i < project.markerCount(); i++) {
      const m = project.markerByIndex(i);
      expect(typeof m.id).toBe('number');
      names.push(m.name);
    }
    expect(names).toContain('E minor');
    expect(names).toContain('chorus');

    // A plain setMarkerEx keeps the default Marker kind with neutral key fields.
    const plain = project.markerByIndex(1);
    expect(plain.kind).toBe(MarkerKind.Marker);
    expect(plain.keyFifths).toBe(0);
    expect(plain.keyMinor).toBe(false);
    project.destroy();
  });

  it('round-trips a long UTF-8 marker name without splitting or truncating it', () => {
    const project = Project.create();
    const name = 'あいうえお'.repeat(7);
    expect(Array.from(name).length).toBeGreaterThan(30);
    project.setMarkerEx({ ppq: 3, name });
    expect(project.markerByIndex(0).name).toBe(name);
    expect(JSON.parse(project.toJson()).markers[0].name).toBe(name);
    project.destroy();
  });

  it('accepts a mixer scene JSON without throwing', () => {
    const project = Project.create();
    expect(() => project.setMixerSceneJson('{}')).not.toThrow();
    project.destroy();
  });

  it('exposes the last bounce compile result', () => {
    const project = buildProject();
    project.bounce({ totalFrames: 256 });
    const result = project.lastBounceCompileResult();
    expect(typeof result.hasTimeline).toBe('boolean');
    expect(Array.isArray(result.diagnostics)).toBe(true);
    project.destroy();
  });

  it('reports an empty compile result on a project no bounce has run on', () => {
    // No bounce has run, so the result is empty in full. A failed bounce is
    // told apart by its diagnostics, never by hasTimeline alone: losing the
    // timeline always leaves an error diagnostic behind.
    const project = Project.create();
    const result = project.lastBounceCompileResult();
    expect(result.hasTimeline).toBe(false);
    expect(result.diagnostics).toHaveLength(0);
    project.destroy();
  });
});

describe('Project validateMidiNotes', () => {
  it('reports fully paired note-on/note-off as ok', () => {
    const project = Project.create();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(2, 0, 0, 60, 0),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(true);
    expect(result.unmatchedNoteOns).toBe(0);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });

  it('flags a hanging note-on as not ok', () => {
    const project = Project.create();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [Project.midiNoteOn(0, 0, 0, 60, 100)]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(false);
    expect(result.unmatchedNoteOns).toBe(1);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });

  it('keeps note pairing separate across UMP groups', () => {
    const project = Project.create();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(1, 1, 0, 60, 0),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(false);
    expect(result.unmatchedNoteOns).toBe(1);
    expect(result.unmatchedNoteOffs).toBe(1);
    project.destroy();
  });

  it('validates the same half-open clip window used by SMF export', () => {
    const project = Project.create();
    const { clipId } = project.addMidiClip(0, 1);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(1, 0, 0, 60, 0),
    ]);

    const result = project.validateMidiNotes(clipId);
    expect(result.ok).toBe(false);
    expect(result.unmatchedNoteOns).toBe(1);
    expect(result.unmatchedNoteOffs).toBe(0);
    project.destroy();
  });
});

// Golden vectors for the hand-written UMP MIDI-1.0 channel-voice packing
// (Project.midiNoteOn/Off/Cc/... build data0 in TS rather than via the native
// packer). These exact words are what sonare::midi::make_midi1_* — and thus the
// C-ABI sonare_midi_* packers Python delegates to — produce, so pinning them
// here (with the identical vectors mirrored in the WASM suite) makes any silent
// drift between the two hand-written JS copies a CI failure.
describe('UMP MIDI-1.0 packing matches the canonical word layout', () => {
  it('packs channel-voice messages into the spec word', () => {
    expect(Project.midiNoteOn(0, 0, 0, 60, 100).data0 >>> 0).toBe(0x20903c64);
    expect(Project.midiNoteOff(2, 0, 0, 60, 0).data0 >>> 0).toBe(0x20803c00);
    expect(Project.midiCc(0, 0, 0, 7, 127).data0 >>> 0).toBe(0x20b0077f);
    expect(Project.midiPolyPressure(0, 0, 0, 60, 64).data0 >>> 0).toBe(0x20a03c40);
    expect(Project.midiProgram(0, 0, 0, 5).data0 >>> 0).toBe(0x20c00500);
    expect(Project.midiChannelPressure(0, 0, 0, 90).data0 >>> 0).toBe(0x20d05a00);
    // Group and channel nibbles land in their own fields.
    expect(Project.midiNoteOn(0, 0xa, 0x3, 60, 100).data0 >>> 0).toBe(0x2a933c64);
  });

  it('exposes ProgramMap names and bank/program lowering', () => {
    expect(Project.gmInstrumentName(0)).toBe('Acoustic Grand Piano');
    expect(Project.gmInstrumentName(40)).toBe('Violin');
    expect(Project.gmInstrumentName(128)).toBeNull();
    expect(Project.gmProgramForName('Violin')).toBe(40);
    expect(Project.gmProgramForName('No Such Instrument')).toBe(-1);
    expect(Project.gmFamilyName(4)).toBe('Bass');
    expect(Project.gmFamilyFirstProgram(4)).toBe(32);
    expect(Project.gm2InstrumentName(1, 24)).toBe('Ukulele');
    expect(Project.gmDrumName(38)).toBe('Acoustic Snare');
    expect(Project.gmDrumNoteForName('Open Triangle')).toBe(81);
    expect(Project.gm2DrumSetName(40)).toBe('Brush');
    expect(Project.gm2DrumName(40, 40)).toBe('Brush Swirl');
    expect(Project.midiCcName(74)).toBe('Brightness');
    expect(Project.midiCcIndexForName('Pan (MSB)')).toBe(10);
    expect(Project.perNoteControllerName(11)).toBe('Expression');

    const events = Project.midiBankProgram(0, 0, 3, 0x79, 1, 24);
    expect(events.map((event) => event.data0 >>> 0)).toEqual([0x20b30079, 0x20b32001, 0x20c31800]);
    expect(events.every((event) => event.ppq === 0 && event.data1 === 0)).toBe(true);
  });

  it('routes MIDI events through the native MidiRouter', () => {
    const routed = Project.midiRouteEvents(
      [
        Project.midiNoteOn(0, 0, 3, 60, 100),
        Project.midiNoteOff(0.5, 0, 3, 60, 0),
        Project.midiNoteOn(1, 0, 2, 61, 100),
      ],
      { filterChannel: 3, remapChannel: 7 },
    );
    expect(routed.overflowed).toBe(false);
    expect(routed.overflowCount).toBe(0);
    expect(routed.events).toHaveLength(2);
    expect(routed.events.map((event) => event.ppq)).toEqual([0, 0.5]);
    expect(routed.events.map((event) => (event.data0 >>> 16) & 0xf)).toEqual([7, 7]);
    expect((routed.events[1].data0 >>> 20) & 0xf).toBe(0x8);

    const muted = Project.midiRouteEvents([Project.midiNoteOn(0, 0, 0, 60, 100)], {
      thru: false,
    });
    expect(muted.events).toHaveLength(0);
    expect(muted.overflowed).toBe(false);
  });

  it('exposes native CcMap learn and CC/automation conversion helpers', () => {
    const learned14 = Project.midiCcLearn(
      [Project.midiCc(0, 0, 2, 1, 64), Project.midiCc(0.1, 0, 2, 33, 12)],
      77,
      { minValue: -1, maxValue: 1 },
    );
    expect(learned14).toMatchObject({
      kind: 1,
      ccNumber: 1,
      ccLsbNumber: 33,
      channel: 2,
      paramId: 77,
      minValue: -1,
      maxValue: 1,
    });

    const learnedRpn = Project.midiCcLearn(
      [
        Project.midiCc(0, 0, 3, 101, 0),
        Project.midiCc(0.1, 0, 3, 100, 1),
        Project.midiCc(0.2, 0, 3, 6, 64),
      ],
      78,
    );
    expect(learnedRpn).toMatchObject({ kind: 2, selectorMsb: 0, selectorLsb: 1, paramId: 78 });

    const binding = {
      ccNumber: 74,
      channel: 4,
      kind: 0 as const,
      paramId: 88,
      minValue: -60,
      maxValue: 0,
    };
    const point = Project.midiCcToBreakpoint([binding], Project.midiCc(2, 0, 4, 74, 127));
    expect(point).toEqual({ ppq: 2, value: 0, curveToNext: 0 });

    const event = Project.midiParamToCc([binding], 88, -60, 0, 3);
    expect(event?.ppq).toBe(3);
    expect(((event?.data0 ?? 0) >>> 16) & 0xf).toBe(4);
    expect(((event?.data0 ?? 0) >>> 8) & 0x7f).toBe(74);
    expect((event?.data0 ?? 0) & 0x7f).toBe(0);
  });

  it('follows a tempo that moves only when asked to', () => {
    // Without adaptiveTempo the tracker fits one tempo to the whole take, so the
    // map it produces describes the take's average rather than its shape.
    const audio = accelerando();
    const segmentsFor = (options?: Record<string, unknown>) => {
      const project = Project.create();
      try {
        project.autoTempo(audio, 22050, 0, false, options);
        return Array.from({ length: project.tempoSegmentCount() }, (_, i) =>
          project.tempoSegmentByIndex(i),
        );
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
    const project = Project.create();
    try {
      expect(() =>
        project.autoTempo(audio, 22050, 0, false, { tempoUpdateIntervalBeats: 0 }),
      ).toThrow();
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
