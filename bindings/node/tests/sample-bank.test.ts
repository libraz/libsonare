/**
 * The host-PCM sample bank and the sample engine it feeds.
 *
 * The bank is a handle class with no readable state beyond two counts, so most
 * of what it promises is only observable through a bounce: whether a zone
 * covers a note at all is a rendered non-silence, and a rejected argument is
 * proven by the counts staying put.
 */

import { describe, expect, it } from 'vitest';
import {
  Project,
  RealtimeEngine,
  SampleBank,
  SYNTH_ENGINE_MODES,
  synthEnumTables,
} from '../src/index.js';

const SR = 48000;
const TOTAL_FRAMES = 24000;

/** Half a second of a 440 Hz sine, the sample every keymap below plays. */
function sampleSine(frames = 24000, freq = 440): Float32Array {
  return new Float32Array(frames).map((_, i) => 0.5 * Math.sin((2 * Math.PI * freq * i) / SR));
}

/** A MIDI-only project sounding @p note for the whole render. */
function buildNoteProject(note: number): Project {
  const project = Project.create();
  project.setSampleRate(SR);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, note, 100),
    Project.midiNoteOff(2, 0, 0, note, 0),
  ]);
  return project;
}

function peak(audio: Float32Array): number {
  let highest = 0;
  for (const value of audio) {
    const magnitude = Math.abs(value);
    if (magnitude > highest) {
      highest = magnitude;
    }
  }
  return highest;
}

function withBank<T>(body: (bank: SampleBank) => T): T {
  const bank = new SampleBank();
  try {
    return body(bank);
  } finally {
    bank.destroy();
  }
}

/** Renders @p note through a sample patch reading @p bank's keymap set 0. */
function renderNote(bank: SampleBank | undefined, note: number): Float32Array {
  const project = buildNoteProject(note);
  try {
    return project.bounceWithSynthInstrument(
      { engineMode: 'sample', sampleBank: bank },
      { totalFrames: TOTAL_FRAMES },
    );
  } finally {
    project.destroy();
  }
}

describe('the sample engine joins the patch enum tables', () => {
  it('lists the sample engine, in step with the native oracle', () => {
    expect(SYNTH_ENGINE_MODES).toContain('sample');
    expect(synthEnumTables().engineModes).toEqual([...SYNTH_ENGINE_MODES]);
    // The engine is the last mode, so a table that dropped it would still pass
    // a `toContain` on any earlier name.
    expect(SYNTH_ENGINE_MODES[SYNTH_ENGINE_MODES.length - 1]).toBe('sample');
  });
});

