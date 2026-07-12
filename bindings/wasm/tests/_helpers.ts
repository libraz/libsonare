/**
 * Shared test-signal generators for the WASM binding suites.
 */

/** Options for {@link sine}. */
export interface SineOptions {
  /** Peak amplitude of the generated tone. Defaults to 0.5. */
  amp?: number;
  /** Sample rate in Hz. Defaults to 48000. */
  sampleRate?: number;
}

/**
 * Generates a mono sine tone as a Float32Array.
 *
 * @param freqHz - Tone frequency in Hz.
 * @param durationSec - Duration in seconds; the sample count is
 *   `floor(sampleRate * durationSec)`.
 * @param opts - Optional amplitude and sample-rate overrides.
 * @returns The generated tone.
 */
export function sine(freqHz: number, durationSec: number, opts: SineOptions = {}): Float32Array {
  const { amp = 0.5, sampleRate = 48000 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}
