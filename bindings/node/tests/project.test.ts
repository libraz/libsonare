import { describe, expect, it } from 'vitest';
import { EXPECTED_PROJECT_ABI_VERSION, Project, projectAbiVersion } from '../src/index.js';

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

describe('Project native binding', () => {
  it('reports the expected project ABI version', () => {
    expect(projectAbiVersion()).toBe(EXPECTED_PROJECT_ABI_VERSION);
    expect(projectAbiVersion()).toBeGreaterThan(0);
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

  it('sets a clip warp reference and undoes it', () => {
    const project = Project.create();
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();

    project.setClipWarpRef(clipId, 123);
    const after = project.toJson();
    expect(after).not.toBe(before);
    expect(after).toContain('"warp_ref_id":123');

    project.undo();
    expect(project.toJson()).toBe(before);

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
    project.setMidiFx(
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
      if (a > p) p = a;
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

  it('accepts a bare waveform name and an explicit bindings array (instrument-first)', () => {
    const project = buildMidiOnlyProject();
    const byName = project.bounceWithBuiltinInstrument('sine', {
      totalFrames: 2048,
      numChannels: 1,
      sampleRate: 48000,
    });
    expect(peak(byName)).toBeGreaterThan(0);

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

  it('rejects an unknown built-in synth waveform name', () => {
    const project = buildMidiOnlyProject();
    expect(() =>
      project.bounceWithBuiltinInstrument({
        waveform: 'noise' as unknown as 'sine',
      }),
    ).toThrow(/waveform/);
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
});
