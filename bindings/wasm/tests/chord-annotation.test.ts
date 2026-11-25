/**
 * Analysis chord / key mode -> annotation conversion on the WASM surface:
 * `chordSymbolFromAnalysis` and `keyModeFromAnalysis` over the
 * sonare_c_project_annotate.h C ABI.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  CHORD_QUALITIES,
  type ChordQuality,
  chordSymbolFromAnalysis,
  init,
  keyModeFromAnalysis,
  Project,
} from '../src/index';

const QUALITIES = CHORD_QUALITIES;

const chord = (root: number, quality: ChordQuality, bass: number = root) =>
  ({ root, bass, quality }) as Parameters<typeof chordSymbolFromAnalysis>[0];

describe('chordSymbolFromAnalysis', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    ['Major', 'major', 'major', []],
    ['Minor', 'minor', 'minor', []],
    ['Dominant7', 'dominant7', 'dominant', [7]],
    ['HalfDim7', 'halfDim7', 'halfDiminished', [7]],
    ['Sus4', 'sus4', 'suspended', [4]],
    ['Dominant13', 'dominant13', 'dominant', [7, 9, 13]],
    ['Unknown', 'unknown', 'unknown', []],
  ] as const)(
    'maps %s to annotation quality %s with extensions %j',
    (_name, quality, mapped, extensions) => {
      const out = chordSymbolFromAnalysis(chord(7, quality));
      expect(out).toEqual({ rootPc: 7, quality: mapped, extensions });
      expect(out).not.toHaveProperty('slashBassPc');
    },
  );

  it('keeps the root of an unknown chord and reports a distinct bass as slash bass', () => {
    expect(chordSymbolFromAnalysis(chord(2, 'unknown')).rootPc).toBe(2);
    expect(chordSymbolFromAnalysis(chord(0, 'major', 7)).slashBassPc).toBe(7);
  });

  it('refuses a quality ordinal and a pitch-class name', () => {
    expect(() => chordSymbolFromAnalysis({ root: 2, bass: 2, quality: 1 } as never)).toThrow(
      TypeError,
    );
    expect(() =>
      chordSymbolFromAnalysis({ root: 'D', bass: 2, quality: 'minor' } as never),
    ).toThrow(TypeError);
  });

  it('yields all 25 analysis qualities as annotations the project accepts and keeps', () => {
    expect(QUALITIES.length).toBe(25);
    const symbols = QUALITIES.map((quality, i) => ({
      startPpq: i,
      endPpq: i + 1,
      ...chordSymbolFromAnalysis(chord(i % 12, quality, (i + 3) % 12)),
    }));
    const project = new Project();
    try {
      project.setSampleRate(48000);
      project.annotateChords(symbols);
      const expected = JSON.parse(project.toJson()).annotation.chords;
      expect(expected.length).toBe(25);
      const restored = Project.fromJson(project.toJson());
      try {
        expect(JSON.parse(restored.toJson()).annotation.chords).toEqual(expected);
      } finally {
        restored.destroy();
      }
      for (const [i, row] of expected.entries()) {
        expect(row.quality ?? 'unknown', `quality ${QUALITIES[i]}`).toBe(symbols[i].quality);
        expect(row.extensions ?? [], `quality ${QUALITIES[i]}`).toEqual(symbols[i].extensions);
      }
    } finally {
      project.destroy();
    }
  });

  it('refuses a non-object as a TypeError', () => {
    for (const value of [null, undefined, 5, 'C', [1, 2]]) {
      expect(() => chordSymbolFromAnalysis(value as never)).toThrow(TypeError);
    }
  });

  it('names the field of an out-of-domain value as a RangeError', () => {
    expect(() => chordSymbolFromAnalysis(chord(12, 'major'))).toThrow(RangeError);
    expect(() => chordSymbolFromAnalysis(chord(12, 'major'))).toThrow(/root/);
    expect(() => chordSymbolFromAnalysis(chord(0, 'major', 12))).toThrow(/bass/);
    expect(() => chordSymbolFromAnalysis(chord(0, 'ninth' as ChordQuality))).toThrow(RangeError);
    expect(() => chordSymbolFromAnalysis(chord(0, 'ninth' as ChordQuality))).toThrow(/quality/);
    expect(() =>
      chordSymbolFromAnalysis({ root: 'H', bass: 0, quality: 'major' } as never),
    ).toThrow(/root/);
  });
});

describe('keyModeFromAnalysis', () => {
  beforeAll(async () => {
    await init();
  });

  it.each(['major', 'minor', 'dorian', 'phrygian', 'lydian', 'mixolydian', 'locrian'] as const)(
    'maps %s to the same-named annotation mode and stores it by name',
    (mode) => {
      expect(keyModeFromAnalysis(mode)).toBe(mode);
      const project = new Project();
      try {
        project.setSampleRate(48000);
        project.annotateKeys([
          { startPpq: 0, endPpq: 4, tonicPc: 0, mode: keyModeFromAnalysis(mode) },
        ]);
        expect(JSON.parse(project.toJson()).annotation.keys[0].mode).toBe(mode);
      } finally {
        project.destroy();
      }
    },
  );

  it('refuses a mode ordinal', () => {
    expect(() => keyModeFromAnalysis(1 as never)).toThrow(TypeError);
  });

  it('refuses a wrong type as a TypeError and an unknown mode as a RangeError naming mode', () => {
    expect(() => keyModeFromAnalysis(null as never)).toThrow(TypeError);
    expect(() => keyModeFromAnalysis({} as never)).toThrow(TypeError);
    expect(() => keyModeFromAnalysis('ionian' as never)).toThrow(RangeError);
    expect(() => keyModeFromAnalysis('ionian' as never)).toThrow(/mode/);
    expect(() => keyModeFromAnalysis(7 as never)).toThrow(TypeError);
    expect(() => keyModeFromAnalysis(-1 as never)).toThrow(TypeError);
  });
});

describe('annotation list refusals name the field by its path from the argument root', () => {
  beforeAll(async () => {
    await init();
  });

  const key = { startPpq: 0, endPpq: 4 };
  const modes = 'unknown, major, minor, dorian, phrygian, lydian, mixolydian, locrian';
  const cases: ReadonlyArray<readonly [string, (project: Project) => void, Error]> = [
    [
      'an absent key end',
      (p) => p.annotateKeys([key, { startPpq: 4 } as never]),
      new TypeError('Project.annotateKeys: keys[1].endPpq must be a number'),
    ],
    [
      'a number key',
      (p) => p.annotateKeys([42 as never]),
      new TypeError('Project.annotateKeys: keys[0] must be an object'),
    ],
    [
      'a null key',
      (p) => p.annotateKeys([null as never]),
      new TypeError('Project.annotateKeys: keys[0] must be an object'),
    ],
    [
      'an out-of-range tonic',
      (p) => p.annotateKeys([{ ...key, tonicPc: 12 }]),
      new RangeError(
        'Project.annotateKeys: keys[0].tonicPc must be an integer in [0, 11], or omitted',
      ),
    ],
    [
      'a wrong-typed tonic',
      (p) => p.annotateKeys([{ ...key, tonicPc: 'C' as never }]),
      new TypeError('Project.annotateKeys: keys[0].tonicPc must be an integer'),
    ],
    [
      'an unknown mode',
      (p) => p.annotateKeys([{ ...key, mode: 'bogus' as never }]),
      new RangeError(`Project.annotateKeys: keys[0].mode must be one of ${modes}`),
    ],
    [
      'a numeric mode',
      (p) => p.annotateKeys([{ ...key, mode: 3 as never }]),
      new TypeError(`Project.annotateKeys: keys[0].mode must be one of ${modes}`),
    ],
    [
      'an absent chord start',
      (p) => p.annotateChords([{ endPpq: 4 } as never]),
      new TypeError('Project.annotateChords: chords[0].startPpq must be a number'),
    ],
    [
      'a wrong-typed chord end',
      (p) => p.annotateChords([{ startPpq: 0, endPpq: 'x' as never }]),
      new TypeError('Project.annotateChords: chords[0].endPpq must be a number'),
    ],
    [
      'an out-of-range chord root',
      (p) => p.annotateChords([{ startPpq: 0, endPpq: 4, rootPc: 12 }]),
      new RangeError(
        'Project.annotateChords: chords[0].rootPc must be an integer in [0, 11], or omitted',
      ),
    ],
    [
      'an out-of-range slash bass',
      (p) => p.annotateChords([{ startPpq: 0, endPpq: 4, slashBassPc: -1 }]),
      new RangeError(
        'Project.annotateChords: chords[0].slashBassPc must be an integer in [0, 11], or omitted',
      ),
    ],
    [
      'a wrong-typed modulation flag',
      (p) => p.annotateChords([{ startPpq: 0, endPpq: 4, modulationBoundary: 1 as never }]),
      new TypeError('Project.annotateChords: chords[0].modulationBoundary must be a boolean'),
    ],
  ];

  it.each(cases)('refuses %s', (_name, call, error) => {
    const project = new Project();
    try {
      expect(() => call(project)).toThrow(error);
    } finally {
      project.destroy();
    }
  });
});
