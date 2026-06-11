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

  function danglingSourceJson(): string {
    return '{"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,"channel_strip_ref":"","output_target":"","midi_destination_id":0,"automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,"start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,"fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0,"warp_mode":0}]}';
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
      project.setClipWarpMode(clipId, 'repitch');
      const after = project.toJson();
      expect(after).not.toBe(before);
      expect(after).toContain('"warp_ref_id":123');
      expect(after).toContain('"warp_mode":1');
      const restored = Project.fromJson(after);
      expect(restored.toJson()).toBe(after);
      restored.delete();
      project.undo();
      expect(project.toJson()).toContain('"warp_mode":0');
      project.undo();
      expect(project.toJson()).toBe(before);
      project.setClipWarpMode(clipId, 'tempo-sync');
      expect(project.toJson()).toContain('"warp_mode":2');
      expect(() => project.setClipWarpMode(clipId, 'typo' as 'repitch')).toThrow();
      expect(() => project.setClipWarpMode(clipId, 99 as 1)).toThrow();
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

  it('surfaces compile diagnostic messages per diagnostic', () => {
    const project = buildProject();
    try {
      const result = project.compile();
      expect(result.hasTimeline).toBe(true);
      expect(result.diagnostics.length).toBe(1);
      expect(result.diagnostics[0].code).toBe(10);
      expect(result.diagnostics[0].message).toContain('project contains MIDI clips');
      expect(result.messages.split('\n')[0]).toBe(result.diagnostics[0].message);
    } finally {
      project.delete();
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

  function buildMidiOnlyProject(): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 4);
    // A short held note so the built-in synth produces sustained tone.
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(3, 0, 0, 60, 0),
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

  it('bounces a MIDI-only project through the built-in instrument to non-silent audio', () => {
    const project = buildMidiOnlyProject();
    try {
      // Without an instrument the same MIDI project bounces to silence.
      const silent = project.bounce({ totalFrames: 48000, numChannels: 2, sampleRate: 48000 });
      expect(silent).toBeInstanceOf(Float32Array);
      expect(maxAbs(silent)).toBe(0);

      // Routing the built-in synth makes it audible.
      const audible = project.bounceWithBuiltinInstrument(
        { waveform: 'saw', gain: 0.5 },
        { totalFrames: 48000, numChannels: 2, sampleRate: 48000 },
      );
      expect(audible).toBeInstanceOf(Float32Array);
      expect(audible.length).toBe(48000 * 2);
      expect(maxAbs(audible)).toBeGreaterThan(0.01);
    } finally {
      project.delete();
    }
  });

  it('keeps the base preset when synth patch numeric fields are explicitly zero', () => {
    const project = buildMidiOnlyProject();
    try {
      const preset = project.bounceWithBuiltinInstrument('warm-pad', {
        totalFrames: 4096,
        numChannels: 2,
        sampleRate: 48000,
      });
      const explicitZero = project.bounceWithBuiltinInstrument(
        { preset: 'warm-pad', ampSustain: 0, filterSustain: 0, gain: 0 },
        { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
      );
      expect(explicitZero).toEqual(preset);
    } finally {
      project.delete();
    }
  });

  it('bounceWithBuiltinInstrument auto-derives length when totalFrames is omitted', () => {
    const project = buildMidiOnlyProject();
    try {
      // No totalFrames: the C ABI auto-derives the render length from the
      // arrangement (musical end + synth release tail).
      const audio = project.bounceWithBuiltinInstrument({}, { numChannels: 2, sampleRate: 48000 });
      expect(audio).toBeInstanceOf(Float32Array);
      expect(audio.length).toBeGreaterThan(0);
      expect(maxAbs(audio)).toBeGreaterThan(0.01);
    } finally {
      project.delete();
    }
  });

  it('bounce auto-derives length for a MIDI project when totalFrames is omitted', () => {
    const project = buildMidiOnlyProject();
    try {
      // The README WASM quick-start shape: bounce({ numChannels }) with no
      // totalFrames must now produce a buffer (auto-derived length), not error.
      const audio = project.bounce({ numChannels: 2, sampleRate: 48000 });
      expect(audio).toBeInstanceOf(Float32Array);
      expect(audio.length).toBeGreaterThan(0);
    } finally {
      project.delete();
    }
  });

  it('bounceWithBuiltinInstrument accepts a default patch and per-destination array', () => {
    const project = buildMidiOnlyProject();
    try {
      // Default ({}) sine patch.
      const def = project.bounceWithBuiltinInstrument({}, { totalFrames: 24000, numChannels: 1 });
      expect(def.length).toBe(24000);
      expect(maxAbs(def)).toBeGreaterThan(0.01);

      // Array of bindings (single destination 0 here).
      const arr = project.bounceWithBuiltinInstrument([{ destinationId: 0, waveform: 'square' }], {
        totalFrames: 24000,
        numChannels: 1,
      });
      expect(arr.length).toBe(24000);
      expect(maxAbs(arr)).toBeGreaterThan(0.01);
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
      project.bakeMidiFx(
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

  it('validates MIDI note pairing and detects hanging notes', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      // A well-paired note-on / note-off.
      project.setMidiEvents(clipId, [
        Project.midiNoteOn(0, 0, 0, 60, 100),
        Project.midiNoteOff(2, 0, 0, 60, 0),
      ]);
      const paired = project.validateMidiNotes(clipId);
      expect(paired.ok).toBe(true);
      expect(paired.unmatchedNoteOns).toBe(0);
      expect(paired.unmatchedNoteOffs).toBe(0);

      // A single hanging note-on (no matching note-off).
      project.setMidiEvents(clipId, [Project.midiNoteOn(0, 0, 0, 60, 100)]);
      const hanging = project.validateMidiNotes(clipId);
      expect(hanging.ok).toBe(false);
      expect(hanging.unmatchedNoteOns).toBe(1);
      expect(hanging.unmatchedNoteOffs).toBe(0);

      // UMP groups are independent endpoint namespaces.
      project.setMidiEvents(clipId, [
        Project.midiNoteOn(0, 0, 0, 60, 100),
        Project.midiNoteOff(1, 1, 0, 60, 0),
      ]);
      const crossGroup = project.validateMidiNotes(clipId);
      expect(crossGroup.ok).toBe(false);
      expect(crossGroup.unmatchedNoteOns).toBe(1);
      expect(crossGroup.unmatchedNoteOffs).toBe(1);
    } finally {
      project.delete();
    }
  });

  it('throws when validateMidiNotes targets a non-MIDI clip', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
      const clipId = project.addClip({ trackId, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
      expect(() => project.validateMidiNotes(clipId)).toThrow();
    } finally {
      project.delete();
    }
  });

  it('throws cleanly on malformed fromJson input', () => {
    expect(() => Project.fromJson('{ not valid project json')).toThrow();
  });

  it('fromJsonWithDiagnostics returns warnings from successful loads', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(danglingSourceJson());
    try {
      expect(diagnostics).toContain('dangling_clip_source');
      expect(project.trackCount()).toBe(1);
    } finally {
      project.delete();
    }
  });

  // Golden vectors for the hand-written UMP MIDI-1.0 channel-voice packing
  // (Project.midiNoteOn/Off/Cc/... build data0 in TS rather than via the native
  // packer). These exact words are what sonare::midi::make_midi1_* — and thus
  // the C-ABI sonare_midi_* packers Python delegates to — produce, so pinning
  // them here (with the identical vectors mirrored in the Node suite) makes any
  // silent drift between the two hand-written JS copies a CI failure.
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
      expect(events.map((event) => event.data0 >>> 0)).toEqual([
        0x20b30079, 0x20b32001, 0x20c31800,
      ]);
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
  });
});
