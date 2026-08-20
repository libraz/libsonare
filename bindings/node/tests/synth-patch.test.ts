import { describe, expect, it } from 'vitest';
import {
  BUILTIN_SYNTH_WAVEFORMS,
  Project,
  RealtimeEngine,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
  synthEnumTables,
  synthPresetNames,
  synthPresetPatch,
} from '../src/index.js';
import { addon } from '../src/native.js';
import type { SynthPatch } from '../src/types.js';

function synthPatchRoundTripForTest(patch: SynthPatch): SynthPatch {
  return addon._synthPatchRoundTrip(patch);
}

function buildMidiOnlyProject(note = 60): Project {
  const project = Project.create();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, note, 100),
    Project.midiNoteOff(2, 0, 0, note, 0),
  ]);
  return project;
}

function buildGmProgramProject(program = 4): Project {
  const project = Project.create();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 1);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    Project.midiProgram(0, 0, 0, program),
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(0.5, 0, 0, 60, 0),
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

describe('NativeSynth preset catalog', () => {
  it('lists the catalog names', () => {
    const names = synthPresetNames();
    for (const expected of [
      'sine',
      'saw-lead',
      'warm-pad',
      'e-piano',
      'electric-guitar',
      'harp',
      'marimba',
      'organ',
      'drum-kit',
      'acoustic-piano',
    ]) {
      expect(names).toContain(expected);
    }
  });

  it('fetches preset patches with canonical enum names', () => {
    const pad = synthPresetPatch('warm-pad');
    expect(pad.preset).toBe('warm-pad');
    expect(pad.engineMode).toBe('subtractive');
    expect(pad.waveform).toBe('saw');
    expect(pad.unison).toBe(7);
    expect(pad.stereoSpread).toBeGreaterThan(0);
    // The "va:" routing prefix is accepted.
    expect(synthPresetPatch('va:e-piano').engineMode).toBe('fm');
    expect(synthPresetPatch('acoustic-piano').engineMode).toBe('piano');
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
    for (const [ordinal, name] of SYNTH_FILTER_OUTPUTS.entries()) {
      expect(synthPatchRoundTripForTest({ filterOutput: name }).filterOutput).toBe(name);
      expect(synthPatchRoundTripForTest({ filterOutput: ordinal }).filterOutput).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_BODY_TYPES.entries()) {
      expect(synthPatchRoundTripForTest({ body: name }).body).toBe(name);
      expect(synthPatchRoundTripForTest({ body: ordinal }).body).toBe(name);
    }
    for (const [ordinal, name] of SYNTH_MOD_SOURCES.entries()) {
      const byName = synthPatchRoundTripForTest({
        modRoutings: [{ source: name, destination: 'pitch-cents', depth: 1 }],
      });
      const byOrdinal = synthPatchRoundTripForTest({
        modRoutings: [{ source: ordinal, destination: 'pitch-cents', depth: 1 }],
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

  it('rejects non-string preset properties consistently', () => {
    expect(synthPatchRoundTripForTest({ preset: undefined }).preset).toBe('');
    expect(synthPatchRoundTripForTest({ preset: null } as unknown as SynthPatch).preset).toBe('');
    expect(() => synthPatchRoundTripForTest({ preset: 123 } as unknown as SynthPatch)).toThrow(
      /synth patch preset must be a string/,
    );
  });
});

describe('Project.bounceWithSynthInstrument', () => {
  it('renders preset patches deterministically', () => {
    const project = buildMidiOnlyProject();
    try {
      for (const preset of ['va:saw-lead', 'e-piano', 'harp']) {
        const audio = project.bounceWithSynthInstrument(preset, { totalFrames: 24000 });
        expect(audio.length).toBe(48000);
        expect(peak(audio)).toBeGreaterThan(0);
      }
      const first = project.bounceWithSynthInstrument('saw-lead', { totalFrames: 24000 });
      const second = project.bounceWithSynthInstrument('saw-lead', { totalFrames: 24000 });
      expect(first).toEqual(second);
    } finally {
      project.destroy();
    }
  });

  it('takes an explicit zero as an override, not as "keep the base"', () => {
    const project = buildMidiOnlyProject();
    try {
      // warm-pad carries a non-zero stereo spread and bus drive, so turning
      // either off is a real edit. Before the patch carried presence bits a
      // zero was indistinguishable from an omitted key and the render was
      // unchanged, which made "no spread" impossible to ask for.
      const reference = project.bounceWithSynthInstrument('warm-pad', { totalFrames: 24000 });
      expect(peak(reference)).toBeGreaterThan(0);

      const noSpread = project.bounceWithSynthInstrument(
        { preset: 'warm-pad', stereoSpread: 0 },
        { totalFrames: 24000 },
      );
      expect(noSpread).not.toEqual(reference);

      const noDrive = project.bounceWithSynthInstrument(
        { preset: 'warm-pad', busDrive: 0 },
        { totalFrames: 24000 },
      );
      expect(noDrive).not.toEqual(reference);

      // Omitting the key still keeps the base value.
      const untouched = project.bounceWithSynthInstrument(
        { preset: 'warm-pad' },
        { totalFrames: 24000 },
      );
      expect(untouched).toEqual(reference);

      // An empty routing array clears the base matrix; omitting it keeps it.
      const wobble = project.bounceWithSynthInstrument(
        {
          preset: 'warm-pad',
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitch-cents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      const cleared = project.bounceWithSynthInstrument(
        { preset: 'warm-pad', lfoRateHz: 6, modRoutings: [] },
        { totalFrames: 24000 },
      );
      expect(cleared).not.toEqual(wobble);
    } finally {
      project.destroy();
    }
  });

  it('applies field overrides and the mod matrix', () => {
    const project = buildMidiOnlyProject();
    try {
      const plain = project.bounceWithSynthInstrument({}, { totalFrames: 24000 });
      expect(peak(plain)).toBeGreaterThan(0);
      const dark = project.bounceWithSynthInstrument(
        { cutoffHz: 300, resonanceQ: 4 },
        { totalFrames: 24000 },
      );
      expect(dark).not.toEqual(plain);
      const wobble = project.bounceWithSynthInstrument(
        {
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitch-cents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      expect(wobble).not.toEqual(plain);
      expect(() =>
        project.bounceWithSynthInstrument('no-such-preset', { totalFrames: 128 }),
      ).toThrow();
      expect(() =>
        // @ts-expect-error deliberately unknown waveform name; the patch resolver must reject it.
        project.bounceWithSynthInstrument({ waveform: 'sawtooth-ish' }, { totalFrames: 128 }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('plays the GM drum map through the drum-kit preset', () => {
    // Note 38 = acoustic snare in the GM drum map.
    const project = buildMidiOnlyProject(38);
    try {
      const audio = project.bounceWithSynthInstrument('drum-kit', { totalFrames: 24000 });
      expect(peak(audio)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('follows GM program changes only when useGmPrograms is enabled', () => {
    const fixedProject = buildGmProgramProject();
    const changedProject = buildGmProgramProject(40);
    try {
      const options = { totalFrames: 12000, numChannels: 1, sampleRate: 48000 };
      const fixed = fixedProject.bounceWithSynthInstrument({ preset: 'sine' }, options);
      const explicitFalse = fixedProject.bounceWithSynthInstrument(
        { preset: 'sine', useGmPrograms: false },
        options,
      );
      const gm = fixedProject.bounceWithSynthInstrument(
        { preset: 'sine', useGmPrograms: true },
        options,
      );
      const otherGm = changedProject.bounceWithSynthInstrument(
        { preset: 'sine', useGmPrograms: true },
        options,
      );

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
        project.bounceWithSynthInstrument(
          { preset: 'sine', useGmPrograms: 1 } as unknown as SynthPatch,
          { totalFrames: 128 },
        ),
      ).toThrow();
      expect(() =>
        project.bounceWithSynthInstrument(
          { preset: 'sine', useGmPrograms: 'true' } as unknown as SynthPatch,
          { totalFrames: 128 },
        ),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });
});

describe('RealtimeEngine.setSynthInstrument', () => {
  it('renders live MIDI through a preset patch', () => {
    const engine = new RealtimeEngine();
    try {
      engine.prepare(48000, 128, 16, 16);
      engine.setSynthInstrument('saw-lead', 7);
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
      expect(() => engine.setSynthInstrument('no-such-preset', 7)).toThrow();
      expect(engine.midiInstrumentCount()).toBe(1);
    } finally {
      engine.destroy();
    }
  });
});