describe('SampleBank', () => {
  it('counts samples and keymap sets as they are added', () => {
    withBank((bank) => {
      expect(bank.sampleCount()).toBe(0);
      expect(bank.setCount()).toBe(0);

      const index = bank.addSample(sampleSine(), { rootKey: 69, sourceRate: SR });
      expect(index).toBe(0);
      expect(bank.addSample(sampleSine(1024))).toBe(1);
      expect(bank.sampleCount()).toBe(2);

      bank.addZone({ sampleIndex: 0 });
      expect(bank.setCount()).toBe(1);
      // A set is created along with every set below it.
      bank.addZone({ setIndex: 3, sampleIndex: 1 });
      expect(bank.setCount()).toBe(4);
    });
  });

  it('accepts both loop-mode spellings for the same three states', () => {
    withBank((bank) => {
      const pcm = sampleSine(1024);
      for (const [name, sf2Mode] of [
        ['none', 0],
        ['continuous', 1],
        ['key-down', 3],
      ] as const) {
        expect(bank.addSample(pcm, { loopMode: name, loopStart: 0, loopEnd: 512 })).toBeTypeOf(
          'number',
        );
        expect(bank.addSample(pcm, { loopMode: sf2Mode, loopStart: 0, loopEnd: 512 })).toBeTypeOf(
          'number',
        );
      }
      expect(bank.sampleCount()).toBe(6);
    });
  });

  it('rejects a sample that is not a Float32Array, adding nothing', () => {
    withBank((bank) => {
      // @ts-expect-error the frames argument is deliberately the wrong type.
      expect(() => bank.addSample([0, 1, 2])).toThrow(TypeError);
      expect(() => bank.addSample(new Float32Array(0))).toThrow();
      expect(bank.sampleCount()).toBe(0);
    });
  });

  it('rejects a descriptor that is not an object', () => {
    withBank((bank) => {
      // @ts-expect-error the descriptor is deliberately the wrong type.
      expect(() => bank.addSample(sampleSine(64), 'rootKey=60')).toThrow(TypeError);
      expect(bank.sampleCount()).toBe(0);
    });
  });

  it('rejects a rootKey the narrowing cast would wrap', () => {
    withBank((bank) => {
      // 256 would arrive at the C ABI as 0, which its own check accepts.
      expect(() => bank.addSample(sampleSine(64), { rootKey: 256 })).toThrow(RangeError);
      expect(bank.sampleCount()).toBe(0);
    });
  });

  it('rejects an unknown loop-mode name', () => {
    withBank((bank) => {
      // @ts-expect-error deliberately unknown loop mode name.
      expect(() => bank.addSample(sampleSine(64), { loopMode: 'ping-pong' })).toThrow(
        /Unknown sample loop mode name/,
      );
      expect(bank.sampleCount()).toBe(0);
    });
  });

  it('rejects a value it could not attribute later, adding nothing', () => {
    withBank((bank) => {
      for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
        const poisoned = sampleSine(1024);
        poisoned[500] = bad;
        expect(() => bank.addSample(poisoned, { rootKey: 69, sourceRate: SR })).toThrow();
        expect(() => bank.addSample(sampleSine(1024), { sourceRate: bad })).toThrow();
        expect(() => bank.addSample(sampleSine(1024), { fineTuneCents: bad })).toThrow();
      }
      // Checking the count rather than the throw is what separates refusing
      // from appending and then reporting failure.
      expect(bank.sampleCount()).toBe(0);

      // The bank still takes the clean sample, so the refusals are the values
      // being read and not the bank having been left unusable.
      expect(bank.addSample(sampleSine(1024), { rootKey: 69, sourceRate: SR })).toBe(0);
      expect(bank.sampleCount()).toBe(1);
    });
  });

  it('a sample that survives the bank renders without a non-finite frame', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 69, sourceRate: SR });
      bank.addZone({ sampleIndex: 0 });
      const audio = renderNote(bank, 69);
      expect(peak(audio)).toBeGreaterThan(0);
      expect(audio.every((v) => Number.isFinite(v))).toBe(true);
    });
  });

  it('rejects a zone the bank cannot honour, adding nothing', () => {
    withBank((bank) => {
      // No sample yet, so index 0 does not exist.
      expect(() => bank.addZone({ sampleIndex: 0 })).toThrow();
      bank.addSample(sampleSine(64));

      expect(() => bank.addZone({ sampleIndex: 7 })).toThrow();
      expect(() => bank.addZone({ sampleIndex: 0, keyLo: 90, keyHi: 30 })).toThrow();
      expect(() => bank.addZone({ sampleIndex: 0, velLo: 100, velHi: 20 })).toThrow();
      expect(() => bank.addZone({ setIndex: 4096, sampleIndex: 0 })).toThrow();
      expect(bank.setCount()).toBe(0);
    });
  });

  it('rejects a setIndex that is not a number', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(64));
      // @ts-expect-error the set index is deliberately the wrong type.
      expect(() => bank.addZone({ setIndex: '0', sampleIndex: 0 })).toThrow(TypeError);
      expect(bank.setCount()).toBe(0);
    });
  });

  it('releases the native bank once, and refuses to be used afterwards', () => {
    const bank = new SampleBank();
    bank.addSample(sampleSine(64));
    bank.destroy();
    expect(() => bank.destroy()).not.toThrow();
    expect(() => bank.delete()).not.toThrow();
    expect(() => bank.sampleCount()).toThrow(/destroyed/);
    expect(() => bank.addSample(sampleSine(64))).toThrow(/destroyed/);
  });

  it('frees its native bank through `using`', () => {
    let captured: SampleBank | undefined;
    {
      using bank = new SampleBank();
      captured = bank;
      expect(bank.addSample(sampleSine(64))).toBe(0);
    }
    expect(() => captured?.sampleCount()).toThrow(/destroyed/);
  });
});

