import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  ARTICULATIONS,
  BUILTIN_SYNTH_WAVEFORMS,
  CONTROLLER_AXES,
  CONTROLLER_INPUTS,
  MPE_DIMENSIONS,
  midiNoteOff,
  midiNoteOn,
  midiProgram,
  NOTE_TRACKINGS,
  PART_RIG_MODES,
  Project,
  type ProjectBounceOptions,
  RealtimeEngine,
  SAMPLE_KEY_TRACKS,
  SAMPLE_LOOP_MODES,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_LFO_SHAPES,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
  SYNTH_RETRIGGERS,
  synthEngineParamInfo,
  synthEnumTables,
  synthPatchParamInfo,
  synthPresetNames,
  synthPresetPatch,
} from '../src/index.js';
import { addon } from '../src/native.js';
import type { SynthParamInfo, SynthPatch } from '../src/types.js';

/** The addon's own patch reader, which still takes an ordinal the facades refuse. */
function synthPatchRoundTripForTest(patch: object): SynthPatch {
  return addon._synthPatchRoundTrip(patch);
}

function buildMidiOnlyProject(note = 60): Project {
  const project = new Project();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note, velocity: 100 }),
    midiNoteOff({ ppq: 2, group: 0, channel: 0, note, velocity: 0 }),
  ]);
  return project;
}

function buildGmProgramProject(program = 4): Project {
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

// One note played twice, the second long after the first has ended and at an
// offset that is no whole number of its periods.
function buildRepeatedNoteProject(): Project {
  const project = new Project();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 16);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 48, velocity: 100 }),
    midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 48, velocity: 0 }),
    midiNoteOn({ ppq: 9.37, group: 0, channel: 0, note: 48, velocity: 100 }),
    midiNoteOff({ ppq: 10.37, group: 0, channel: 0, note: 48, velocity: 0 }),
  ]);
  return project;
}

// Largest difference between the two plays, each aligned on its own onset.
function repeatedNoteDiff(planes: Float32Array[]): number {
  const [audio] = planes;
  const onset = (from: number) => {
    for (let i = from; i < audio.length; i++) {
      if (Math.abs(audio[i]) > 1e-4) {
        return i;
      }
    }
    return -1;
  };
  const first = onset(0);
  const second = onset(first + 2 * 48000);
  expect(first).toBeGreaterThanOrEqual(0);
  expect(second).toBeGreaterThan(first);
  let diff = 0;
  for (const plane of planes) {
    for (let i = 0; i < 24000; i++) {
      diff = Math.max(diff, Math.abs(plane[first + i] - plane[second + i]));
    }
  }
  return diff;
}

/** Bounces one patch (or preset name) bound to destination 0 and returns the audio planes. */
function bounceSynth(
  project: Project,
  patch: SynthPatch | string,
  options: ProjectBounceOptions,
): Float32Array[] {
  const instrument = typeof patch === 'string' ? { preset: patch } : patch;
  return project.bounce({ ...options, synthInstruments: [{ instrument }] }).channels;
}

function peak(audio: Float32Array | Float32Array[]): number {
  if (Array.isArray(audio)) {
    return Math.max(0, ...audio.map(peak));
  }
  let p = 0;
  for (let i = 0; i < audio.length; i++) {
    const a = Math.abs(audio[i]);
    if (a > p) {
      p = a;
    }
  }
  return p;
}

