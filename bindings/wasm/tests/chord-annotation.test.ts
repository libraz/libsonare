/**
 * Analysis chord / key mode -> annotation conversion on the WASM surface:
 * `chordSymbolFromAnalysis` and `keyModeFromAnalysis` over the
 * sonare_c_project_annotate.h C ABI.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ChordQuality,
  chordSymbolFromAnalysis,
  init,
  keyModeFromAnalysis,
  Project,
} from '../src/index';

const QUALITIES = Object.values(ChordQuality);

const chord = (root: number, quality: number, bass: number = root) =>
  ({ root, bass, quality }) as Parameters<typeof chordSymbolFromAnalysis>[0];

describe('chordSymbolFromAnalysis', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    ['Major', ChordQuality.Major, 1, []],
    ['Minor', ChordQuality.Minor, 2, []],
    ['Dominant7', ChordQuality.Dominant7, 5, [7]],
    ['HalfDim7', ChordQuality.HalfDim7, 6, [7]],
    ['Sus4', ChordQuality.Sus4, 7, [4]],
    ['Dominant13', ChordQuality.Dominant13, 5, [7, 9, 13]],
    ['Unknown', ChordQuality.Unknown, 0, []],
  ])(
    'maps %s to annotation quality %i with extensions %j',
    (_name, quality, mapped, extensions) => {
      const out = chordSymbolFromAnalysis(chord(7, quality));
      expect(out).toEqual({ rootPc: 7, quality: mapped, extensions, slashBassPc: 255 });
    },
  );

  it('keeps the root of an unknown chord and reports a distinct bass as slash bass', () => {
    expect(chordSymbolFromAnalysis(chord(2, ChordQuality.Unknown)).rootPc).toBe(2);
    expect(chordSymbolFromAnalysis(chord(0, ChordQuality.Major, 7)).slashBassPc).toBe(7);
  });

  it('accepts names as well as ordinals', () => {
    const out = chordSymbolFromAnalysis({
      root: 'D',
      bass: 'D',
      quality: 'Minor',
    } as unknown as Parameters<typeof chordSymbolFromAnalysis>[0]);
    expect(out).toEqual({ rootPc: 2, quality: 2, extensions: [], slashBassPc: 255 });
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
        expect(row.quality, `quality ${QUALITIES[i]}`).toBe(symbols[i].quality);
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
    expect(() => chordSymbolFromAnalysis(chord(12, 0))).toThrow(RangeError);
    expect(() => chordSymbolFromAnalysis(chord(12, 0))).toThrow(/root/);
    expect(() => chordSymbolFromAnalysis(chord(0, 0, 12))).toThrow(/bass/);
    expect(() => chordSymbolFromAnalysis(chord(0, 25))).toThrow(RangeError);
    expect(() => chordSymbolFromAnalysis(chord(0, 25))).toThrow(/quality/);
    expect(() => chordSymbolFromAnalysis(chord(0, -1))).toThrow(/quality/);
    expect(() => chordSymbolFromAnalysis({ root: 'H', bass: 'C', quality: 0 } as never)).toThrow(
      /root/,
    );
  });
});

describe('keyModeFromAnalysis', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    ['major', 1],
    ['minor', 2],
    ['dorian', 3],
    ['phrygian', 4],
    ['lydian', 5],
    ['mixolydian', 6],
    ['locrian', 7],
  ] as const)('maps %s to annotation mode %i, which annotateKeys accepts', (mode, mapped) => {
    expect(keyModeFromAnalysis(mode)).toBe(mapped);
    const project = new Project();
    try {
      project.setSampleRate(48000);
      project.annotateKeys([
        { startPpq: 0, endPpq: 4, tonicPc: 0, mode: keyModeFromAnalysis(mode) },
      ]);
      expect(JSON.parse(project.toJson()).annotation.keys[0].mode).toBe(mapped);
    } finally {
      project.destroy();
    }
  });

  it('accepts a mode ordinal', () => {
    expect(keyModeFromAnalysis(1)).toBe(2);
  });

  it('refuses a wrong type as a TypeError and an unknown mode as a RangeError naming mode', () => {
    expect(() => keyModeFromAnalysis(null as never)).toThrow(TypeError);
    expect(() => keyModeFromAnalysis({} as never)).toThrow(TypeError);
    expect(() => keyModeFromAnalysis('ionian' as never)).toThrow(RangeError);
    expect(() => keyModeFromAnalysis('ionian' as never)).toThrow(/mode/);
    expect(() => keyModeFromAnalysis(7)).toThrow(RangeError);
    expect(() => keyModeFromAnalysis(-1)).toThrow(RangeError);
  });
});
