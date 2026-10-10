import { describe, expect, it } from 'vitest';
import type { AnalysisChord, Chord } from '../src/index.js';
import { chordSymbolFromAnalysis, keyModeFromAnalysis, Project } from '../src/index.js';

/** Analysis quality names in ordinal order (the C ABI's `SONARE_CHORD_*`). */
const QUALITIES = [
  'major',
  'minor',
  'diminished',
  'augmented',
  'dominant7',
  'major7',
  'minor7',
  'sus2',
  'sus4',
  'unknown',
  'add9',
  'minorAdd9',
  'dim7',
  'halfDim7',
  'major9',
  'dominant9',
  'sus2Add4',
  'major6',
  'minor6',
  'minorMajor7',
  'dominant7Sus4',
  'dominant11',
  'dominant13',
  'dominant7Flat9',
  'dominant7Sharp9',
] as const;

const chord = (quality: string, root = 'D', bass = root): Chord =>
  ({ root, bass, rootName: root, bassName: bass, quality }) as unknown as Chord;

const byOrdinal = (quality: number, root = 2, bass = root): AnalysisChord =>
  ({ root, bass, quality, start: 0, end: 1, confidence: 1, name: '' }) as unknown as AnalysisChord;

describe('chordSymbolFromAnalysis', () => {
  it.each([
    ['minor', 2, [] as number[]],
    ['dominant7', 5, [7]],
    ['halfDim7', 6, [7]],
    ['sus4', 7, [4]],
    ['dominant13', 5, [7, 9, 13]],
  ])('maps %s to annotation quality %i', (name, quality, extensions) => {
    expect(chordSymbolFromAnalysis(chord(name))).toEqual({
      rootPc: 2,
      quality,
      extensions,
      slashBassPc: 255,
    });
  });

  it('keeps the root of an unknown chord', () => {
    expect(chordSymbolFromAnalysis(chord('unknown', 'F#'))).toEqual({
      rootPc: 6,
      quality: 0,
      extensions: [],
      slashBassPc: 255,
    });
  });

  it('reports a slash bass only when it differs from the root', () => {
    expect(chordSymbolFromAnalysis(chord('major', 'C', 'E')).slashBassPc).toBe(4);
    expect(chordSymbolFromAnalysis(chord('major', 'C', 'C')).slashBassPc).toBe(255);
  });

  it('accepts an AnalysisChord with ordinals and agrees with the string form', () => {
    QUALITIES.forEach((name, ordinal) => {
      expect(chordSymbolFromAnalysis(byOrdinal(ordinal, 9, 4))).toEqual(
        chordSymbolFromAnalysis(chord(name, 'A', 'E')),
      );
    });
  });

  it('produces annotations annotateChords accepts and a round trip leaves unchanged', () => {
    const project = Project.create();
    try {
      project.annotateChords(
        QUALITIES.map((name, i) => ({
          startPpq: i,
          endPpq: i + 1,
          ...chordSymbolFromAnalysis(chord(name, 'G', 'B')),
        })),
      );
      const json = project.toJson();
      const restored = Project.fromJson(json);
      try {
        expect(restored.toJson()).toBe(json);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('refuses a non-object with a TypeError', () => {
    for (const bad of [null, undefined, 42, 'C', [1]]) {
      expect(() => chordSymbolFromAnalysis(bad as never)).toThrow(TypeError);
    }
  });

  it('refuses an unrecognised value with a RangeError naming the field', () => {
    expect(() => chordSymbolFromAnalysis(chord('lydian'))).toThrowError(
      new RangeError("chord.quality is not a chord quality: 'lydian'"),
    );
    expect(() => chordSymbolFromAnalysis(byOrdinal(25))).toThrow(/chord\.quality/);
    expect(() => chordSymbolFromAnalysis(byOrdinal(-1))).toThrow(/chord\.quality/);
    expect(() => chordSymbolFromAnalysis(chord('major', 'H'))).toThrow(/chord\.root/);
    expect(() => chordSymbolFromAnalysis(byOrdinal(1, 12))).toThrow(/chord\.root/);
    expect(() => chordSymbolFromAnalysis(chord('major', 'C', 'Z'))).toThrow(/chord\.bass/);
    for (const bad of [
      () => chordSymbolFromAnalysis(chord('lydian')),
      () => chordSymbolFromAnalysis(byOrdinal(1, 12)),
    ]) {
      expect(bad).toThrow(RangeError);
    }
  });

  it('refuses an omitted field instead of defaulting it', () => {
    expect(() => chordSymbolFromAnalysis({ root: 'C', bass: 'C' } as never)).toThrow(TypeError);
    expect(() => chordSymbolFromAnalysis({ quality: 'major', bass: 'C' } as never)).toThrow(
      TypeError,
    );
  });
});

describe('keyModeFromAnalysis', () => {
  it.each([
    ['major', 1],
    ['minor', 2],
    ['dorian', 3],
    ['phrygian', 4],
    ['lydian', 5],
    ['mixolydian', 6],
    ['locrian', 7],
  ])('maps %s to annotation mode %i', (name, mode) => {
    expect(keyModeFromAnalysis(name)).toBe(mode);
  });

  it('feeds annotateKeys', () => {
    const project = Project.create();
    try {
      project.annotateKeys([
        { startPpq: 0, endPpq: 4, tonicPc: 9, mode: keyModeFromAnalysis('minor') },
      ]);
    } finally {
      project.destroy();
    }
  });

  it('refuses a non-string with a TypeError and an unknown name with a RangeError', () => {
    for (const bad of [null, undefined, 1, {}]) {
      expect(() => keyModeFromAnalysis(bad as never)).toThrow(TypeError);
    }
    expect(() => keyModeFromAnalysis('unknown')).toThrow(RangeError);
    expect(() => keyModeFromAnalysis('Major')).toThrow(/mode is not a key mode name/);
  });
});
