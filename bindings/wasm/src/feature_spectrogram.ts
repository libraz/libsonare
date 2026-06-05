import { getSonareModule } from './module_state';
import type {
  ChromaResult,
  MelPowerResult,
  MelSpectrogramResult,
  MfccResult,
  StftPowerResult,
  StftResult,
} from './public_types';
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

function validateSpectrogramSamples(
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

function validateMelFrequencyRange(
  fnName: string,
  fmin: number,
  fmax: number,
  sampleRate: number,
): void {
  assertFiniteScalar(fnName, fmin, 'fmin');
  assertFiniteScalar(fnName, fmax, 'fmax');
  if (fmin < 0) {
    throw new RangeError(`${fnName}: fmin must be non-negative`);
  }
  if (fmax < 0) {
    throw new RangeError(`${fnName}: fmax must be non-negative`);
  }
  const effectiveFmax = fmax === 0 ? sampleRate / 2 : fmax;
  if (effectiveFmax <= fmin) {
    throw new RangeError(`${fnName}: fmax must be greater than fmin`);
  }
}

function validateMatrix(
  fnName: string,
  data: Float32Array,
  rows: number,
  frames: number,
  dataName: string,
  rowName: string,
  options: GuardedOptions = {},
): void {
  validatePositiveIntegers(fnName, { [rowName]: rows, nFrames: frames });
  assertSamples(fnName, data, options.validate !== false, dataName);
  const expectedLength = rows * frames;
  if (!Number.isSafeInteger(expectedLength) || data.length !== expectedLength) {
    throw new RangeError(`${fnName}: ${dataName} length must equal ${rowName} * nFrames`);
  }
}

/**
 * Trim silence from beginning and end of audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param thresholdDb - Silence threshold in dB (default: -60 dB)
 * @returns Trimmed audio
 */
export function trim(
  samples: Float32Array,
  sampleRate: number,
  thresholdDb = -60.0,
  options: GuardedOptions = {},
): Float32Array {
  validateSpectrogramSamples('trim', samples, sampleRate, options);
  assertFiniteScalar('trim', thresholdDb, 'thresholdDb');
  return requireModule().trim(samples, sampleRate, thresholdDb);
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Compute Short-Time Fourier Transform (STFT).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with magnitude and power spectrograms
 */
export function stft(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): StftResult {
  validateSpectrogramSamples('stft', samples, sampleRate, options);
  validatePositiveIntegers('stft', { nFft, hopLength });
  return requireModule().stft(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute STFT and return magnitude in decibels.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with dB values
 */
export function stftDb(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): { nBins: number; nFrames: number; db: Float32Array } {
  validateSpectrogramSamples('stftDb', samples, sampleRate, options);
  validatePositiveIntegers('stftDb', { nFft, hopLength });
  return requireModule().stftDb(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute Chroma Energy Normalized Statistics.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param nChroma - Number of chroma bins (default: 12)
 * @returns Chroma result
 */
export function chromaCens(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  options: GuardedOptions = {},
): ChromaResult {
  validateSpectrogramSamples('chromaCens', samples, sampleRate, options);
  validatePositiveIntegers('chromaCens', { hopLength, nChroma });
  return requireModule().chromaCens(samples, sampleRate, hopLength, nChroma);
}

/**
 * Compute low-frequency bass chroma.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param nChroma - Number of chroma bins (default: 12)
 * @returns Chroma result
 */
export function bassChroma(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  options: GuardedOptions = {},
): ChromaResult {
  validateSpectrogramSamples('bassChroma', samples, sampleRate, options);
  validatePositiveIntegers('bassChroma', { hopLength, nChroma });
  return requireModule().bassChroma(samples, sampleRate, hopLength, nChroma);
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

/**
 * Compute Mel spectrogram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param fmin - Minimum Mel frequency in Hz (default: 0 = librosa default).
 *   Set with `fmax` to round-trip with `melToStft` / `melToAudio`.
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sampleRate / 2)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Mel spectrogram result
 */
export function melSpectrogram(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  fmin = 0,
  fmax = 0,
  htk = false,
  options: GuardedOptions = {},
): MelSpectrogramResult {
  validateSpectrogramSamples('melSpectrogram', samples, sampleRate, options);
  validatePositiveIntegers('melSpectrogram', { nFft, hopLength, nMels });
  validateMelFrequencyRange('melSpectrogram', fmin, fmax, sampleRate);
  return requireModule().melSpectrogram(
    samples,
    sampleRate,
    nFft,
    hopLength,
    nMels,
    fmin,
    fmax,
    htk,
  );
}

/**
 * Compute MFCC (Mel-Frequency Cepstral Coefficients).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param nMfcc - Number of MFCC coefficients (default: 20)
 * @param fmin - Minimum Mel frequency in Hz (default: 0 = librosa default)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sampleRate / 2)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns MFCC result
 */
export function mfcc(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 20,
  fmin = 0,
  fmax = 0,
  htk = false,
  options: GuardedOptions = {},
): MfccResult {
  validateSpectrogramSamples('mfcc', samples, sampleRate, options);
  validatePositiveIntegers('mfcc', { nFft, hopLength, nMels, nMfcc });
  validateMelFrequencyRange('mfcc', fmin, fmax, sampleRate);
  return requireModule().mfcc(samples, sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk);
}

// ============================================================================
// Features - Inverse reconstruction
// ============================================================================

/**
 * Approximate inverse of a Mel filterbank: Mel power spectrogram -> STFT power
 * spectrogram. Mirrors `feature::mel_to_stft`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param fmin - Lower Mel band edge in Hz (default: 0)
 * @param fmax - Upper Mel band edge in Hz (default: sr/2 when 0)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns STFT power spectrogram result
 */
export function melToStft(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate = 22050,
  nFft = 2048,
  fmin = 0,
  fmax = 0,
  htk = false,
  options: GuardedOptions = {},
): StftPowerResult {
  assertSampleRate('melToStft', sampleRate);
  validateMatrix('melToStft', melPower, nMels, nFrames, 'melPower', 'nMels', options);
  validatePositiveIntegers('melToStft', { nFft });
  validateMelFrequencyRange('melToStft', fmin, fmax, sampleRate);
  return requireModule().melToStft(melPower, nMels, nFrames, sampleRate, nFft, fmin, fmax, htk);
}

/**
 * Reconstruct audio from a Mel power spectrogram via Griffin-Lim. Mirrors
 * `feature::mel_to_audio`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum Mel frequency in Hz (default: 0)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sr/2)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function melToAudio(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
  options: GuardedOptions = {},
): Float32Array {
  assertSampleRate('melToAudio', sampleRate);
  validateMatrix('melToAudio', melPower, nMels, nFrames, 'melPower', 'nMels', options);
  validatePositiveIntegers('melToAudio', { nFft, hopLength, nIter });
  validateMelFrequencyRange('melToAudio', fmin, fmax, sampleRate);
  return requireModule().melToAudio(
    melPower,
    nMels,
    nFrames,
    sampleRate,
    nFft,
    hopLength,
    fmin,
    fmax,
    nIter,
    htk,
  );
}

/**
 * Invert MFCC coefficients back to a Mel power spectrogram. Mirrors
 * `feature::mfcc_to_mel`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins to reconstruct (default: 128)
 * @returns Mel power spectrogram result
 */
export function mfccToMel(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels = 128,
  options: GuardedOptions = {},
): MelPowerResult {
  validateMatrix(
    'mfccToMel',
    mfccCoefficients,
    nMfcc,
    nFrames,
    'mfccCoefficients',
    'nMfcc',
    options,
  );
  validatePositiveIntegers('mfccToMel', { nMels });
  return requireModule().mfccToMel(mfccCoefficients, nMfcc, nFrames, nMels);
}

/**
 * Reconstruct audio directly from MFCC coefficients via Griffin-Lim. Mirrors
 * `feature::mfcc_to_audio`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins (default: 128)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum Mel frequency in Hz (default: 0)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sr/2)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function mfccToAudio(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels = 128,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
  options: GuardedOptions = {},
): Float32Array {
  assertSampleRate('mfccToAudio', sampleRate);
  validateMatrix(
    'mfccToAudio',
    mfccCoefficients,
    nMfcc,
    nFrames,
    'mfccCoefficients',
    'nMfcc',
    options,
  );
  validatePositiveIntegers('mfccToAudio', { nMels, nFft, hopLength, nIter });
  validateMelFrequencyRange('mfccToAudio', fmin, fmax, sampleRate);
  return requireModule().mfccToAudio(
    mfccCoefficients,
    nMfcc,
    nFrames,
    nMels,
    sampleRate,
    nFft,
    hopLength,
    fmin,
    fmax,
    nIter,
    htk,
  );
}

// ============================================================================
// Features - Chroma
// ============================================================================

/**
 * Compute chromagram (pitch class distribution).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Chroma features result
 */
export function chroma(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): ChromaResult {
  validateSpectrogramSamples('chroma', samples, sampleRate, options);
  validatePositiveIntegers('chroma', { nFft, hopLength });
  return requireModule().chroma(samples, sampleRate, nFft, hopLength);
}
