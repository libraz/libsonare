import { getSonareModule } from './module_state';
import type {
  AnalyzeSectionsOptions,
  CqtResult,
  LufsResult,
  MelodyResult,
  OnsetStrengthMultiResult,
  Section,
  SectionType,
} from './public_types';
import type { WasmFourierTempogramResult, WasmNnlsChromaResult } from './sonare.js';
import type { ValidateOptions } from './validation';
import {
  assertFiniteScalar,
  assertPositiveInteger,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

type GuardedOptions = ValidateOptions;
type AnalyzeSectionsGuardedOptions = AnalyzeSectionsOptions & ValidateOptions;
type MelodyGuardedOptions = MelodyOptions & ValidateOptions;

function validateMusicSamples(
  fnName: string,
  samples: Float32Array,
  sampleRate: number,
  options: GuardedOptions = {},
): void {
  assertSampleRate(fnName, sampleRate);
  assertSamples(fnName, samples, options.validate !== false);
}

function validatePositiveIntegers(fnName: string, values: Record<string, number>): void {
  for (const [name, value] of Object.entries(values)) {
    assertPositiveInteger(fnName, value, name);
  }
}

function validateFrequencyBounds(fnName: string, fmin: number, fmax?: number): void {
  assertFiniteScalar(fnName, fmin, 'fmin');
  if (fmin < 0) {
    throw new RangeError(`${fnName}: fmin must be non-negative`);
  }
  if (fmax !== undefined) {
    assertFiniteScalar(fnName, fmax, 'fmax');
    if (fmax <= fmin) {
      throw new RangeError(`${fnName}: fmax must be greater than fmin`);
    }
  }
}

/**
 * Compute NNLS (non-negative least squares) chromagram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns NNLS chroma result
 */
export function nnlsChroma(
  samples: Float32Array,
  sampleRate = 22050,
  options: GuardedOptions = {},
): WasmNnlsChromaResult {
  validateMusicSamples('nnlsChroma', samples, sampleRate, options);
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
  options: GuardedOptions = {},
): CqtResult {
  validateMusicSamples('cqt', samples, sampleRate, options);
  validatePositiveIntegers('cqt', { hopLength, nBins, binsPerOctave });
  validateFrequencyBounds('cqt', fmin);
  return requireModule().cqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
}

/**
 * Compute the pseudo Constant-Q Transform magnitude.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @returns CQT magnitude result
 */
export function pseudoCqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  options: GuardedOptions = {},
): CqtResult {
  validateMusicSamples('pseudoCqt', samples, sampleRate, options);
  validatePositiveIntegers('pseudoCqt', { hopLength, nBins, binsPerOctave });
  validateFrequencyBounds('pseudoCqt', fmin);
  return requireModule().pseudoCqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
}

/**
 * Compute the hybrid Constant-Q Transform magnitude.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @returns CQT magnitude result
 */
export function hybridCqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  options: GuardedOptions = {},
): CqtResult {
  validateMusicSamples('hybridCqt', samples, sampleRate, options);
  validatePositiveIntegers('hybridCqt', { hopLength, nBins, binsPerOctave });
  validateFrequencyBounds('hybridCqt', fmin);
  return requireModule().hybridCqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
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
  options: GuardedOptions = {},
): CqtResult {
  validateMusicSamples('vqt', samples, sampleRate, options);
  validatePositiveIntegers('vqt', { hopLength, nBins, binsPerOctave });
  validateFrequencyBounds('vqt', fmin);
  assertFiniteScalar('vqt', gamma, 'gamma');
  if (gamma < 0) {
    throw new RangeError('vqt: gamma must be non-negative');
  }
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
  options: AnalyzeSectionsGuardedOptions = {},
): Section[] {
  validateMusicSamples('analyzeSections', samples, sampleRate, options);
  validatePositiveIntegers('analyzeSections', {
    nFft: options.nFft ?? 2048,
    hopLength: options.hopLength ?? 512,
  });
  assertFiniteScalar('analyzeSections', options.minSectionSec ?? 4.0, 'minSectionSec');
  if ((options.minSectionSec ?? 4.0) <= 0) {
    throw new RangeError('analyzeSections: minSectionSec must be positive');
  }
  return requireModule()
    .analyzeSections(
      samples,
      sampleRate,
      options.nFft ?? 2048,
      options.hopLength ?? 512,
      options.minSectionSec ?? 4.0,
    )
    .map((s) => ({ ...s, type: s.type as SectionType }));
}

