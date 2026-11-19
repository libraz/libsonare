import { withEntry } from './internal/entry.js';
import { KEY_MODES } from './internal/key_modes.js';
import { scaleReferenceMidiArg } from './internal/scale_reference.js';
import { addon } from './native.js';
import type { KeyMode, PitchClass } from './types_analysis.js';
import { assertClosedToken, assertFiniteScalar } from './validation.js';

/**
 * The 12-bit scale mask of a mode, in the layout {@link scaleQuantizeMidi} and
 * the pitch correctors read: bit `i` is the semitone `i` above the root.
 *
 * The mask is relative to the root, so it is the same for every `root`, which is
 * still validated. `mode` is a church-mode name as {@link detectKey} reports it
 * (`'major'`, `'minor'`, `'dorian'`, `'phrygian'`, `'lydian'`, `'mixolydian'`,
 * `'locrian'`); minor is the natural minor. A number is refused (`TypeError`) and
 * so is an unknown name (`RangeError`).
 *
 * @example
 * scaleMaskForMode(0, 'major'); // 0b101010110101
 */
export function scaleMaskForMode(root: PitchClass, mode: KeyMode): number {
  return withEntry('scaleMaskForMode', () => {
    assertClosedToken(mode, KEY_MODES, 'mode');
    return addon.scaleMaskForMode(root, mode);
  });
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `scaleReferenceMidi` anchors the grid; omit it for A4 (69). An explicit 0 is
 * refused (`RangeError`) until the native layer reads it as MIDI note 0.
 */
export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  scaleReferenceMidi?: number,
): number {
  return withEntry('scaleQuantizeMidi', () => {
    assertFiniteScalar(midi, 'midi');
    const reference = scaleReferenceMidiArg(scaleReferenceMidi);
    return addon.scaleQuantizeMidi(root, modeMask, midi, reference);
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
    const reference = scaleReferenceMidiArg(scaleReferenceMidi);
    return addon.scaleCorrectionSemitones(root, modeMask, midi, reference);
  });
}

/** Whether `pitchClass` (0-11, absolute) is enabled by `modeMask` relative to `root`. */
export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return addon.scalePitchClassEnabled(root, modeMask, pitchClass);
}