describe('bouncing a project through a sample patch', () => {
  it('renders host PCM to non-silence', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      bank.addZone({ sampleIndex: 0 });

      const audio = renderNote(bank, 60);
      expect(audio.length).toBe(TOTAL_FRAMES * 2);
      expect(peak(audio)).toBeGreaterThan(0);
    });
  });

  it('covers the whole keyboard with an all-zero zone', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      // No bounds at all: the C ABI reads the empty rectangle as every key at
      // every velocity, which is what makes a single-zone keymap this short.
      bank.addZone({ sampleIndex: 0 });

      for (const note of [24, 60, 96]) {
        expect(peak(renderNote(bank, note)), `note ${note} should sound`).toBeGreaterThan(0);
      }
    });
  });

  it('honours a key range narrowed on its own, and sounds nowhere else', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      // Only the key axis is given; the velocity bounds default on their own
      // and still cover 1..127.
      bank.addZone({ sampleIndex: 0, keyLo: 48, keyHi: 72 });

      expect(peak(renderNote(bank, 60))).toBeGreaterThan(0);
      expect(peak(renderNote(bank, 96))).toBe(0);
    });
  });

  it('reads a single bound as an open range, on either axis', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      // keyLo alone is 48..127: the upper bound defaults rather than staying at
      // 0 and inverting the range.
      bank.addZone({ sampleIndex: 0, keyLo: 48 });

      expect(peak(renderNote(bank, 96))).toBeGreaterThan(0);
      expect(peak(renderNote(bank, 24))).toBe(0);
    });

    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      // The mirror: velLo alone is 64..127 across the whole keyboard, and the
      // rendered note's velocity is 100.
      bank.addZone({ sampleIndex: 0, velLo: 64 });

      expect(peak(renderNote(bank, 96))).toBeGreaterThan(0);
    });
  });

  it('renders silence for a sample patch bound without a bank', () => {
    // The C ABI treats a missing bank as an empty keymap rather than an error,
    // so the failure mode is a quiet track, not a thrown bounce.
    expect(peak(renderNote(undefined, 60))).toBe(0);
  });

  it('rejects a sampleBank that is not a bank', () => {
    const project = buildNoteProject(60);
    try {
      expect(() =>
        project.bounceWithSynthInstrument(
          // @ts-expect-error the bank is deliberately the wrong type.
          { engineMode: 'sample', sampleBank: { addSample: () => 0 } },
          { totalFrames: 128 },
        ),
      ).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('rejects a destroyed sampleBank instead of reading freed memory', () => {
    const bank = new SampleBank();
    bank.addSample(sampleSine(64));
    bank.addZone({ sampleIndex: 0 });
    bank.destroy();
    expect(() => renderNote(bank, 60)).toThrow(TypeError);
  });

  it('binds the same bank to a realtime destination', () => {
    const bank = new SampleBank();
    bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
    bank.addZone({ sampleIndex: 0 });
    const engine = new RealtimeEngine();
    try {
      engine.prepare(SR, 128, 16, 16);
      engine.setSynthInstrument({ engineMode: 'sample', sampleBank: bank }, 7);
      // The engine took a share of the bank, so the caller's handle can go now
      // and the voice still has its pool.
      bank.destroy();
      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.max(...out.map(peak))).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });

  it('takes the patch sample fields', () => {
    withBank((bank) => {
      bank.addSample(sampleSine(), { rootKey: 60, sourceRate: SR });
      bank.addZone({ sampleIndex: 0 });
      const project = buildNoteProject(60);
      try {
        const plain = project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: bank },
          { totalFrames: TOTAL_FRAMES },
        );
        const quiet = project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: bank, sampleLevel: 0.1 },
          { totalFrames: TOTAL_FRAMES },
        );
        expect(peak(plain)).toBeGreaterThan(0);
        expect(peak(quiet)).toBeLessThan(peak(plain));

        // A keymap set the bank does not have is silent, not an error.
        const missingSet = project.bounceWithSynthInstrument(
          { engineMode: 'sample', sampleBank: bank, sampleSet: 5 },
          { totalFrames: TOTAL_FRAMES },
        );
        expect(peak(missingSet)).toBe(0);

        // The three enum spellings resolve; a bad one is rejected by name.
        for (const sampleLoop of ['default', 'none', 'continuous', 'key-down'] as const) {
          const audio = project.bounceWithSynthInstrument(
            { engineMode: 'sample', sampleBank: bank, sampleLoop, sampleKeyTrack: 'off' },
            { totalFrames: TOTAL_FRAMES },
          );
          expect(peak(audio)).toBeGreaterThan(0);
        }
        expect(() =>
          project.bounceWithSynthInstrument(
            // @ts-expect-error deliberately unknown loop mode name.
            { engineMode: 'sample', sampleBank: bank, sampleLoop: 'ping-pong' },
            { totalFrames: 128 },
          ),
        ).toThrow(/sample loop mode/);
      } finally {
        project.destroy();
      }
    });
  });
});
