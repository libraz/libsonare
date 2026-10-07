import { beforeAll, describe, expect, it } from 'vitest';
import {
  type ChordFunctionsInput,
  ChordQuality,
  chordFunctions,
  detectChords,
  init,
  Mode,
  PitchClass,
} from '../src/index';

const SAMPLE_RATE = 22050;

function triad(notes: number[], seconds: number): Float32Array {
  const out = new Float32Array(Math.floor(SAMPLE_RATE * seconds));
  for (let i = 0; i < out.length; i++) {
    let sum = 0;
    for (const f of notes) {
      sum += Math.sin((2 * Math.PI * f * i) / SAMPLE_RATE);
    }
    out[i] = 0.25 * sum;
  }
  return out;
}

function progression(): Float32Array {
  const parts = [
    triad([261.63, 329.63, 392.0], 1.5),
    triad([349.23, 440.0, 523.25], 1.5),
    triad([392.0, 493.88, 587.33], 1.5),
    triad([261.63, 329.63, 392.0], 1.5),
  ];
  const out = new Float32Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

describe('chordFunctions', () => {
  beforeAll(async () => {
    await init();
  });

  it('labels a detected C-F-G-C progression in C major without re-detecting', () => {
    const detected = detectChords({
      samples: progression(),
      sampleRate: SAMPLE_RATE,
      useBeatSync: false,
      useTriadsOnly: true,
    });
    const result = chordFunctions({
      chords: detected,
      key: { root: PitchClass.C, mode: Mode.Major },
    });
    expect(result.length).toBe(detected.chords.length);
    result.forEach((entry, i) => {
      expect(entry.start).toBe(detected.chords[i].start);
      expect(entry.end).toBe(detected.chords[i].end);
      expect(entry.name).toBe(detected.chords[i].name);
    });
    const collapsed = result.filter((entry, i) => i === 0 || entry.roman !== result[i - 1].roman);
    expect(collapsed.map((entry) => entry.roman)).toEqual(['I', 'IV', 'V', 'I']);
    expect(collapsed.map((entry) => entry.function)).toEqual([
      'tonic',
      'subdominant',
      'dominant',
      'tonic',
    ]);
    expect(
      chordFunctions({ chords: detected.chords, key: { root: PitchClass.C, mode: Mode.Major } }),
    ).toEqual(result);
  });

  it('labels hand-built chords, including unknown and chromatic ones', () => {
    const chords: (ChordFunctionsInput & { start: number; end: number; confidence: number })[] = [
      { root: PitchClass.A, quality: ChordQuality.Minor, start: 0, end: 1, confidence: 0.9 },
      { root: PitchClass.D, quality: ChordQuality.Minor, start: 1, end: 2, confidence: 0.9 },
      { root: PitchClass.E, quality: ChordQuality.Major, start: 2, end: 3, confidence: 0.9 },
      { root: PitchClass.C, quality: ChordQuality.Unknown, start: 3, end: 4, confidence: 0 },
      { root: PitchClass.As, quality: ChordQuality.Major, start: 4, end: 5, confidence: 0.9 },
    ];
    const result = chordFunctions({ chords, key: { root: PitchClass.A, mode: Mode.Minor } });
    expect(result.map((entry) => entry.function)).toEqual([
      'tonic',
      'subdominant',
      'dominant',
      'none',
      'chromatic',
    ]);
    expect(result[3].roman).toBe('N.C.');
    expect(result[0]).toMatchObject({ start: 0, end: 1, confidence: 0.9 });
  });

  it('refuses an invalid key or chord', () => {
    expect(() =>
      chordFunctions({ chords: [], key: { root: 12 as PitchClass, mode: Mode.Major } }),
    ).toThrow();
    expect(() =>
      chordFunctions({
        chords: [{ root: PitchClass.C, quality: 99 as ChordQuality }],
        key: { root: PitchClass.C, mode: Mode.Major },
      }),
    ).toThrow();
    expect(chordFunctions({ chords: [], key: { root: PitchClass.C, mode: Mode.Major } })).toEqual(
      [],
    );
  });
});
