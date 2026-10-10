import { addon } from './native.js';
import type { AnalysisChord, Chord } from './types_analysis.js';
import type { ProjectChordSymbol } from './types_project.js';

/** The annotation fields {@link chordSymbolFromAnalysis} fills; spreads into a {@link ProjectChordSymbol}. */
export type ChordSymbolFields = Required<
  Pick<ProjectChordSymbol, 'rootPc' | 'quality' | 'extensions' | 'slashBassPc'>
>;

/**
 * Convert a chord from analysis into the root, quality, extensions and slash bass
 * of a chord annotation.
 *
 * Analysis and annotations number chord qualities differently, so the quality is
 * mapped through the library's own table rather than by index. The result spreads
 * into an entry for {@link Project.annotateChords}; the caller supplies
 * `startPpq` and `endPpq`.
 *
 * @param chord A {@link Chord} (string root, bass and quality) or an
 *        {@link AnalysisChord} (ordinals).
 * @throws `TypeError` when `chord` is not an object or a field has the wrong type;
 *         `RangeError` naming the field for an unrecognised pitch class or quality.
 *
 * @example
 * ```typescript
 * project.annotateChords([{ startPpq: 0, endPpq: 4, ...chordSymbolFromAnalysis(chord) }]);
 * ```
 */
export function chordSymbolFromAnalysis(chord: Chord | AnalysisChord): ChordSymbolFields {
  return addon.chordSymbolFromAnalysis(chord);
}

/**
 * Convert an analysis key mode name (for example `'minor'`) into the `mode` of a
 * key annotation for {@link Project.annotateKeys}.
 *
 * @throws `TypeError` when `mode` is not a string; `RangeError` for an
 *         unrecognised mode name.
 */
export function keyModeFromAnalysis(mode: string): number {
  return addon.keyModeFromAnalysis(mode);
}
