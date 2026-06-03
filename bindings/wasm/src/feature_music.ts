import { getSonareModule } from './module_state';
import type { CqtResult, LufsResult, MelodyResult, Section, SectionType } from './public_types';
import type { WasmFourierTempogramResult, WasmNnlsChromaResult } from './sonare.js';
import { assertSamples } from './validation';
import type { ValidateOptions } from './validation';

function requireModule() {
  return getSonareModule();
}

/**
 * Compute NNLS (non-negative least squares) chromagram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns NNLS chroma result
 */
export function nnlsChroma(samples: Float32Array, sampleRate = 22050): WasmNnlsChromaResult {
  return requireModule().nnlsChroma(samples, sampleRate);
}

/**
 * Compute the Constant-Q Transform magnitude.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @returns CQT magnitude result
 */
export function cqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  return requireModule().cqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
}

/**
 * Compute the Variable-Q Transform magnitude (gamma controls Q).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @param gamma - Bandwidth offset; 0 is equivalent to CQT (default: 0)
 * @returns VQT magnitude result (same shape as CQT)
 */
export function vqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  gamma = 0,
): CqtResult {
  return requireModule().vqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave, gamma);
}

/**
 * Detect song-structure sections (intro/verse/chorus/...).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param minSectionSec - Minimum section duration in seconds (default: 4.0)
 * @returns Array of detected sections
 */
export function analyzeSections(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  minSectionSec = 4.0,
): Section[] {
  return requireModule()
    .analyzeSections(samples, sampleRate, nFft, hopLength, minSectionSec)
    .map((s) => ({ ...s, type: s.type as SectionType }));
}

/**
 * Options selecting the melody tracker. Both default to the historical
 * behaviour for backward compatibility.
 */
export interface MelodyOptions {
  /**
   * Use the pYIN tracker (Viterbi-smoothed) instead of plain per-frame YIN.
   * Produces a less octave-jumpy contour. Defaults to `false`.
   */
  usePyin?: boolean;
  /**
   * When {@link usePyin} is `true`, reflect-pad by `frameLength / 2` so frame
   * `i` is centered at `i * hopLength` (matches `librosa.pyin(center=True)`).
   * Ignored by the plain-YIN path. Defaults to `true`.
   */
  center?: boolean;
}

/**
 * Extract the melody contour from monophonic audio via YIN (or pYIN).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param fmin - Minimum frequency in Hz (default: 65.0)
 * @param fmax - Maximum frequency in Hz (default: 2093.0)
 * @param frameLength - Frame length in samples (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param threshold - YIN threshold; lower is stricter (default: 0.1)
 * @param options - Tracker options ({@link MelodyOptions.usePyin} / {@link MelodyOptions.center})
 * @returns Melody contour with per-frame pitch points and summary stats
 */
export function analyzeMelody(
  samples: Float32Array,
  sampleRate = 22050,
  fmin = 65.0,
  fmax = 2093.0,
  frameLength = 2048,
  hopLength = 256,
  threshold = 0.1,
  options: MelodyOptions = {},
): MelodyResult {
  return requireModule().analyzeMelody(
    samples,
    sampleRate,
    fmin,
    fmax,
    frameLength,
    hopLength,
    threshold,
    options.usePyin ?? false,
    options.center ?? true,
  );
}

/**
 * Compute the onset strength envelope.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @returns Onset envelope for each frame
 */
export function onsetEnvelope(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): Float32Array {
  return requireModule().onsetEnvelope(samples, sampleRate, nFft, hopLength, nMels);
}

/**
 * Compute the Fourier tempogram from an onset envelope.
 *
 * @param onsetEnvelope - Onset strength envelope (float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param winLength - Window length in frames (default: 384)
 * @returns Fourier tempogram result
 */
export function fourierTempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
): WasmFourierTempogramResult {
  return requireModule().fourierTempogram(onsetEnvelope, sampleRate, hopLength, winLength);
}

/**
 * Compute tempogram ratio features.
 *
 * @param tempogramData - Tempogram data (float32)
 * @param winLength - Window length in frames (default: 384)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @returns Tempogram ratio features
 */
export function tempogramRatio(
  tempogramData: Float32Array,
  winLength = 384,
  sampleRate = 22050,
  hopLength = 512,
): Float32Array {
  return requireModule().tempogramRatio(tempogramData, winLength, sampleRate, hopLength);
}

/**
 * Measure loudness (EBU R128 / ITU-R BS.1770).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Loudness measurement result
 */
export function lufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): LufsResult {
  assertSamples('lufs', samples, options.validate !== false);
  return requireModule().lufs(samples, sampleRate);
}

/**
 * Compute the momentary loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Momentary LUFS values over time
 */
export function momentaryLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('momentaryLufs', samples, options.validate !== false);
  return requireModule().momentaryLufs(samples, sampleRate);
}

/**
 * Compute the short-term loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Short-term LUFS values over time
 */
export function shortTermLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('shortTermLufs', samples, options.validate !== false);
  return requireModule().shortTermLufs(samples, sampleRate);
}
