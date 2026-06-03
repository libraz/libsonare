/**
 * Headless DAW project WASM binding tests.
 *
 * The WASM `Project` binding mirrors the Node/Python project surface and drives
 * the same C ABI through embind.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { EXPECTED_PROJECT_ABI_VERSION, init, Project, projectAbiVersion } from '../dist/index.js';

describe('Sonare WASM Project', () => {
  beforeAll(async () => {
    await init();
  });

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

  it('reports the expected project ABI version', () => {
    expect(projectAbiVersion()).toBe(EXPECTED_PROJECT_ABI_VERSION);
    expect(projectAbiVersion()).toBeGreaterThan(0);
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
        restored.delete();
      }
    } finally {
      project.delete();
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
        restored.delete();
      }
    } finally {
      project.delete();
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
      expect(after).toContain('"midi_destination_id":7');
      project.undo();
      expect(project.toJson()).toBe(before);
    } finally {
      project.delete();
    }
  });

  it('sets a clip warp reference and undoes it', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
      const before = project.toJson();
      project.setClipWarpRef(clipId, 123);
      const after = project.toJson();
      expect(after).not.toBe(before);
      expect(after).toContain('"warp_ref_id":123');
      project.undo();
      expect(project.toJson()).toBe(before);
    } finally {
      project.delete();
    }
  });

  it('compiles an empty project into a renderable timeline', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const result = project.compile();
      expect(result.hasTimeline).toBe(true);
      expect(typeof result.diagnosticCount).toBe('number');
      expect(typeof result.messages).toBe('string');
      expect(Array.isArray(result.diagnostics)).toBe(true);
      expect(result.diagnostics.length).toBe(result.diagnosticCount);
    } finally {
      project.delete();
    }
  });

  it('bounces deterministically (same-build repeatability)', () => {
    const project = buildProject();
    try {
      project.setSampleRate(48000);
      // total_frames must be > 0 for the C-ABI bounce (an empty/zero render is
      // rejected). An empty project bounces to silence; that is enough to assert
      // same-build repeatability. We compare within a small tolerance rather than
      // requiring cross-platform bit-exactness.
      const options = { totalFrames: 256, blockSize: 128, numChannels: 2, sampleRate: 48000 };

      const first = project.bounce(options);
      const second = project.bounce(options);

      expect(first).toBeInstanceOf(Float32Array);
      expect(first.length).toBe(256 * 2);
      expect(second.length).toBe(first.length);
      for (let i = 0; i < first.length; i++) {
        expect(second[i]).toBeCloseTo(first[i], 6);
      }
    } finally {
      project.delete();
    }
  });

  it('exports MIDI, applies program/MIDI FX, and preserves imported SysEx payloads', () => {
    const project = buildProject();
    try {
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
      expect(smf[0]).toBe(0x4d);
      expect(Array.from(smf).join(',')).toContain('192,42');
      expect(Array.from(smf).join(',')).toContain('195,24');
    } finally {
      project.delete();
    }

    const sysexProject = new Project();
    try {
      const firstClip = sysexProject.importSmf(makeSysexSmf());
      expect(firstClip).toBeGreaterThan(0);
      const json = sysexProject.toJson();
      expect(json).toContain('__sysex_payloads');
      const restored = Project.fromJson(json);
      try {
        expect(Array.from(restored.exportSmf()).join(',')).toContain('240,5,126,127,9,1,247');
      } finally {
        restored.delete();
      }
    } finally {
      sysexProject.delete();
    }
  });

  it('round-trips a MIDI 2.0 Clip File through the binding', () => {
    const project = buildProject();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      // A MIDI 2.0 note-on (message type 0x4) with a full 16-bit velocity.
      project.setMidiEvents(clipId, [
        { ppq: 0, data0: 0x40903c00, data1: 0xbeef0000 },
        { ppq: 1, data0: 0x40803c00, data1: 0 },
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
        reimported.delete();
      }
    } finally {
      project.delete();
    }
  });

  it('validates MIDI event inputs before native conversion', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 1);

      expect(() =>
        project.setMidiEvents(clipId, [{ ppq: Number.NaN, data0: 0, data1: 0 }]),
      ).toThrow(/ppq/);
      expect(() => project.setMidiEvents(clipId, [{ ppq: 0, data0: -1, data1: 0 }])).toThrow(
        /data0/,
      );
      expect(() =>
        project.setMidiEvents(clipId, [[0, 0] as unknown as [number, number, number]]),
      ).toThrow(/\[ppq, data0, data1\]/);
    } finally {
      project.delete();
    }
  });

  it('throws cleanly on malformed fromJson input', () => {
    expect(() => Project.fromJson('{ not valid project json')).toThrow();
  });
});
