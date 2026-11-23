import { withEntry } from './internal/entry.js';
import { KEY_MODES } from './internal/key_modes.js';
import { CHORD_QUALITY, KEY_MODE, tokenName } from './internal/project_tokens.js';
import { addon } from './native.js';
import type { AnalysisChord, Chord } from './types_analysis.js';
import type { ProjectChordSymbol, ProjectKeyMode } from './types_project.js';
import { assertClosedToken } from './validation.js';

/** The native layer's "bass is the root" value, which the result spells as an absent key. */
const NO_SLASH_BASS = 255;

/** The annotation fields {@link chordSymbolFromAnalysis} fills; spreads into a {@link ProjectChordSymbol}. */
export type ChordSymbolFields = Required<
  Pick<ProjectChordSymbol, 'rootPc' | 'quality' | 'extensions'>
> &
  Pick<ProjectChordSymbol, 'slashBassPc'>;

/**
 * Convert a chord from analysis into the root, quality, extensions and slash bass
 * of a chord annotation.
 *
 * Analysis and annotations number chord qualities differently, so the quality is
 * mapped through the library's own table rather than by index. The result spreads
 * into an entry for {@link Project.annotateChords}; the caller supplies
 * `startPpq` and `endPpq`. `slashBassPc` is present only when the bass differs
 * from the root.
 *
 * @param chord A {@link Chord} or an {@link AnalysisChord}: `root` and `bass` are pitch
 *        classes and `quality` is a {@link ChordQuality} token.
 * @throws `TypeError` when `chord` is not an object or a field has the wrong type;
 *         `RangeError` naming the field for an unrecognised pitch class or quality.
 *
 * @example
 * ```ts
 * const [chord] = detectChords({ samples, sampleRate }).chords;
 * project.annotateChords([{ startPpq: 0, endPpq: 4, ...chordSymbolFromAnalysis(chord) }]);
 * ```
 */
export function chordSymbolFromAnalysis(chord: Chord | AnalysisChord): ChordSymbolFields {
  return withEntry('chordSymbolFromAnalysis', () => {
    const { slashBassPc, ...native } = addon.chordSymbolFromAnalysis(chord);
    const fields: ChordSymbolFields = {
      ...native,
      quality: tokenName(CHORD_QUALITY, native.quality, 'chord quality'),
    };
    if (slashBassPc !== NO_SLASH_BASS) {
      fields.slashBassPc = slashBassPc;
    }
    return fields;
  });
}

/**
 * Convert an analysis key mode name (for example `'minor'`) into the `mode` of a
 * key annotation for {@link Project.annotateKeys}.
 *
 * @throws `TypeError` when `mode` is not a string; `RangeError` for an
 *         unrecognised mode name.
 */
export function keyModeFromAnalysis(mode: string): ProjectKeyMode {
  return withEntry('keyModeFromAnalysis', () => {
    assertClosedToken(mode, KEY_MODES, 'mode');
    return tokenName(KEY_MODE, addon.keyModeFromAnalysis(mode), 'key mode');
  });
}
