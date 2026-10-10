import { keyModeOrdinal, pitchClassOrdinal } from './analysis_helpers.js';
import { resolveEnumOrdinal } from './codes.js';
import { projectModule } from './project_internal.js';
import type { Chord, KeyMode } from './public_types.js';
import { ChordQuality } from './public_types.js';

/**
 * The annotation fields of one chord, named as {@link ProjectChordSymbol} names
 * them so the result spreads into an {@link Project.annotateChords} entry.
 */
export interface ChordAnnotationFields {
  /** Root pitch class 0..11 (C=0); kept as given, including for an unknown chord. */
  rootPc: number;
  /** Annotation chord quality 0..7, not the analysis quality. */
  quality: number;
  /** Extensions as scale degrees, e.g. `[7, 9, 13]` for a dominant 13th. */
  extensions: number[];
  /** Slash-bass pitch class 0..11, or 255 when the bass is the root. */
  slashBassPc: number;
}

/**
 * Converts a chord from analysis into the fields of a project chord annotation.
 *
 * Analysis and annotation number their chord qualities differently (25 analysis
 * qualities fold into 8 annotation qualities plus extensions), so an ordinal
 * offset mislabels most extended chords. This is the one conversion the library
 * itself applies when it builds a harmonic timeline. Times are not converted: the
 * caller places the result on the timeline with `startPpq` / `endPpq`.
 *
 * @param chord - An analysis chord; `root`, `bass` and `quality` are read, as a
 *   pitch class and chord quality name or ordinal
 * @returns `{ rootPc, quality, extensions, slashBassPc }`
 * @throws {TypeError} when `chord` is not an object
 * @throws {RangeError} naming `root`, `bass` or `quality` for an unrecognised value
 *
 * @example
 * ```typescript
 * const { chords } = detectChords(samples, sampleRate);
 * project.annotateChords(
 *   chords.map((c) => ({ startPpq: 0, endPpq: 4, ...chordSymbolFromAnalysis(c) })),
 * );
 * ```
 */
export function chordSymbolFromAnalysis(
  chord: Pick<Chord, 'root' | 'bass' | 'quality'>,
): ChordAnnotationFields {
  if (typeof chord !== 'object' || chord === null || Array.isArray(chord)) {
    throw new TypeError('chordSymbolFromAnalysis: chord must be an object');
  }
  const root = pitchClassOrdinal(chord.root, 'root');
  const bass = pitchClassOrdinal(chord.bass, 'bass');
  const quality = resolveEnumOrdinal(chord.quality, ChordQuality, 'quality');
  return projectModule().chordSymbolFromAnalysis(root, quality, bass);
}

/**
 * Converts a key mode from analysis into the `mode` of a
 * {@link ProjectKeySegment} for {@link Project.annotateKeys}.
 *
 * The annotation numbering reserves 0 for an unknown mode, so every analysis mode
 * lands one ordinal higher.
 *
 * @param mode - A mode name (`'major'`, `'minor'`, `'dorian'`, ...) or ordinal
 * @returns The annotation mode ordinal, 1..7
 * @throws {TypeError} when `mode` is neither a string nor a number
 * @throws {RangeError} naming `mode` for an unrecognised value
 */
export function keyModeFromAnalysis(mode: KeyMode | number): number {
  return projectModule().keyModeFromAnalysis(keyModeOrdinal(mode, 'mode'));
}
