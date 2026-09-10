/**
 * Sample-bank WASM binding: handle lifetime, the argument rejections, the
 * zero-init conventions the C ABI promises, and a bounce that renders
 * host-supplied PCM through a sample patch.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { SampleZoneDesc, SynthPatch } from '../dist/index.js';
import {
  ErrorCode,
  init,
  isSonareError,
  Project,
  RealtimeEngine,
  SampleBank,
  SYNTH_ENGINE_MODES,
  synthEnumTables,
} from '../dist/index.js';

const SAMPLE_RATE = 48000;

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

/** A pitched tone with content across the band, so a render is measurably not silence. */
function tone(hz: number, seconds: number): Float32Array {
  const n = Math.floor(seconds * SAMPLE_RATE);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / SAMPLE_RATE;
    let v = 0;
    for (let h = 1; h <= 24; h++) {
      const f = hz * h;
      if (f > 0.45 * SAMPLE_RATE) {
        break;
      }
      v += Math.sin(2 * Math.PI * f * t) / h;
    }
    out[i] = v * 0.5;
  }
  return out;
}

function expectInvalidParameter(action: () => void): void {
  let caught: unknown;
  try {
    action();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  if (isSonareError(caught)) {
    expect(caught.code).toBe(ErrorCode.InvalidParameter);
  }
}

describe('Sonare WASM sample bank', () => {
  beforeAll(async () => {
    await init();
  });

  /** A one-track project with a single sounding note on destination 0. */
  function oneNoteProject(note: number): Project {
    const project = new Project();
    project.setSampleRate(SAMPLE_RATE);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, note, 100),
      Project.midiNoteOff(2, 0, 0, note, 0),
    ]);
    return project;
  }

  it('counts what it holds and rejects what it cannot', () => {
    const bank = new SampleBank();
    try {
      const pcm = tone(261.6256, 0.2);

      expectInvalidParameter(() => bank.addSample(new Float32Array(0)));
      expect(bank.sampleCount()).toBe(0);
      expect(bank.setCount()).toBe(0);

      expect(bank.addSample(pcm)).toBe(0);
      expect(bank.sampleCount()).toBe(1);

      // Descriptor fields are range- and type-checked before they are narrowed:
      // a bare cast would wrap 200 into a valid-looking MIDI key.
      expectInvalidParameter(() => bank.addSample(pcm, { rootKey: 200 }));
      expectInvalidParameter(() => bank.addSample(pcm, { rootKey: 60.5 }));
      expectInvalidParameter(() => bank.addSample(pcm, { rootKey: -1 }));
      expectInvalidParameter(() =>
        bank.addSample(pcm, { fineTuneCents: 'flat' as unknown as number }),
      );
      expectInvalidParameter(() => bank.addSample(pcm, { sourceRate: Number.NaN }));
      // loopMode is the raw SoundFont sampleModes number or one of its three
      // names — not the patch's SonareSampleLoopMode, which numbers them
      // differently and has a 'default' the recording itself cannot have.
      expectInvalidParameter(() =>
        bank.addSample(pcm, { loopMode: 'sometimes' as unknown as number }),
      );
      expectInvalidParameter(() =>
        bank.addSample(pcm, { loopMode: 'default' as unknown as number }),
      );
      expect(bank.addSample(pcm, { loopMode: 3 })).toBe(1);
      expect(bank.addSample(pcm, { loopMode: 'key-down' })).toBe(2);
      expect(bank.sampleCount()).toBe(3);

      expectInvalidParameter(() => bank.addZone({ sampleIndex: 9 }));
      expectInvalidParameter(() => bank.addZone({ setIndex: 4096, sampleIndex: 0 }));
      // Genuinely inverted, both edges given: a rejection the per-bound
      // defaults cannot explain away.
      expectInvalidParameter(() => bank.addZone({ keyLo: 80, keyHi: 20 }));
      expectInvalidParameter(() => bank.addZone({ velLo: 100, velHi: 10 }));
      expectInvalidParameter(() => bank.addZone({ setIndex: -1 }));
      expectInvalidParameter(() => bank.addZone({ keyLo: 200 }));
      expect(bank.setCount()).toBe(0);

      bank.addZone({ sampleIndex: 0 });
      expect(bank.setCount()).toBe(1);

      // A set index creates the sets below it, so the count follows the highest.
      bank.addZone({ setIndex: 3, sampleIndex: 0 });
      expect(bank.setCount()).toBe(4);
    } finally {
      bank.delete();
    }
  });

  it('defaults each zone bound on its own, so narrowing one axis keeps the other whole', () => {
    // A bound the caller left out must not collapse the axis beside it: an
    // omitted upper edge is 127, an omitted velLo is 1, and an omitted keyLo is
    // the lowest key. Note 72 at velocity 100 sits inside every sounding
    // rectangle below and outside both silent ones.
    const cases: ReadonlyArray<[string, SampleZoneDesc, boolean]> = [
      ['no bounds at all', {}, true],
      ['only the key range', { keyLo: 48, keyHi: 96 }, true],
      ['only a lower key bound', { keyLo: 48 }, true],
      ['only the velocity range', { velLo: 64, velHi: 127 }, true],
      ['only a lower velocity bound', { velLo: 64 }, true],
      // The two silent rows are the control: without them every row above would
      // pass just as well on a binding that ignored the rectangle altogether.
      ['a key range the note sits below', { keyLo: 90 }, false],
      ['a velocity range the note sits below', { velLo: 110 }, false],
    ];
    for (const [label, bounds, sounds] of cases) {
      const bank = new SampleBank();
      const project = oneNoteProject(72);
      try {
        const index = bank.addSample(tone(261.6256, 0.2));
        bank.addZone({ ...bounds, sampleIndex: index });

        const audio = project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: bank },
          { totalFrames: 12000 },
        );
        expect(peak(audio) > 0, label).toBe(sounds);
      } finally {
        project.delete();
        bank.delete();
      }
    }
  });

  it('renders host PCM through a sample patch, deterministically', () => {
    const bank = new SampleBank();
    const project = oneNoteProject(60);
    try {
      const pcm = tone(261.6256, 0.25);
      const index = bank.addSample(pcm, {
        rootKey: 60,
        fineTuneCents: 5,
        sourceRate: 44100,
        loopStart: 1000,
        loopEnd: pcm.length - 1,
        loopMode: 'continuous',
      });
      bank.addZone({
        setIndex: 1,
        sampleIndex: index,
        keyLo: 0,
        keyHi: 127,
        velLo: 1,
        velHi: 127,
        tuneCents: -3,
        gain: 0.8,
        panUnits: 120,
      });

      const patch: SynthPatch = {
        engineMode: 'sample',
        sampleSet: 1,
        sampleLevel: 0.75,
        sampleLoop: 'continuous',
        sampleStartOffset: 0.1,
        sampleKeyTrack: 'on',
        sampleBank: bank,
      };
      const first = project.bounceWithSynthInstrument(patch, { totalFrames: SAMPLE_RATE });
      const second = project.bounceWithSynthInstrument(patch, { totalFrames: SAMPLE_RATE });
      expect(peak(first)).toBeGreaterThan(0);
      expect(Array.from(second)).toEqual(Array.from(first));
    } finally {
      project.delete();
      bank.delete();
    }
  });

  it('renders silence, not an error, for a sample patch with no bank', () => {
    const project = oneNoteProject(60);
    try {
      const audio = project.bounceWithSynthInstrument(
        { engineMode: 'sample' },
        { totalFrames: 24000 },
      );
      expect(peak(audio)).toBe(0);
    } finally {
      project.delete();
    }
  });

  it('refuses a released bank and anything that is not a SampleBank', () => {
    const project = oneNoteProject(60);
    const released = new SampleBank();
    try {
      released.addSample(tone(261.6256, 0.1));
      released.delete();

      // The id outlives the handle, so a released bank has to be refused
      // rather than left to a pointer that still looks plausible.
      expect(() =>
        project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: released },
          { totalFrames: 4800 },
        ),
      ).toThrow(TypeError);
      expect(() =>
        project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: {} as unknown as SampleBank },
          { totalFrames: 4800 },
        ),
      ).toThrow(TypeError);
    } finally {
      project.delete();
    }
  });

  it('binds the same bank on the realtime engine', () => {
    // The realtime path resolves `sampleBank` through the same normalizer as
    // the bounce, so a patch that sounds offline sounds live. The synth takes a
    // share of the bank, which is why releasing the handle first still renders.
    const engine = new RealtimeEngine(SAMPLE_RATE, 128);
    const bank = new SampleBank();
    try {
      const index = bank.addSample(tone(261.6256, 0.2));
      bank.addZone({ sampleIndex: index });
      engine.setSynthInstrument({ engineMode: 'sample', sampleBank: bank }, 7);
      bank.delete();

      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      let p = 0;
      for (let block = 0; block < 8; block++) {
        for (const channel of engine.process([new Float32Array(128), new Float32Array(128)])) {
          for (const sample of channel) {
            p = Math.max(p, Math.abs(sample));
          }
        }
      }
      expect(p).toBeGreaterThan(0);

      expect(() =>
        engine.setSynthInstrument(
          { engineMode: 'sample', sampleBank: {} as unknown as SampleBank },
          7,
        ),
      ).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('lists the sample engine among the engine modes', () => {
    expect(SYNTH_ENGINE_MODES).toContain('sample');
    expect(SYNTH_ENGINE_MODES[SYNTH_ENGINE_MODES.length - 1]).toBe('sample');
    expect(synthEnumTables().engineModes).toEqual([...SYNTH_ENGINE_MODES]);
  });
});
