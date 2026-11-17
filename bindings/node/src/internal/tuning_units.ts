/**
 * Tuning unit conversion at the native boundary.
 *
 * The public surface speaks cents; native still reports a fraction of a bin
 * (a semitone fraction at 12 bins per octave), so every site converts here.
 */

/** Native tuning (fraction of one of `binsPerOctave` bins) to cents. */
export function nativeTuningToCents(tuning: number, binsPerOctave = 12): number {
  return (tuning * 1200) / binsPerOctave;
}

/** Cents to the native semitone fraction. */
export function centsToNativeTuning(cents: number): number {
  return cents / 100;
}
