import { chordQualityOrdinal, keyModeOrdinal, pitchClassOrdinal } from './analysis_helpers.js';
import { projectChordQualityName, projectKeyModeName, projectModule } from './project_internal.js';
import type { ProjectChordQuality, ProjectKeyMode } from './project_types.js';
import type { Chord, KeyMode } from './public_types.js';

/** The native layer's "bass is the root" value, which the result spells as an absent key. */
const NO_SLASH_BASS = 255;

/**
 * The annotation fields of one chord, named as {@link ProjectChordSymbol} names
 * them so the result spreads into an {@link Project.annotateChords} entry.
 */
export interface ChordSymbolFields {
  /** Root pitch class 0..11 (C=0); kept as given, including for an unknown chord. */
  rootPc: number;
  /** Annotation chord quality, not the analysis quality. */
  quality: ProjectChordQuality;
  /** Extensions as scale degrees, e.g. `[7, 9, 13]` for a dominant 13th. */
  extensions: number[];
  /** Slash-bass pitch class 0..11; absent when the bass is the root. */
  slashBassPc?: number;
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
 *   pitch class 0-11 and a chord quality token
 * @returns `{ rootPc, quality, extensions }`, plus `slashBassPc` when the bass
 *   differs from the root
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
): ChordSymbolFields {
  if (typeof chord !== 'object' || chord === null || Array.isArray(chord)) {
    throw new TypeError('chordSymbolFromAnalysis: chord must be an object');
  }
  const root = pitchClassOrdinal('chordSymbolFromAnalysis', chord.root, 'root');
  const bass = pitchClassOrdinal('chordSymbolFromAnalysis', chord.bass, 'bass');
  const quality = chordQualityOrdinal(chord.quality, 'quality');
  const { slashBassPc, ...native } = projectModule().chordSymbolFromAnalysis(root, quality, bass);
  const fields: ChordSymbolFields = { ...native, quality: projectChordQualityName(native.quality) };
  if (slashBassPc !== NO_SLASH_BASS) {
    fields.slashBassPc = slashBassPc;
  }
  return fields;
}

/**
 * Converts a key mode from analysis into the `mode` of a
 * {@link ProjectKeySegment} for {@link Project.annotateKeys}.
 *
 * Every analysis mode has an annotation mode of the same name.
 *
 * @param mode - A mode name (`'major'`, `'minor'`, `'dorian'`, ...)
 * @returns The annotation mode token
 * @throws {TypeError} when `mode` is not a string
 * @throws {RangeError} naming `mode` for an unrecognised value
 */
export function keyModeFromAnalysis(mode: KeyMode): ProjectKeyMode {
  return projectKeyModeName(projectModule().keyModeFromAnalysis(keyModeOrdinal(mode, 'mode')));
}
