import { getSonareModule } from './module_state';
import type {
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmLufsResult,
  WasmMatrix2dResult,
} from './sonare.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Features - Spectral
// ============================================================================

/**
 * Compute spectral centroid (center of mass of spectrum).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral centroid in Hz for each frame
 */
export function spectralCentroid(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().spectralCentroid(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral contrast (librosa.feature.spectral_contrast).
 *
 * @returns Matrix2d of shape (nBands + 1) x nFrames.
 */
export function spectralContrast(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nBands = 6,
  fmin = 200.0,
  quantile = 0.02,
): WasmMatrix2dResult {
  return requireModule().spectralContrast(
    samples,
    sampleRate,
    nFft,
    hopLength,
    nBands,
    fmin,
    quantile,
  );
}

/**
 * Fit per-frame polynomial coefficients (librosa.feature.poly_features).
 *
 * @returns Matrix2d of shape (order + 1) x nFrames.
 */
export function polyFeatures(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  order = 1,
): WasmMatrix2dResult {
  return requireModule().polyFeatures(samples, sampleRate, nFft, hopLength, order);
}

/**
 * Locate zero-crossing indices of a signal (librosa.zero_crossings).
 */
export function zeroCrossings(
  samples: Float32Array,
  threshold = 1e-10,
  refMagnitude = false,
  pad = true,
  zeroPos = true,
): Int32Array {
  return requireModule().zeroCrossings(samples, threshold, refMagnitude, pad, zeroPos);
}

/**
 * Estimate the global tuning offset from a set of frequencies
 * (librosa.pitch_tuning). Returns a deviation in fractions of a bin.
 */
export function pitchTuning(
  frequencies: Float32Array,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  return requireModule().pitchTuning(frequencies, resolution, binsPerOctave);
}

/**
 * Estimate the tuning offset of an audio signal (librosa.estimate_tuning).
 */
export function estimateTuning(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  return requireModule().estimateTuning(
    samples,
    sampleRate,
    nFft,
    hopLength,
    resolution,
    binsPerOctave,
  );
}

/**
 * Non-negative matrix factorisation of a flattened [nFeatures x nFrames]
 * spectrogram (librosa.decompose.decompose). Returns the W and H factors.
 */
export function decompose(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  nComponents: number,
  nIter = 50,
  beta = 2.0,
): WasmDecomposeResult {
  return requireModule().decompose(s, nFeatures, nFrames, nComponents, nIter, beta);
}

/**
 * Nearest-neighbour filtering of a flattened [nFeatures x nFrames] spectrogram
 * (librosa.decompose.nn_filter).
 */
export function nnFilter(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  aggregate = 'mean',
  k = 7,
  width = 1,
): WasmMatrix2dResult {
  return requireModule().nnFilter(s, nFeatures, nFrames, aggregate, k, width);
}

/**
 * Reorder/concatenate a signal by interval slices (librosa.effects.remix).
 *
 * @param intervals - Flat (start, end) sample pairs (even length).
 */
export function remix(
  samples: Float32Array,
  intervals: Int32Array | ArrayLike<number>,
  sampleRate = 22050,
  alignZeros = false,
): Float32Array {
  // Sample indices must reach the native side as exact 32-bit integers. Passing
  // a Float32Array (or a number[] holding fractional/large values) would round
  // boundaries above 2^24 and misalign the slice. Coerce to an Int32Array,
  // truncating toward zero, so callers can hand us any numeric array safely.
  const intervalsI32 =
    intervals instanceof Int32Array ? intervals : Int32Array.from(intervals, (v) => Math.trunc(v));
  return requireModule().remix(samples, intervalsI32, sampleRate, alignZeros);
}

/**
 * Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower).
 */
export function phaseVocoder(
  samples: Float32Array,
  rate: number,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().phaseVocoder(samples, sampleRate, rate, nFft, hopLength);
}

/**
 * HPSS into harmonic / percussive / residual signals.
 */
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): WasmHpssWithResidualResult {
  return requireModule().hpssWithResidual(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Channel-weighted multichannel integrated loudness + LRA (ITU-R BS.1770 /
 * EBU R128) from an interleaved buffer of `frames * channels` samples. The
 * per-channel frame count is derived from the buffer length and `channels`.
 */
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate = 22050,
): WasmLufsResult {
  return requireModule().lufsInterleaved(samples, channels, sampleRate);
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU.
 */
export function ebur128LoudnessRange(samples: Float32Array, sampleRate = 22050): number {
  return requireModule().ebur128LoudnessRange(samples, sampleRate);
}

/**
 * Compute spectral bandwidth.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral bandwidth in Hz for each frame
 */
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().spectralBandwidth(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral rolloff frequency.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param rollPercent - Percentage threshold (default: 0.85)
 * @returns Rolloff frequency in Hz for each frame
 */
export function spectralRolloff(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  return requireModule().spectralRolloff(samples, sampleRate, nFft, hopLength, rollPercent);
}

/**
 * Compute spectral flatness.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral flatness for each frame (0 = tonal, 1 = noise-like)
 */
export function spectralFlatness(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().spectralFlatness(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute zero crossing rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Zero crossing rate for each frame
 */
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().zeroCrossingRate(samples, sampleRate, frameLength, hopLength);
}

/**
 * Compute RMS energy.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns RMS energy for each frame
 */
export function rmsEnergy(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  return requireModule().rmsEnergy(samples, sampleRate, frameLength, hopLength);
}