/** Options for {@link analyzeMelody}. All fields are optional. */
export interface MelodyOptions {
  /** Lowest f0 (Hz) the tracker will consider. Default 65 (≈ C2). */
  fmin?: number;
  /** Highest f0 (Hz) the tracker will consider. Default 2093 (≈ C7). */
  fmax?: number;
  /** Analysis frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop length between frames in samples. Default 256. */
  hopLength?: number;
  /** Voicing confidence threshold in [0,1]; frames below are unvoiced. Default 0.1. */
  threshold?: number;
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
 * @param options - Tracker + tuning options ({@link MelodyOptions})
 * @returns Melody contour with per-frame pitch points and summary stats
 */
export function analyzeMelody(
  samples: Float32Array,
  sampleRate = 22050,
  options: MelodyGuardedOptions = {},
): MelodyResult {
  validateMusicSamples('analyzeMelody', samples, sampleRate, options);
  const fmin = options.fmin ?? 65.0;
  const fmax = options.fmax ?? 2093.0;
  validateFrequencyBounds('analyzeMelody', fmin, fmax);
  // The melody tracker's fmin is a YIN pitch floor: 0 is meaningless, and the
  // flat C ABI (sonare_analyze_melody) rejects it. validateFrequencyBounds only
  // guards fmin >= 0, so enforce strict positivity here for parity.
  if (fmin <= 0) {
    throw new RangeError('analyzeMelody: fmin must be positive');
  }
  validatePositiveIntegers('analyzeMelody', {
    frameLength: options.frameLength ?? 2048,
    hopLength: options.hopLength ?? 256,
  });
  const threshold = options.threshold ?? 0.1;
  assertFiniteScalar('analyzeMelody', threshold, 'threshold');
  if (threshold <= 0) {
    throw new RangeError('analyzeMelody: threshold must be positive');
  }
  return requireModule().analyzeMelody(
    samples,
    sampleRate,
    options.fmin ?? 65.0,
    options.fmax ?? 2093.0,
    options.frameLength ?? 2048,
    options.hopLength ?? 256,
    options.threshold ?? 0.1,
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
  options: GuardedOptions = {},
): Float32Array {
  validateMusicSamples('onsetEnvelope', samples, sampleRate, options);
  validatePositiveIntegers('onsetEnvelope', { nFft, hopLength, nMels });
  return requireModule().onsetEnvelope(samples, sampleRate, nFft, hopLength, nMels);
}

/**
 * Compute multi-band onset strength envelopes.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param nBands - Number of onset bands (default: 3)
 * @returns Multi-band onset matrix
 */
export function onsetStrengthMulti(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nBands = 3,
  options: GuardedOptions = {},
): OnsetStrengthMultiResult {
  validateMusicSamples('onsetStrengthMulti', samples, sampleRate, options);
  validatePositiveIntegers('onsetStrengthMulti', { nFft, hopLength, nMels, nBands });
  return requireModule().onsetStrengthMulti(samples, sampleRate, nFft, hopLength, nMels, nBands);
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
  options: GuardedOptions = {},
): WasmFourierTempogramResult {
  assertSampleRate('fourierTempogram', sampleRate);
  assertSamples('fourierTempogram', onsetEnvelope, options.validate !== false, 'onsetEnvelope');
  validatePositiveIntegers('fourierTempogram', { hopLength, winLength });
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
  options: GuardedOptions = {},
): Float32Array {
  assertSampleRate('tempogramRatio', sampleRate);
  assertSamples('tempogramRatio', tempogramData, options.validate !== false, 'tempogramData');
  validatePositiveIntegers('tempogramRatio', { winLength, hopLength });
  return requireModule().tempogramRatio(tempogramData, winLength, sampleRate, hopLength);
}

/**
 * Measure loudness (EBU R128 / ITU-R BS.1770).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz. The default (22050) is non-standard for
 *   audio; pass the buffer's actual rate, as K-weighting is sample-rate
 *   dependent and a wrong rate yields wrong loudness.
 * @returns Loudness measurement result
 */
export function lufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): LufsResult {
  assertSampleRate('lufs', sampleRate);
  assertSamples('lufs', samples, options.validate !== false);
  return requireModule().lufs(samples, sampleRate);
}

/**
 * Compute the momentary loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz. The default (22050) is non-standard and
 *   K-weighting is sample-rate dependent; pass the buffer's actual rate.
 * @returns Momentary LUFS values over time
 */
export function momentaryLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSampleRate('momentaryLufs', sampleRate);
  assertSamples('momentaryLufs', samples, options.validate !== false);
  return requireModule().momentaryLufs(samples, sampleRate);
}

/**
 * Compute the short-term loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz. The default (22050) is non-standard and
 *   K-weighting is sample-rate dependent; pass the buffer's actual rate.
 * @returns Short-term LUFS values over time
 */
export function shortTermLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSampleRate('shortTermLufs', sampleRate);
  assertSamples('shortTermLufs', samples, options.validate !== false);
  return requireModule().shortTermLufs(samples, sampleRate);
}