describe('NativeSynth preset catalog', () => {
  it('lists the catalog names', () => {
    const names = synthPresetNames();
    for (const expected of [
      'sine',
      'sawLead',
      'warmPad',
      'ePiano',
      'electricGuitar',
      'harp',
      'marimba',
      'organ',
      'drumKit',
      'acousticPiano',
    ]) {
      expect(names).toContain(expected);
    }
  });

  it('fetches preset patches with canonical enum names', () => {
    const pad = synthPresetPatch('warmPad');
    expect(pad.preset).toBe('warmPad');
    expect(pad.engineMode).toBe('subtractive');
    expect(pad.waveform).toBe('saw');
    expect(pad.unison).toBe(2);
    expect(pad.stereoSpread).toBeGreaterThan(0);
    // The "va:" routing prefix is accepted.
    expect(synthPresetPatch('va:ePiano').engineMode).toBe('fm');
    expect(synthPresetPatch('acousticPiano').engineMode).toBe('piano');
    expect(synthPresetPatch('clarinet').engineMode).toBe('reed');
    expect(() => synthPresetPatch('no-such-preset')).toThrow();
  });

  it('keeps every NativeSynth enum table in parity with native round-trip ordinals', () => {
    expect(synthEnumTables()).toEqual({
      engineModes: [...SYNTH_ENGINE_MODES],
      waveforms: [...SYNTH_OSC_WAVEFORMS],
      builtinWaveforms: [...BUILTIN_SYNTH_WAVEFORMS],
      filterModels: [...SYNTH_FILTER_MODELS],
      filterOutputs: [...SYNTH_FILTER_OUTPUTS],
      bodyTypes: [...SYNTH_BODY_TYPES],
      modSources: [...SYNTH_MOD_SOURCES],
      modDestinations: [...SYNTH_MOD_DESTINATIONS],
      controllerInputs: [...CONTROLLER_INPUTS],
      controllerAxes: [...CONTROLLER_AXES],
      articulations: [...ARTICULATIONS],
      mpeDimensions: [...MPE_DIMENSIONS],
      noteTrackings: [...NOTE_TRACKINGS],
      sampleLoopModes: [...SAMPLE_LOOP_MODES],
      sampleKeyTracks: [...SAMPLE_KEY_TRACKS],
      retriggers: [...SYNTH_RETRIGGERS],
      lfoShapes: [...SYNTH_LFO_SHAPES],
      partRigModes: [...PART_RIG_MODES],
      sampleDescLoopModes: ['none', 'continuous', 'keyDown'],
    });

    for (const [ordinal, name] of SYNTH_ENGINE_MODES.entries()) {
      expect(synthPatchRoundTripForTest({ engineMode: name }).engineMode).toBe(name);
      expect(synthPatchRoundTripForTest({ engineMode: ordinal }).engineMode).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_OSC_WAVEFORMS.entries()) {
      expect(synthPatchRoundTripForTest({ waveform: name }).waveform).toBe(name);
      expect(synthPatchRoundTripForTest({ waveform: ordinal }).waveform).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_FILTER_MODELS.entries()) {
      expect(synthPatchRoundTripForTest({ filterModel: name }).filterModel).toBe(name);
      expect(synthPatchRoundTripForTest({ filterModel: ordinal }).filterModel).toBe(name);
    }
    // The pre-rename spelling is refused like any other unknown name.
    expect(() =>
      synthPatchRoundTripForTest({ filterModel: 'moog-ladder' } as unknown as SynthPatch),
    ).toThrow(RangeError);
    for (const [ordinal, name] of SYNTH_FILTER_OUTPUTS.entries()) {
      expect(synthPatchRoundTripForTest({ filterOutput: name }).filterOutput).toBe(name);
      expect(synthPatchRoundTripForTest({ filterOutput: ordinal }).filterOutput).toBe(name);
    }
    for (const retired of ['lowpass', 'bandpass', 'highpass']) {
      expect(() =>
        synthPatchRoundTripForTest({ filterOutput: retired } as unknown as SynthPatch),
      ).toThrow(RangeError);
    }
    for (const [ordinal, name] of SYNTH_BODY_TYPES.entries()) {
      expect(synthPatchRoundTripForTest({ body: name }).body).toBe(name);
      expect(synthPatchRoundTripForTest({ body: ordinal }).body).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_MOD_SOURCES.entries()) {
      const byName = synthPatchRoundTripForTest({
        modRoutings: [{ source: name, destination: 'pitchCents', depth: 1 }],
      });
      const byOrdinal = synthPatchRoundTripForTest({
        modRoutings: [{ source: ordinal, destination: 'pitchCents', depth: 1 }],
      });
      expect(byName.modRoutings?.[0]?.source).toBe(name);
      expect(byOrdinal.modRoutings?.[0]?.source).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_MOD_DESTINATIONS.entries()) {
      const byName = synthPatchRoundTripForTest({
        modRoutings: [{ source: 'lfo1', destination: name, depth: 1 }],
      });
      const byOrdinal = synthPatchRoundTripForTest({
        modRoutings: [{ source: 'lfo1', destination: ordinal, depth: 1 }],
      });
      expect(byName.modRoutings?.[0]?.destination).toBe(name);
      expect(byOrdinal.modRoutings?.[0]?.destination).toBe(name);
    }
  });

  it('round-trips the retrigger mode by name and ordinal and refuses unknown values', () => {
    for (const [ordinal, name] of SYNTH_RETRIGGERS.entries()) {
      expect(synthPatchRoundTripForTest({ retrigger: name }).retrigger).toBe(name);
      expect(synthPatchRoundTripForTest({ retrigger: ordinal }).retrigger).toBe(name);
    }
    expect(synthPatchRoundTripForTest({}).retrigger).toBe('default');
    expect(() =>
      synthPatchRoundTripForTest({ retrigger: 'phase' } as unknown as SynthPatch),
    ).toThrow(RangeError);
    expect(() => synthPatchRoundTripForTest({ retrigger: SYNTH_RETRIGGERS.length })).toThrow(
      RangeError,
    );
    expect(synthPresetPatch('sawLead').retrigger).toBe('free');
  });

  it('round-trips the LFO shape by name and ordinal and refuses unknown values', () => {
    expect(SYNTH_LFO_SHAPES).toEqual(['default', 'triangle', 'sine', 'random']);
    for (const [ordinal, name] of SYNTH_LFO_SHAPES.entries()) {
      expect(synthPatchRoundTripForTest({ lfoShape: name }).lfoShape).toBe(name);
      expect(synthPatchRoundTripForTest({ lfoShape: ordinal }).lfoShape).toBe(name);
    }
    expect(synthPatchRoundTripForTest({}).lfoShape).toBe('default');
    expect(() =>
      synthPatchRoundTripForTest({ lfoShape: 'square' } as unknown as SynthPatch),
    ).toThrow(RangeError);
    expect(() => synthPatchRoundTripForTest({ lfoShape: SYNTH_LFO_SHAPES.length })).toThrow(
      RangeError,
    );
    expect(synthPresetPatch('sawLead').lfoShape).toBe('triangle');
  });

  it('reads a null enum field as absent and refuses a wrong type by name', () => {
    // The reader family's contract: only undefined/null take the default.
    const base = synthPatchRoundTripForTest({});
    const fields = [
      ['engineMode', 'synth engine mode'],
      ['waveform', 'oscillator waveform'],
      ['filterModel', 'filter model'],
      ['filterOutput', 'filter output'],
    ] as const;
    for (const [field, label] of fields) {
      const absent = synthPatchRoundTripForTest({ [field]: null } as unknown as SynthPatch);
      expect(absent[field], field).toBe(base[field]);
      expect(() => synthPatchRoundTripForTest({ [field]: true } as unknown as SynthPatch)).toThrow(
        label,
      );
    }
  });

  it('reads an absent preset back as absent and refuses an empty, over-long or non-string one', () => {
    for (const preset of [undefined, null]) {
      const patch = synthPatchRoundTripForTest({ preset } as unknown as SynthPatch);
      expect('preset' in patch, String(preset)).toBe(false);
    }
    for (const preset of ['', 'va:']) {
      expect(() => synthPatchRoundTripForTest({ preset }), preset).toThrow(
        /synth patch preset must not be empty; omit it for the init patch/,
      );
    }
    expect(() => synthPatchRoundTripForTest({ preset: 'x'.repeat(32) })).toThrow(
      /synth patch preset must be at most 31 bytes/,
    );
    expect(() => synthPatchRoundTripForTest({ preset: 123 } as unknown as SynthPatch)).toThrow(
      /synth patch preset must be a string/,
    );
  });
});

