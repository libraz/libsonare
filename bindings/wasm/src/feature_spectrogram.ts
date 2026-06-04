import { getSonareModule } from './module_state';
import type {
  ChromaResult,
  MelPowerResult,
  MelSpectrogramResult,
  MfccResult,
  StftPowerResult,
  StftResult,
} from './public_types';

function requireModule() {
  return getSonareModule();
}

/**
 * Trim silence from beginning and end of audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param thresholdDb - Silence threshold in dB (default: -60 dB)
 * @returns Trimmed audio
 */
export function trim(samples: Float32Array, sampleRate: number, thresholdDb = -60.0): Float32Array {
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
): StftResult {
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
): { nBins: number; nFrames: number; db: Float32Array } {
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
): ChromaResult {
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
): ChromaResult {
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
): MelSpectrogramResult {
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
): MfccResult {
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
): StftPowerResult {
  return requireModule().melToStft(melPower, nMels, nFrames, sampleRate, nFft, fmin, fmax);
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
): Float32Array {
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
): MelPowerResult {
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
): Float32Array {
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
): ChromaResult {
  return requireModule().chroma(samples, sampleRate, nFft, hopLength);
}
