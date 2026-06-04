import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { Project, RealtimeEngine } from '../src/index.js';

// Canonical minimal GS test SoundFont (presets: "Piano 1" at (0,0),
// "Piano 2" at (0,1), "Standard Kit" at (128,0); program 2 uncovered).
const fixturePath = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/sf2/minimal_gs.sf2',
);
const sf2Bytes = new Uint8Array(readFileSync(fixturePath));

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

describe('Project SoundFont (SF2) binding', () => {
  it('loads, counts presets and clears a SoundFont', () => {
    const project = Project.create();
    expect(project.soundFontPresetCount()).toBe(0);
    project.loadSoundFont(sf2Bytes);
    expect(project.soundFontPresetCount()).toBe(3);
    project.clearSoundFont();
    expect(project.soundFontPresetCount()).toBe(0);
    project.destroy();
  });

  it('rejects malformed SoundFont bytes and keeps the previous state', () => {
    const project = Project.create();
    project.loadSoundFont(sf2Bytes);
    expect(() => project.loadSoundFont(new Uint8Array([1, 2, 3, 4]))).toThrow();
    expect(project.soundFontPresetCount()).toBe(3);
    project.destroy();
  });

  it('reports per-program backends in the bounce manifest', () => {
    const project = buildMidiOnlyProject();
    // Without a SoundFont the played program falls back to the synth.
    expect(project.soundFontManifest()).toEqual([
      { channel: 0, bank: 0, program: 0, backend: 'synth', presetName: '' },
    ]);
    project.loadSoundFont(sf2Bytes);
    expect(project.soundFontManifest()).toEqual([
      { channel: 0, bank: 0, program: 0, backend: 'sf2', presetName: 'Piano 1' },
    ]);
    project.destroy();
  });

  it('bounces MIDI through the SoundFont player to non-silent audio', () => {
    const project = buildMidiOnlyProject();
    // Without a loaded SoundFont the bounce still sounds: the built-in
    // synthesizer GM fallback is the data-free floor.
    const fallback = project.bounceWithSf2Instrument(
      {},
      { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
    );
    expect(peak(fallback)).toBeGreaterThan(0.01);

    project.loadSoundFont(sf2Bytes);
    const audio = project.bounceWithSf2Instrument(
      { destinationId: 0, gain: 1 },
      { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
    );
    expect(audio.length).toBe(4096 * 2);
    expect(peak(audio)).toBeGreaterThan(0.01);

    // Deterministic: a second bounce is bit-identical.
    const again = project.bounceWithSf2Instrument(
      { destinationId: 0, gain: 1 },
      { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
    );
    expect(again).toEqual(audio);

    // An explicitly empty bindings array renders silence.
    const silent = project.bounceWithSf2Instruments([], {
      totalFrames: 2048,
      numChannels: 2,
      sampleRate: 48000,
    });
    expect(peak(silent)).toBe(0);
    project.destroy();
  });
});

describe('RealtimeEngine SoundFont (SF2) binding', () => {
  it('renders live MIDI input through a bound SF2 instrument', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Binding before a SoundFont is loaded is allowed: live MIDI plays through
    // the built-in synthesizer GM fallback (the data-free floor).
    engine.setSf2Instrument({}, 7);
    engine.pushMidiNoteOn(7, 0, 0, 60, 100, -1);
    const [fbLeft, fbRight] = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(peak(fbLeft), peak(fbRight))).toBeGreaterThan(0);
    engine.clearMidiInstrument(7);

    expect(() => engine.loadSoundFont(new Uint8Array([9, 9, 9]))).toThrow();

    engine.loadSoundFont(sf2Bytes);
    engine.setSf2Instrument({ gain: 1 }, 7);
    expect(engine.midiInstrumentCount()).toBe(1);

    engine.pushMidiNoteOn(7, 0, 0, 60, 100, -1);
    const [left, right] = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(peak(left), peak(right))).toBeGreaterThan(0);

    engine.clearMidiInstrument(7);
    expect(engine.midiInstrumentCount()).toBe(0);
    engine.destroy();
  });
});
