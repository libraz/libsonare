/**
 * NativeSynth WASM binding tests: synthPresetNames / synthPresetPatch,
 * Project.bounceWithSynthInstrument and the realtime engine
 * setSynthInstrument entry.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
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
  synthPatchRoundTripForTest,
  synthPresetNames,
  synthPresetPatch,
} from '../dist/index.js';

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

describe('Sonare WASM NativeSynth', () => {
  beforeAll(async () => {
    await init();
  });

  function buildMidiOnlyProject(note = 60): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, note, 100),
      Project.midiNoteOff(2, 0, 0, note, 0),
    ]);
    return project;
  }

  it('lists the preset catalog and fetches patches', () => {
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
    const pad = synthPresetPatch('warm-pad');
    expect(pad.preset).toBe('warm-pad');
    expect(pad.engineMode).toBe('subtractive');
    expect(pad.waveform).toBe('saw');
    expect(pad.unison).toBe(7);
    // The "va:" routing prefix is accepted.
    expect(synthPresetPatch('va:e-piano').engineMode).toBe('fm');
    expect(() => synthPresetPatch('no-such-preset')).toThrow();
  });

  it('keeps every NativeSynth enum table in parity with native round-trip ordinals', () => {
    expect(synthEnumTables()).toEqual({
      engineModes: [...SYNTH_ENGINE_MODES],
      waveforms: [...SYNTH_OSC_WAVEFORMS],
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

  it('bounces preset patches deterministically', () => {
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
      expect(() =>
        project.bounceWithSynthInstrument('no-such-preset', { totalFrames: 128 }),
      ).toThrow();
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

  it('renders live MIDI through the engine synth instrument', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
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
      expect(() => engine.setSynthInstrument('no-such-preset', 7)).toThrow();
      expect(engine.midiInstrumentCount()).toBe(1);
    } finally {
      engine.destroy();
    }
  });
});
