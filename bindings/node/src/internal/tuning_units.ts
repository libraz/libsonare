/**
 * Tuning unit conversion for `estimateTuning` and `pitchTuning`, the two
 * results native still reports as a fraction of a bin (librosa mirrors); the
 * public surface speaks cents.
 */

/** Native tuning (fraction of one of `binsPerOctave` bins) to cents. */
export function nativeTuningToCents(tuning: number, binsPerOctave = 12): number {
  return (tuning * 1200) / binsPerOctave;
}
