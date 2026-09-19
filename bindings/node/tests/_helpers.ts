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

export interface GlideOptions extends SineOptions {
  /** Interval swept over the whole duration, in semitones (default 11). */
  semitones?: number;
}

/**
 * Generate a continuous pitch glide, two partials deep.
 *
 * For anything that recovers a time correspondence between two signals: the
 * chroma moves every frame, so no two frames carry the same pitch class and one
 * alignment is strictly cheaper than the rest. A held tone leaves the path
 * unconstrained along its whole length and the recovered anchors say nothing.
 *
 * @param startHz Starting frequency in Hz; the sweep ends `semitones` above it.
 * @param durationSec Duration in seconds; the sample count is floor(sampleRate * durationSec).
 * @param opts Optional sample rate, amplitude (default 0.4) and interval.
 * @returns The generated samples.
 */
export function glide(startHz: number, durationSec: number, opts: GlideOptions = {}): Float32Array {
  const { sampleRate = 44100, amp = 0.4, semitones = 11 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  let phase = 0;
  let octavePhase = 0;
  for (let i = 0; i < n; i++) {
    const hz = startHz * 2 ** ((semitones * (i / n)) / 12);
    phase += (2 * Math.PI * hz) / sampleRate;
    octavePhase += (2 * Math.PI * hz * 2) / sampleRate;
    out[i] = amp * (Math.sin(phase) + 0.5 * Math.sin(octavePhase));
  }
  return out;
}
