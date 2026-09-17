/**
 * Frame-level spectral descriptors: the scalar shape measures taken per STFT
 * frame, and the zero-crossing counts beside them.
 */

import { getSonareModule } from './module_state';
import type { WasmMatrix2dResult } from './sonare.js';

function requireModule() {
  return getSonareModule();
}

/** Canonical request form for frame-based spectral feature extraction. */
export interface SpectralFrameRequest {
  samples: Float32Array;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
}

export interface SpectralRolloffRequest extends SpectralFrameRequest {
  rollPercent?: number;
}

export interface ZeroCrossingRateRequest {
  samples: Float32Array;
  sampleRate?: number;
  frameLength?: number;
  hopLength?: number;
}

export interface SpectralContrastRequest extends SpectralFrameRequest {
  nBands?: number;
  fmin?: number;
  quantile?: number;
}

export interface PolyFeaturesRequest extends SpectralFrameRequest {
  order?: number;
}

export interface ZeroCrossingsRequest {
  samples: Float32Array;
  threshold?: number;
  refMagnitude?: boolean;
  pad?: boolean;
  zeroPos?: boolean;
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
export function spectralCentroid(request: SpectralFrameRequest): Float32Array;
export function spectralCentroid(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function spectralCentroid(
  samples: Float32Array | SpectralFrameRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralCentroid(samples.samples, samples.sampleRate, samples.nFft, samples.hopLength);
  }
  return requireModule().spectralCentroid(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral contrast (librosa.feature.spectral_contrast).
 *
 * @remarks
 * Band 0 spans `[0, fmin]`, so an `fmin` below one analysis bin
 * (`sampleRate / nFft`) leaves it empty after the band trim. Row 0 is still
 * finite, but comes from the other bands' extremes rather than from itself, and
 * is neither level-invariant nor confined to the band. Keep `fmin` at or above
 * one bin width for row 0 to mean anything — at the defaults (22050 Hz, 2048)
 * one bin is 10.8 Hz, so only a small `nFft` or a tiny `fmin` reaches this.
 *
 * @returns Matrix2d of shape (nBands + 1) x nFrames.
 */
export function spectralContrast(request: SpectralContrastRequest): WasmMatrix2dResult;
export function spectralContrast(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nBands?: number,
  fmin?: number,
  quantile?: number,
): WasmMatrix2dResult;
export function spectralContrast(
  samples: Float32Array | SpectralContrastRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nBands = 6,
  fmin = 200.0,
  quantile = 0.02,
): WasmMatrix2dResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return spectralContrast(
      r.samples,
      r.sampleRate,
      r.nFft,
      r.hopLength,
      r.nBands,
      r.fmin,
      r.quantile,
    );
  }
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
export function polyFeatures(request: PolyFeaturesRequest): WasmMatrix2dResult;
export function polyFeatures(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  order?: number,
): WasmMatrix2dResult;
export function polyFeatures(
  samples: Float32Array | PolyFeaturesRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  order = 1,
): WasmMatrix2dResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return polyFeatures(r.samples, r.sampleRate, r.nFft, r.hopLength, r.order);
  }
  return requireModule().polyFeatures(samples, sampleRate, nFft, hopLength, order);
}

/**
 * Locate zero-crossing indices of a signal (librosa.zero_crossings).
 */
export function zeroCrossings(request: ZeroCrossingsRequest): Int32Array;
export function zeroCrossings(
  samples: Float32Array,
  threshold?: number,
  refMagnitude?: boolean,
  pad?: boolean,
  zeroPos?: boolean,
): Int32Array;
export function zeroCrossings(
  samples: Float32Array | ZeroCrossingsRequest,
  threshold = 1e-10,
  refMagnitude = false,
  pad = true,
  zeroPos = true,
): Int32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return zeroCrossings(r.samples, r.threshold, r.refMagnitude, r.pad, r.zeroPos);
  }
  return requireModule().zeroCrossings(samples, threshold, refMagnitude, pad, zeroPos);
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
export function spectralBandwidth(request: SpectralFrameRequest & { p?: number }): Float32Array;
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  p?: number,
): Float32Array;
export function spectralBandwidth(
  samples: Float32Array | (SpectralFrameRequest & { p?: number }),
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  p = 2,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralBandwidth(
      samples.samples,
      samples.sampleRate,
      samples.nFft,
      samples.hopLength,
      samples.p,
    );
  }
  return requireModule().spectralBandwidth(samples, sampleRate, nFft, hopLength, p);
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
export function spectralRolloff(request: SpectralRolloffRequest): Float32Array;
export function spectralRolloff(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  rollPercent?: number,
): Float32Array;
export function spectralRolloff(
  samples: Float32Array | SpectralRolloffRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralRolloff(
      samples.samples,
      samples.sampleRate,
      samples.nFft,
      samples.hopLength,
      samples.rollPercent,
    );
  }
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
export function spectralFlatness(request: SpectralFrameRequest): Float32Array;
export function spectralFlatness(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function spectralFlatness(
  samples: Float32Array | SpectralFrameRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralFlatness(samples.samples, samples.sampleRate, samples.nFft, samples.hopLength);
  }
  return requireModule().spectralFlatness(samples, sampleRate, nFft, hopLength);
}

export function spectralFlux(request: SpectralFrameRequest & { lag?: number }): Float32Array;
export function spectralFlux(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  lag?: number,
): Float32Array;
export function spectralFlux(
  samples: Float32Array | (SpectralFrameRequest & { lag?: number }),
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  lag = 1,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralFlux(
      samples.samples,
      samples.sampleRate,
      samples.nFft,
      samples.hopLength,
      samples.lag,
    );
  }
  return requireModule().spectralFlux(samples, sampleRate, nFft, hopLength, lag);
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
export function zeroCrossingRate(request: ZeroCrossingRateRequest): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array | ZeroCrossingRateRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return zeroCrossingRate(
      samples.samples,
      samples.sampleRate,
      samples.frameLength,
      samples.hopLength,
    );
  }
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
export function rmsEnergy(request: ZeroCrossingRateRequest): Float32Array;
export function rmsEnergy(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function rmsEnergy(
  samples: Float32Array | ZeroCrossingRateRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return rmsEnergy(samples.samples, samples.sampleRate, samples.frameLength, samples.hopLength);
  }
  return requireModule().rmsEnergy(samples, sampleRate, frameLength, hopLength);
}
