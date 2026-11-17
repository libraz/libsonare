import { keyModeOrdinal } from './analysis_helpers.js';
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
 * scaleMaskForMode(0, 'major'); // 0b101010110101
 */
export function scaleMaskForMode(root: PitchClass, mode: KeyMode): number {
  return getSonareModule().scaleMaskForMode(root, keyModeOrdinal(mode, 'mode'));
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `scaleReferenceMidi` defaults to A4 (69) when passed as 0.
 */
export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  scaleReferenceMidi = 0,
): number {
  assertFiniteScalar('scaleQuantizeMidi', midi, 'midi');
  assertFiniteScalar('scaleQuantizeMidi', scaleReferenceMidi, 'scaleReferenceMidi');
  return getSonareModule().scaleQuantizeMidi(root, modeMask, midi, scaleReferenceMidi);
}

export function scaleCorrectionSemitones(
  root: number,
  modeMask: number,
  midi: number,
  scaleReferenceMidi = 0,
): number {
  assertFiniteScalar('scaleCorrectionSemitones', midi, 'midi');
  assertFiniteScalar('scaleCorrectionSemitones', scaleReferenceMidi, 'scaleReferenceMidi');
  return getSonareModule().scaleCorrectionSemitones(root, modeMask, midi, scaleReferenceMidi);
}

export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return getSonareModule().scalePitchClassEnabled(root, modeMask, pitchClass);
}
