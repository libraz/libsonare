/**
 * Shared test signal helpers.
 *
 * `sine` is the canonical replacement for the per-file Math.sin loops. Argument
 * order and defaults are fixed here; each test keeps its own thin wrapper when
 * its historical call sites use a different arg order or sample-count shape.
 */

export interface SineOptions {
  /** Sample rate in Hz (default 44100). */
  sampleRate?: number;
  /** Peak amplitude (default 0.5). */
  amp?: number;
}

/**
 * Generate a mono sine wave.
 *
 * @param freqHz Frequency in Hz.
 * @param durationSec Duration in seconds; the sample count is floor(sampleRate * durationSec).
 * @param opts Optional sample rate and amplitude.
 * @returns The generated samples.
 */
export function sine(freqHz: number, durationSec: number, opts: SineOptions = {}): Float32Array {
  const { sampleRate = 44100, amp = 0.5 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}
