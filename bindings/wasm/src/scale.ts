import { keyModeOrdinal } from './analysis_helpers.js';
import { withEntry } from './internal/entry.js';
import { getSonareModule } from './module_state.js';
import type { KeyMode, PitchClass } from './public_types_music.js';
import { assertFiniteScalar } from './validation.js';

// ============================================================================
// Editing — 12-TET scale quantizer
// ============================================================================

/**
 * The 12-bit scale mask of a mode, in the layout `scaleQuantizeMidi` and the
 * pitch correctors read: bit `i` is the semitone `i` above the root.
 *
 * The mask is relative to the root, so it is the same for every `root`, which
 * is still validated. `mode` is a church-mode name (`'major'`, `'minor'`,
 * `'dorian'`, `'phrygian'`, `'lydian'`, `'mixolydian'`, `'locrian'`); minor is
 * the natural minor. A number or an unknown name is refused (`TypeError` for a
 * non-string, `RangeError` for an unknown token).
 *
 * @example
 * ```ts
 * scaleMaskForMode(0, 'major'); // 0b101010110101
 * ```
 */
export function scaleMaskForMode(root: PitchClass, mode: KeyMode): number {
  return withEntry('scaleMaskForMode', () => {
    return getSonareModule().scaleMaskForMode(root, keyModeOrdinal(mode, 'mode'));
  });
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `scaleReferenceMidi` anchors the grid, in [0, 127]; omit it for A4 (69). 0 is
 * MIDI note 0.
 */
export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  scaleReferenceMidi?: number,
): number {
  return withEntry('scaleQuantizeMidi', () => {
    assertFiniteScalar(midi, 'midi');
    return getSonareModule().scaleQuantizeMidi(root, modeMask, midi, scaleReferenceMidi);
  });
}

/** The signed correction in semitones that moves `midi` onto the scale; `scaleReferenceMidi` as in {@link scaleQuantizeMidi}. */
export function scaleCorrectionSemitones(
  root: number,
  modeMask: number,
  midi: number,
  scaleReferenceMidi?: number,
): number {
  return withEntry('scaleCorrectionSemitones', () => {
    assertFiniteScalar(midi, 'midi');
    return getSonareModule().scaleCorrectionSemitones(root, modeMask, midi, scaleReferenceMidi);
  });
}

/** Whether `pitchClass` (0-11, absolute) is enabled by `modeMask` relative to `root`. */
export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return withEntry('scalePitchClassEnabled', () => {
    return getSonareModule().scalePitchClassEnabled(root, modeMask, pitchClass);
  });
}
