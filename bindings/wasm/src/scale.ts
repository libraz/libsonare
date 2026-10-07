import { getSonareModule } from './module_state.js';
import type { Mode, ScaleName } from './public_types_music.js';
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
 * `'dorian'`, `'phrygian'`, `'lydian'`, `'mixolydian'`, `'locrian'`) or the
 * `Mode` ordinal `detectKey` reports; minor is the natural minor.
 *
 * @example
 * scaleMaskForMode(0, 'major'); // 0b101010110101
 */
export function scaleMaskForMode(root: number, mode: ScaleName | Mode): number {
  return getSonareModule().scaleMaskForMode(root, mode);
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `referenceMidi` defaults to A4 (69) when passed as 0.
 */
export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleQuantizeMidi', midi, 'midi');
  assertFiniteScalar('scaleQuantizeMidi', referenceMidi, 'referenceMidi');
  return getSonareModule().scaleQuantizeMidi(root, modeMask, midi, referenceMidi);
}

export function scaleCorrectionSemitones(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleCorrectionSemitones', midi, 'midi');
  assertFiniteScalar('scaleCorrectionSemitones', referenceMidi, 'referenceMidi');
  return getSonareModule().scaleCorrectionSemitones(root, modeMask, midi, referenceMidi);
}

export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return getSonareModule().scalePitchClassEnabled(root, modeMask, pitchClass);
}
