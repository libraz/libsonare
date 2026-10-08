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

  it('clears the amplifier the bank binds after an electric guitar', () => {
    // Program 30 is one of six the bank binds a rig to, program 0 is not, so
    // the flag has to move the first render and leave the second untouched.
    const render = (program: number, clearBankRig: boolean) => {
      const project = Project.create();
      project.setSampleRate(48000);
      const { clipId, trackId } = project.addMidiClip(0, 2);
      project.setTrackMidiDestination(trackId, 0);
      project.setMidiEvents(clipId, [
        Project.midiProgram(0, 0, 0, program),
        Project.midiNoteOn(0.05, 0, 0, 52, 110),
        Project.midiNoteOff(1, 0, 0, 52, 0),
      ]);
      const out = project.bounceWithSf2Instrument(
        { destinationId: 0, gain: 1, clearBankRig },
        { totalFrames: 48000, numChannels: 2, sampleRate: 48000 },
      );
      project.destroy();
      return out;
    };
    const rigged = render(30, false);
    const direct = render(30, true);
    expect(peak(direct)).toBeGreaterThan(0.01);
    expect(direct).not.toEqual(rigged);
    expect(render(0, true)).toEqual(render(0, false));
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

    // Program 0 is a dedicated piano model. The opt-in must bypass the
    // loaded Piano 1 preset, while the default above remains SF2-first.
    const modeled = project.bounceWithSf2Instrument(
      { destinationId: 0, gain: 1, preferModelForModeledFamilies: true },
      { totalFrames: 4096, numChannels: 2, sampleRate: 48000 },
    );
    expect(peak(modeled)).toBeGreaterThan(0.01);
    expect(modeled).not.toEqual(audio);

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
    engine.pushMidiNoteOn(7, 0, 0, 60, 100);
    const [fbLeft, fbRight] = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(peak(fbLeft), peak(fbRight))).toBeGreaterThan(0);
    engine.clearMidiInstrument(7);

    expect(() => engine.loadSoundFont(new Uint8Array([9, 9, 9]))).toThrow();

    engine.loadSoundFont(sf2Bytes);
    engine.setSf2Instrument({ gain: 1 }, 7);
    expect(engine.midiInstrumentCount()).toBe(1);

    engine.pushMidiNoteOn(7, 0, 0, 60, 100);
    const [left, right] = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(peak(left), peak(right))).toBeGreaterThan(0);

    engine.clearMidiInstrument(7);
    expect(engine.midiInstrumentCount()).toBe(0);
    engine.destroy();
  });
});

function gsDt1(a0: number, a1: number, a2: number, data: number[]): Uint8Array {
  const total = a0 + a1 + a2 + data.reduce((sum, b) => sum + b, 0);
  return new Uint8Array([
    0xf0,
    0x41,
    0x10,
    0x42,
    0x12,
    a0,
    a1,
    a2,
    ...data,
    (128 - (total % 128)) % 128,
    0xf7,
  ]);
}

// One held note on part 1 through an Overdrive insertion effect, realised as asked.
function renderOverdrive(gsEfxRealization?: 'modern' | 'classic'): Float32Array {
  const engine = new RealtimeEngine(48000, 128);
  try {
    engine.loadSoundFont(sf2Bytes);
    engine.setSf2Instrument(gsEfxRealization === undefined ? {} : { gsEfxRealization }, 7);
    engine.pushMidiSysex(7, gsDt1(0x40, 0x03, 0x00, [0x01, 0x10]));
    engine.pushMidiSysex(7, gsDt1(0x40, 0x41, 0x22, [0x01]));
    engine.pushMidiNoteOn(7, 0, 0, 60, 100);
    const out = new Float32Array(64 * 128);
    for (let block = 0; block < 64; block++) {
      const [left] = engine.process([new Float32Array(128), new Float32Array(128)]);
      out.set(left, block * 128);
    }
    return out;
  } finally {
    engine.destroy();
  }
}

describe('SF2 instrument gsEfxRealization', () => {
  it('refuses a value other than modern or classic by name', () => {
    const engine = new RealtimeEngine(48000, 128);
    for (const bad of ['Classic', '', 'vintage', 1, true]) {
      expect(() => engine.setSf2Instrument({ gsEfxRealization: bad as never }, 7)).toThrow(
        /gsEfxRealization/,
      );
    }
    engine.setSf2Instrument({ gsEfxRealization: 'classic' }, 7);
    engine.destroy();
    const project = Project.create();
    expect(() =>
      project.bounceWithSf2Instrument(
        { gsEfxRealization: 'vintage' as never },
        { totalFrames: 128, numChannels: 2, sampleRate: 48000 },
      ),
    ).toThrow(/gsEfxRealization/);
    project.destroy();
  });

  it('defaults to modern and the classic realisation renders differently', () => {
    const modern = renderOverdrive('modern');
    const classic = renderOverdrive('classic');
    expect(renderOverdrive()).toEqual(modern);
    expect(renderOverdrive('modern')).toEqual(modern);
    let modernEnergy = 0;
    let classicEnergy = 0;
    let difference = 0;
    for (let i = 0; i < modern.length; i++) {
      modernEnergy += modern[i] * modern[i];
      classicEnergy += classic[i] * classic[i];
      difference += (modern[i] - classic[i]) ** 2;
    }
    expect(modernEnergy).toBeGreaterThan(0);
    expect(classicEnergy).toBeGreaterThan(1e-3 * modernEnergy);
    expect(difference).toBeGreaterThan(1e-3 * modernEnergy);
  });
});