describe('Project.bounce with synthInstruments', () => {
  it('renders preset patches deterministically', () => {
    const project = buildMidiOnlyProject();
    try {
      for (const preset of ['va:sawLead', 'ePiano', 'harp']) {
        const audio = bounceSynth(project, preset, { totalFrames: 24000 });
        expect(audio.map((plane) => plane.length)).toEqual([24000, 24000]);
        expect(peak(audio)).toBeGreaterThan(0);
      }
      const first = bounceSynth(project, 'sawLead', { totalFrames: 24000 });
      const second = bounceSynth(project, 'sawLead', { totalFrames: 24000 });
      expect(first).toEqual(second);
    } finally {
      project.destroy();
    }
  });

  it('takes an explicit zero as an override, not as "keep the base"', () => {
    const project = buildMidiOnlyProject();
    try {
      // warmPad carries a non-zero stereo spread and bus drive, so turning
      // either off is a real edit. Before the patch carried presence bits a
      // zero was indistinguishable from an omitted key and the render was
      // unchanged, which made "no spread" impossible to ask for.
      const reference = bounceSynth(project, 'warmPad', { totalFrames: 24000 });
      expect(peak(reference)).toBeGreaterThan(0);

      const noSpread = bounceSynth(
        project,
        { preset: 'warmPad', stereoSpread: 0 },
        { totalFrames: 24000 },
      );
      expect(noSpread).not.toEqual(reference);

      const noDrive = bounceSynth(
        project,
        { preset: 'warmPad', busDrive: 0 },
        { totalFrames: 24000 },
      );
      expect(noDrive).not.toEqual(reference);

      // Omitting the key still keeps the base value.
      const untouched = bounceSynth(project, { preset: 'warmPad' }, { totalFrames: 24000 });
      expect(untouched).toEqual(reference);

      // An empty routing array clears the base matrix; omitting it keeps it.
      const wobble = bounceSynth(
        project,
        {
          preset: 'warmPad',
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      const cleared = bounceSynth(
        project,
        { preset: 'warmPad', lfoRateHz: 6, modRoutings: [] },
        { totalFrames: 24000 },
      );
      expect(cleared).not.toEqual(wobble);
    } finally {
      project.destroy();
    }
  });

  it('sets drift rate, envelope delay and hold and one-shot, and reads them back', () => {
    const preset = synthPresetPatch('sawLead');
    expect(preset.driftRateHz).toBeGreaterThan(0);
    for (const key of ['ampDelayMs', 'ampHoldMs', 'filterDelayMs', 'filterHoldMs'] as const) {
      expect(typeof preset[key]).toBe('number');
    }
    expect(preset.oneShot).toBe(false);
    const echoed = synthPatchRoundTripForTest({ ampHoldMs: 12, filterDelayMs: 34, oneShot: true });
    expect(echoed.ampHoldMs).toBe(12);
    expect(echoed.filterDelayMs).toBe(34);
    expect(echoed.oneShot).toBe(true);

    const project = buildMidiOnlyProject();
    try {
      // Long enough to pass the note-off, so a one-shot that ignores it differs.
      const frames = { totalFrames: 72000 };
      const base: SynthPatch = {
        preset: 'sawLead',
        driftCents: 40,
        ampDecayMs: 200,
        ampSustain: 0.5,
        cutoffHz: 1500,
        envToCutoffCents: 2400,
        filterDecayMs: 200,
        filterSustain: 0.3,
      };
      const reference = bounceSynth(project, base, frames);
      expect(peak(reference)).toBeGreaterThan(0);
      for (const edit of [
        { driftRateHz: 7 },
        { ampDelayMs: 80 },
        { ampHoldMs: 80 },
        { filterDelayMs: 80 },
        { filterHoldMs: 80 },
        { oneShot: true },
        // The lowest rate the field accepts, which the base does not use.
        { driftRateHz: 0.01 },
      ]) {
        const edited = bounceSynth(project, { ...base, ...edit }, frames);
        expect(edited, JSON.stringify(edit)).not.toEqual(reference);
      }
      expect(bounceSynth(project, { ...base, oneShot: undefined }, frames)).toEqual(reference);
      expect(() => bounceSynth(project, { ...base, oneShot: 1 as never }, frames)).toThrow(
        TypeError,
      );
    } finally {
      project.destroy();
    }
  });

  it('takes gain: 0 as silence, not the base level', () => {
    const project = buildMidiOnlyProject();
    try {
      const reference = bounceSynth(project, 'warmPad', { totalFrames: 24000 });
      expect(peak(reference)).toBeGreaterThan(0);

      const silent = bounceSynth(project, { preset: 'warmPad', gain: 0 }, { totalFrames: 24000 });
      expect(peak(silent)).toBe(0);

      const quiet = bounceSynth(project, { preset: 'warmPad', gain: 0.01 }, { totalFrames: 24000 });
      expect(peak(quiet)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('refuses a field outside the range synthPatchParamInfo publishes, naming it', () => {
    const project = buildMidiOnlyProject();
    try {
      const rejection = (patch: SynthPatch): { code?: number; name?: string; message: string } => {
        try {
          bounceSynth(project, patch, { totalFrames: 128 });
        } catch (error) {
          return error as { code?: number; name?: string; message: string };
        }
        throw new Error('expected the bounce to be refused');
      };
      const info = synthPatchParamInfo();
      const cutoff = info.find(
        (field): field is SynthParamInfo & { type: 'number' } =>
          field.name === 'cutoffHz' && field.type === 'number',
      );
      expect(cutoff).toBeDefined();
      expect(cutoff?.type).toBe('number');
      expect(cutoff?.max).toBe(22000);
      for (const [patch, key] of [
        [{ cutoffHz: 1e6 }, 'cutoffHz'],
        [{ stereoSpread: 5 }, 'stereoSpread'],
        [{ polyphony: 500 }, 'polyphony'],
        [{ gain: 5 }, 'gain'],
      ] as [SynthPatch, string][]) {
        const error = rejection(patch);
        expect(error.name, key).toBe('RangeError');
        expect(error.message, key).toContain(`patch.${key} must be `);
      }
      // The published ends are accepted.
      expect(() =>
        bounceSynth(project, { cutoffHz: cutoff?.max }, { totalFrames: 128 }),
      ).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('refuses a mod routing naming none on either end', () => {
    const project = buildMidiOnlyProject();
    try {
      expect(() =>
        bounceSynth(
          project,
          { modRoutings: [{ source: 'none', destination: 'pitchCents', depth: 80 }] },
          { totalFrames: 128 },
        ),
      ).toThrow();
      expect(() =>
        bounceSynth(
          project,
          { modRoutings: [{ source: 'lfo1', destination: 'none', depth: 80 }] },
          { totalFrames: 128 },
        ),
      ).toThrow();
      // Not vacuous: a real routing on both ends still passes.
      const audio = bounceSynth(
        project,
        { modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }] },
        { totalFrames: 24000 },
      );
      expect(peak(audio)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('applies field overrides and the mod matrix', () => {
    const project = buildMidiOnlyProject();
    try {
      const plain = bounceSynth(project, {}, { totalFrames: 24000 });
      expect(peak(plain)).toBeGreaterThan(0);
      const dark = bounceSynth(project, { cutoffHz: 300, resonanceQ: 4 }, { totalFrames: 24000 });
      expect(dark).not.toEqual(plain);
      const wobble = bounceSynth(
        project,
        {
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      expect(wobble).not.toEqual(plain);
      expect(() => bounceSynth(project, 'no-such-preset', { totalFrames: 128 })).toThrow();
      expect(() =>
        // @ts-expect-error deliberately unknown waveform name; the patch resolver must reject it.
        bounceSynth(project, { waveform: 'sawtooth-ish' }, { totalFrames: 128 }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('refuses a bare preset name in place of a binding', () => {
    const project = buildMidiOnlyProject();
    try {
      expect(() =>
        project.bounce({ totalFrames: 128, synthInstruments: 'sawLead' as never }),
      ).toThrow(TypeError);
      expect(() =>
        project.bounce({ totalFrames: 128, synthInstruments: [{ preset: 'sawLead' }] as never }),
      ).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('renders a repeated note identically only under note retrigger', () => {
    const project = buildRepeatedNoteProject();
    try {
      const options = { totalFrames: 48000 * 6 };
      const note = bounceSynth(project, { preset: 'sawLead', retrigger: 'note' }, options);
      const free = bounceSynth(project, { preset: 'sawLead', retrigger: 'free' }, options);
      expect(peak(note)).toBeGreaterThan(0.01);
      expect(repeatedNoteDiff(note)).toBe(0);
      expect(repeatedNoteDiff(free)).toBeGreaterThan(1e-3);
    } finally {
      project.destroy();
    }
  });

  it('plays the GM drum map through the drumKit preset', () => {
    // Note 38 = acoustic snare in the GM drum map.
    const project = buildMidiOnlyProject(38);
    try {
      const audio = bounceSynth(project, 'drumKit', { totalFrames: 24000 });
      expect(peak(audio)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('follows GM program changes only when useGmPrograms is enabled', () => {
    const fixedProject = buildGmProgramProject();
    const changedProject = buildGmProgramProject(40);
    try {
      const options = { totalFrames: 12000, numChannels: 1 };
      const fixed = bounceSynth(fixedProject, { preset: 'sine' }, options);
      const explicitFalse = bounceSynth(
        fixedProject,
        { preset: 'sine', useGmPrograms: false },
        options,
      );
      const gm = bounceSynth(fixedProject, { preset: 'sine', useGmPrograms: true }, options);
      const otherGm = bounceSynth(changedProject, { preset: 'sine', useGmPrograms: true }, options);

      expect(explicitFalse).toEqual(fixed);
      expect(gm).not.toEqual(fixed);
      expect(otherGm).not.toEqual(gm);
    } finally {
      fixedProject.destroy();
      changedProject.destroy();
    }
  });

  it('validates useGmPrograms through the shared boolean property reader', () => {
    const project = buildMidiOnlyProject();
    try {
      expect(() =>
        bounceSynth(project, { preset: 'sine', useGmPrograms: 1 } as unknown as SynthPatch, {
          totalFrames: 128,
        }),
      ).toThrow();
      expect(() =>
        bounceSynth(project, { preset: 'sine', useGmPrograms: 'true' } as unknown as SynthPatch, {
          totalFrames: 128,
        }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });
});

describe('RealtimeEngine.setSynthInstrument', () => {
  it('renders live MIDI through a preset patch', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 16,
        telemetryCapacity: 16,
      });
      engine.setSynthInstrument(7, { preset: 'sawLead' });
      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      let p = 0;
      for (const channel of out) {
        for (const sample of channel) {
          p = Math.max(p, Math.abs(sample));
        }
      }
      expect(p).toBeGreaterThan(0);
      // Unknown presets are rejected without disturbing the binding.
      expect(() => engine.setSynthInstrument(7, { preset: 'no-such-preset' })).toThrow();
      expect(engine.midiInstrumentCount).toBe(1);
    } finally {
      engine.destroy();
    }
  });

  it('refuses a preset-name string as the patch and leaves the binding alone', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      expect(() => engine.setSynthInstrument(7, 'sawLead' as never)).toThrow(TypeError);
      expect(engine.midiInstrumentCount).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('binds gain: 0 as silence, not the base level', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 16,
        telemetryCapacity: 16,
      });
      engine.setSynthInstrument(7, { preset: 'warmPad', gain: 0 });
      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      const out = engine.process([new Float32Array(4096), new Float32Array(4096)]);
      let p = 0;
      for (const channel of out) {
        for (const sample of channel) {
          p = Math.max(p, Math.abs(sample));
        }
      }
      expect(p).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  // Mirrors bindings/wasm/tests/realtime-engine-gm.test.ts, so the two facades
  // cannot drift on which incoming MIDI a `useGmPrograms` binding follows.
  it('follows MIDI programs only when useGmPrograms is enabled', () => {
    const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
      (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;
    const maxAbsDifference = (left: Float32Array, right: Float32Array): number => {
      expect(left.length).toBe(right.length);
      let max = 0;
      for (let i = 0; i < left.length; i += 1) {
        max = Math.max(max, Math.abs(left[i] - right[i]));
      }
      return max;
    };

    const blockSize = 128;
    const blockCount = 32;
    const renderProgram = (useGmPrograms: boolean, program: number): Float32Array => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: blockSize });
      try {
        engine.setSynthInstrument(1, { preset: 'sawLead', useGmPrograms });
        engine.setMidiClips([
          {
            id: 1,
            trackId: 1,
            destinationId: 1,
            lengthSamples: blockSize * blockCount,
            events: [
              { renderFrame: 0, word0: midi1Word(0xc, 0, program, 0), wordCount: 1 },
              { renderFrame: 1, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
            ],
          },
        ]);
        engine.play();
        const rendered = new Float32Array(blockSize * blockCount);
        for (let block = 0; block < blockCount; block += 1) {
          const [left] = engine.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
          rendered.set(left, block * blockSize);
        }
        return rendered;
      } finally {
        engine.destroy();
      }
    };

    const gmPiano = renderProgram(true, 0);
    const gmViolin = renderProgram(true, 40);
    expect(Math.max(...gmPiano.map(Math.abs))).toBeGreaterThan(0);
    expect(Math.max(...gmViolin.map(Math.abs))).toBeGreaterThan(0);
    expect(maxAbsDifference(gmPiano, gmViolin)).toBeGreaterThan(1e-4);

    const fixedPiano = renderProgram(false, 0);
    const fixedViolin = renderProgram(false, 40);
    expect(maxAbsDifference(fixedPiano, fixedViolin)).toBe(0);
  });

  it('requires useGmPrograms to be a boolean when supplied', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      expect(() =>
        engine.setSynthInstrument(
          7, // @ts-expect-error Runtime validation intentionally covers a wrong JS type.
          { preset: 'sawLead', useGmPrograms: 1 },
        ),
      ).toThrow(/useGmPrograms/);
    } finally {
      engine.destroy();
    }
  });
});

describe('SynthPatch.engineParams', () => {
  const bounce = (patch: SynthPatch | string): Float32Array[] => {
    const project = buildMidiOnlyProject();
    try {
      return bounceSynth(project, patch, { totalFrames: 24000 });
    } finally {
      project.destroy();
    }
  };
  const rejection = (patch: SynthPatch): { code?: number; name?: string; message: string } => {
    try {
      bounce(patch);
    } catch (error) {
      return error as { code?: number; name?: string; message: string };
    }
    throw new Error('expected the bounce to be refused');
  };

  it('voices every engine mode but sample from a bare engineMode', () => {
    for (const mode of SYNTH_ENGINE_MODES) {
      const audio = bounce({ engineMode: mode });
      if (mode === 'sample') {
        expect(peak(audio)).toBe(0);
      } else {
        expect(peak(audio), mode).toBeGreaterThan(10 ** (-60 / 20));
      }
    }
  });

  it('accepts the subtractive section by key', () => {
    const names = synthEngineParamInfo('subtractive').map((field) => field.name);
    expect(names.sort()).toEqual([
      'bandNoiseHighHz',
      'bandNoiseHighKeyTrack',
      'bandNoiseLevel',
      'bandNoiseLowHz',
      'bandNoiseLowKeyTrack',
      'noiseAm',
      'noiseLevel',
      'osc2Dry',
      'osc2IntervalCents',
      'osc2Level',
      'pulseWidth',
      'sustainKeyBreak',
      'sustainKeySlopeDb',
      'unisonWidth',
      'velocityExponent',
    ]);
    const plain = bounce({ engineMode: 'subtractive' });
    const layered = bounce({
      engineMode: 'subtractive',
      engineParams: { osc2IntervalCents: 702, osc2Level: 0.5, noiseLevel: 0.1 },
    });
    expect(peak(plain)).toBeGreaterThan(0);
    expect(layered).not.toEqual(plain);
    expect(() =>
      bounce({ engineMode: 'subtractive', engineParams: { osc2IntervalCents: 2401 } }),
    ).toThrow(/osc2IntervalCents/);
  });

  it('changes a render when an engine field is set', () => {
    const bare = bounce({ engineMode: 'bowedString' });
    const stronger = bounce({ engineMode: 'bowedString', engineParams: { bowForce: 0.9 } });
    expect(peak(stronger)).toBeGreaterThan(0);
    expect(stronger).not.toEqual(bare);
    // The same params through the realtime-free bounce path are deterministic.
    expect(bounce({ engineMode: 'bowedString', engineParams: { bowForce: 0.9 } })).toEqual(
      stronger,
    );
    // Several bindings each keep their own keys alive.
    const project = buildMidiOnlyProject();
    try {
      const audio = project.bounce({
        totalFrames: 24000,
        synthInstruments: [
          {
            destinationId: 0,
            instrument: { engineMode: 'bowedString', engineParams: { bowForce: 0.9 } },
          },
          {
            destinationId: 1,
            instrument: { engineMode: 'bowedString', engineParams: { bowForce: 0.2 } },
          },
        ],
      });
      expect(peak(audio.channels)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('refuses a bad key or value with a RangeError naming the key', () => {
    const unknown = rejection({ engineMode: 'bowedString', engineParams: { noSuchField: 1 } });
    expect(unknown.name).toBe('RangeError');
    expect(unknown.message).toContain('noSuchField');

    const info = synthEngineParamInfo('bowedString');
    const otherEngine = synthEngineParamInfo('brass').find(
      (field) => !info.some((own) => own.name === field.name),
    );
    expect(otherEngine).toBeDefined();
    const wrongEngine = rejection({
      engineMode: 'bowedString',
      engineParams: { [otherEngine?.name as string]: 0.5 },
    });
    expect(wrongEngine.name).toBe('RangeError');
    expect(wrongEngine.message).toContain(otherEngine?.name as string);

    const bounded = info.find((field) => field.min !== undefined && field.max !== undefined);
    expect(bounded).toBeDefined();
    const outOfRange = rejection({
      engineMode: 'bowedString',
      engineParams: { [bounded?.name as string]: (bounded?.max as number) + 1000 },
    });
    expect(outOfRange.name).toBe('RangeError');
    expect(outOfRange.message).toContain(bounded?.name as string);

    const nonFinite = rejection({
      engineMode: 'bowedString',
      engineParams: { bowForce: Number.NaN },
    });
    expect(nonFinite.name).toBe('RangeError');
    expect(nonFinite.message).toContain('bowForce');
  });

  it('refuses a non-number value by key before the call', () => {
    const wrongType = { engineMode: 'bowedString', engineParams: { bowForce: '0.5' } };
    expect(() => bounce(wrongType as unknown as SynthPatch)).toThrow(/engineParams\.bowForce/);
    expect(() =>
      bounce({ engineMode: 'bowedString', engineParams: [1] } as unknown as SynthPatch),
    ).toThrow(/engineParams/);
  });

  it('is write-only: preset and round-trip reads carry no engineParams', () => {
    expect(synthPresetPatch('violin').engineParams).toBeUndefined();
    const read = synthPatchRoundTripForTest({
      engineMode: 'bowedString',
      engineParams: { bowForce: 0.5 },
    });
    expect(read.engineMode).toBe('bowedString');
    expect(read.engineParams).toBeUndefined();
  });

  it('binds engineParams on a realtime engine and surfaces a refusal', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      engine.setSynthInstrument(0, { engineMode: 'bowedString', engineParams: { bowForce: 0.5 } });
      expect(() =>
        engine.setSynthInstrument(0, {
          engineMode: 'bowedString',
          engineParams: { noSuchField: 1 },
        }),
      ).toThrow(
        expect.objectContaining({
          name: 'RangeError',
          message: expect.stringMatching(/noSuchField/),
        }),
      );
    } finally {
      engine.destroy();
    }
  });
});

describe('synth parameter descriptors', () => {
  it('describes each engine section; engines without one answer empty', () => {
    for (const mode of ['default', 'sample'] as const) {
      expect(synthEngineParamInfo(mode)).toEqual([]);
    }
    const bowed = synthEngineParamInfo('bowedString');
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => synthEngineParamInfo(SYNTH_ENGINE_MODES.indexOf('bowedString') as any)).toThrow(
      TypeError,
    );
    const force = bowed.find((field) => field.name === 'bowForce');
    expect(force).toBeDefined();
    expect(force?.type).toBe('number');
    expect(typeof force?.default).toBe('number');
    expect(typeof force?.unit).toBe('string');
    expect(() => synthEngineParamInfo('no-such-engine' as never)).toThrow(
      new RangeError(
        `synthEngineParamInfo: engineMode must be one of ${SYNTH_ENGINE_MODES.join(', ')}`,
      ),
    );
    expect(() => synthEngineParamInfo(3 as never)).toThrow(TypeError);
  });

  it('states a gate only where a field has one, and a switch default as a boolean', () => {
    const percussion = synthEngineParamInfo('percussion');
    const gmKit = percussion.find((field) => field.name === 'gmKit');
    expect(gmKit?.type).toBe('boolean');
    expect(typeof gmKit?.default).toBe('boolean');
    expect(gmKit && 'activeWhen' in gmKit).toBe(false);
    const kitOff = { key: 'gmKit', scope: 'engine', relation: 'eq', value: false };
    expect(percussion.find((field) => field.name === 'modeRatios6')?.activeWhen).toEqual([
      [kitOff, { key: 'numModes', scope: 'engine', relation: 'gt', value: 6 }],
    ]);
    // Reached through the direct contact or through the plate's: two alternatives.
    expect(percussion.find((field) => field.name === 'contactMs')?.activeWhen).toEqual([
      [kitOff, { key: 'contact', scope: 'engine', relation: 'gt', value: 0 }],
      [
        kitOff,
        { key: 'plateGain', scope: 'engine', relation: 'gt', value: 0 },
        { key: 'plateContact', scope: 'engine', relation: 'gt', value: 0 },
      ],
    ]);
    expect(
      synthEngineParamInfo('bowedString').find((field) => field.name === 'corpusScale')?.activeWhen,
    ).toEqual([
      [
        { key: 'body', scope: 'patch', relation: 'eq', value: 'violin' },
        { key: 'bodyMix', scope: 'patch', relation: 'gt', value: 0 },
      ],
    ]);
    for (const mode of SYNTH_ENGINE_MODES) {
      for (const field of synthEngineParamInfo(mode)) {
        if (field.activeWhen === undefined) {
          continue;
        }
        expect(field.activeWhen.length, `${mode} ${field.name}`).toBeGreaterThan(0);
        for (const alternative of field.activeWhen) {
          expect(alternative.length, `${mode} ${field.name}`).toBeGreaterThan(0);
        }
      }
    }
  });

  it('describes the wrapper fields', () => {
    const fields = synthPatchParamInfo();
    const names = fields.map((field) => field.name);
    for (const expected of ['cutoffHz', 'resonanceQ', 'ampAttackMs', 'gain']) {
      expect(names).toContain(expected);
    }
    const resonance = fields.find(
      (field): field is SynthParamInfo & { type: 'number' } =>
        field.name === 'resonanceQ' && field.type === 'number',
    );
    expect(resonance?.type).toBe('number');
    expect(resonance?.unit).toBe('ratio');
    expect(new Set(names).size).toBe(names.length);
    for (const expected of [
      'driftRateHz',
      'ampDelayMs',
      'ampHoldMs',
      'filterDelayMs',
      'filterHoldMs',
    ]) {
      expect(names).toContain(expected);
    }
    const oneShot = fields.find(
      (field): field is SynthParamInfo & { type: 'boolean' } =>
        field.name === 'oneShot' && field.type === 'boolean',
    );
    expect(oneShot?.type).toBe('boolean');
  });
});

describe('synth parameter metadata', () => {
  it('includes dBTP in the synth parameter unit vocabulary', () => {
    expectTypeOf<Extract<SynthParamInfo['unit'], 'dBTP'>>().toEqualTypeOf<'dBTP'>();
  });
});
