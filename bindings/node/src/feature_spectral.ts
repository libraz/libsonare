import { resolveFftOptions } from './_fft_options.js';
import type { ValuesRequest } from './feature_units.js';
import { addon } from './native.js';
import type {
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftDbResult,
  StftResult,
} from './types.js';
import {
  assertNonNegativeScalar,
  assertPositiveInteger,
  assertSampleRate,
  assertSamples,
} from './validation.js';

/** Common input for one-shot feature extraction requests. */
export interface FeatureSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

export interface StftRequest extends FeatureSamplesRequest {
  nFft?: number;
  hopLength?: number;
}
export interface MelSpectrogramRequest extends StftRequest {
  nMels?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}

export interface MfccRequest extends MelSpectrogramRequest {
  nMfcc?: number;
  lifter?: number;
}
export interface MelDeltaRequest {
  features: Float32Array;
  nFeatures: number;
  nFrames: number;
  width?: number;
}

export interface ReassignedSpectrogramRequest extends StftRequest {
  refPower?: number;
  fillNan?: boolean;
}

export interface SpectralContrastRequest extends StftRequest {
  nBands?: number;
  fmin?: number;
  quantile?: number;
}
export interface PolyFeaturesRequest extends StftRequest {
  order?: number;
}

export interface ZeroCrossingsRequest {
  samples: Float32Array;
  threshold?: number;
  refMagnitude?: boolean;
  pad?: boolean;
  zeroPos?: boolean;
}

export function stft(request: StftRequest): StftResult;
export function stft(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): StftResult;
export function stft(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): StftResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('stft', resolvedSampleRate);
  const fft = resolveFftOptions('stft', request.nFft, request.hopLength);
  return addon.stft(request.samples, resolvedSampleRate, fft.nFft, fft.hopLength);
}

export function stftDb(request: StftRequest): StftDbResult;
export function stftDb(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): StftDbResult;
export function stftDb(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): StftDbResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('stftDb', resolvedSampleRate);
  const fft = resolveFftOptions('stftDb', request.nFft, request.hopLength);
  return addon.stftDb(request.samples, resolvedSampleRate, fft.nFft, fft.hopLength);
}

export function melSpectrogram(request: MelSpectrogramRequest): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array | MelSpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  fmin = 0,
  fmax = 0,
  htk = false,
): MelSpectrogramResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, nMels, fmin, fmax, htk }
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('melSpectrogram', resolvedSampleRate);
  const fft = resolveFftOptions('melSpectrogram', request.nFft, request.hopLength);
  assertPositiveInteger('melSpectrogram', request.nMels ?? 128, 'nMels');
  return addon.melSpectrogram(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nMels ?? 128,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.htk ?? false,
  );
}

export function mfcc(request: MfccRequest): MfccResult;
export function mfcc(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  nMfcc?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
  lifter?: number,
): MfccResult;
export function mfcc(
  samples: Float32Array | MfccRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 20,
  fmin = 0,
  fmax = 0,
  htk = false,
  lifter = 0,
): MfccResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk, lifter }
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('mfcc', resolvedSampleRate);
  const fft = resolveFftOptions('mfcc', request.nFft, request.hopLength);
  assertPositiveInteger('mfcc', request.nMels ?? 128, 'nMels');
  assertPositiveInteger('mfcc', request.nMfcc ?? 20, 'nMfcc');
  return addon.mfcc(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nMels ?? 128,
    request.nMfcc ?? 20,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.htk ?? false,
    request.lifter ?? 0,
  );
}

/** First-order regression delta of a row-major feature matrix. */
export function melDelta(request: MelDeltaRequest): Float32Array;
export function melDelta(
  features: Float32Array,
  nFeatures: number,
  nFrames: number,
  width?: number,
): Float32Array;
export function melDelta(
  features: Float32Array | MelDeltaRequest,
  nFeatures?: number,
  nFrames?: number,
  width = 9,
): Float32Array {
  const request: MelDeltaRequest =
    features instanceof Float32Array
      ? { features, nFeatures: nFeatures ?? 0, nFrames: nFrames ?? 0, width }
      : features;
  assertPositiveInteger('melDelta', request.nFeatures, 'nFeatures');
  assertPositiveInteger('melDelta', request.nFrames, 'nFrames');
  assertPositiveInteger('melDelta', request.width ?? 9, 'width');
  if ((request.width ?? 9) < 3 || (request.width ?? 9) % 2 === 0) {
    throw new RangeError('melDelta: width must be an odd integer of at least 3');
  }
  if (request.features.length !== request.nFeatures * request.nFrames) {
    throw new TypeError('melDelta: feature matrix length must equal nFeatures * nFrames');
  }
  return addon.melDelta(request.features, request.nFeatures, request.nFrames, request.width ?? 9);
}

/** Auger-Flandrin reassigned spectrogram (row-major [nBins x nFrames] arrays). */
export function reassignedSpectrogram(
  request: ReassignedSpectrogramRequest,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  refPower?: number,
  fillNan?: boolean,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array | ReassignedSpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  refPower = 1e-6,
  fillNan = false,
): ReassignedSpectrogramResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, refPower, fillNan }
      : samples;
  assertSamples('reassignedSpectrogram', request.samples, true);
  const resolvedRefPower = request.refPower ?? 1e-6;
  assertNonNegativeScalar('reassignedSpectrogram', resolvedRefPower, 'refPower');
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('reassignedSpectrogram', resolvedSampleRate);
  const fft = resolveFftOptions('reassignedSpectrogram', request.nFft, request.hopLength);
  return addon.reassignedSpectrogram(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    resolvedRefPower,
    request.fillNan ?? false,
  );
}

export function spectralCentroid(request: StftRequest): Float32Array;
export function spectralCentroid(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function spectralCentroid(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralCentroid', resolvedSampleRate);
  const fft = resolveFftOptions('spectralCentroid', request.nFft, request.hopLength);
  return addon.spectralCentroid(request.samples, resolvedSampleRate, fft.nFft, fft.hopLength);
}

/**
 * Spectral contrast (librosa.feature.spectral_contrast); (nBands+1) x nFrames.
 *
 * @remarks
 * Band 0 spans `[0, fmin]`, so an `fmin` below one analysis bin
 * (`sampleRate / nFft`) leaves it empty after the band trim. Row 0 is still
 * finite, but comes from the other bands' extremes rather than from itself, and
 * is neither level-invariant nor confined to the band. Keep `fmin` at or above
 * one bin width for row 0 to mean anything — at the defaults (22050 Hz, 2048)
 * one bin is 10.8 Hz, so only a small `nFft` or a tiny `fmin` reaches this.
 */
export function spectralContrast(request: SpectralContrastRequest): Matrix2D;
export function spectralContrast(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nBands?: number,
  fmin?: number,
  quantile?: number,
): Matrix2D;
export function spectralContrast(
  samples: Float32Array | SpectralContrastRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nBands = 6,
  fmin = 200.0,
  quantile = 0.02,
): Matrix2D {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, nBands, fmin, quantile }
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralContrast', resolvedSampleRate);
  const fft = resolveFftOptions('spectralContrast', request.nFft, request.hopLength);
  assertPositiveInteger('spectralContrast', request.nBands ?? 6, 'nBands');
  return addon.spectralContrast(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nBands ?? 6,
    request.fmin ?? 200,
    request.quantile ?? 0.02,
  );
}

/** Per-frame polynomial coefficients (librosa.feature.poly_features); (order+1) x nFrames. */
export function polyFeatures(request: PolyFeaturesRequest): Matrix2D;
export function polyFeatures(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  order?: number,
): Matrix2D;
export function polyFeatures(
  samples: Float32Array | PolyFeaturesRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  order = 1,
): Matrix2D {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength, order } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('polyFeatures', resolvedSampleRate);
  const fft = resolveFftOptions('polyFeatures', request.nFft, request.hopLength);
  return addon.polyFeatures(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.order ?? 1,
  );
}

/** Zero-crossing indices of a signal (librosa.zero_crossings). */
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
  const request =
    samples instanceof Float32Array ? { samples, threshold, refMagnitude, pad, zeroPos } : samples;
  return addon.zeroCrossings(
    request.samples,
    request.threshold ?? 1e-10,
    request.refMagnitude ?? false,
    request.pad ?? true,
    request.zeroPos ?? true,
  );
}

export function spectralBandwidth(request: StftRequest & { p?: number }): Float32Array;
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  p?: number,
): Float32Array;
export function spectralBandwidth(
  samples: Float32Array | (StftRequest & { p?: number }),
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  p = 2,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength, p } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralBandwidth', resolvedSampleRate);
  const fft = resolveFftOptions('spectralBandwidth', request.nFft, request.hopLength);
  return addon.spectralBandwidth(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.p ?? 2,
  );
}

export function spectralRolloff(request: StftRequest & { rollPercent?: number }): Float32Array;
export function spectralRolloff(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  rollPercent?: number,
): Float32Array;
export function spectralRolloff(
  samples: Float32Array | (StftRequest & { rollPercent?: number }),
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, rollPercent }
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralRolloff', resolvedSampleRate);
  const fft = resolveFftOptions('spectralRolloff', request.nFft, request.hopLength);
  return addon.spectralRolloff(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.rollPercent ?? 0.85,
  );
}

export function spectralFlatness(request: StftRequest): Float32Array;
export function spectralFlatness(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function spectralFlatness(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralFlatness', resolvedSampleRate);
  const fft = resolveFftOptions('spectralFlatness', request.nFft, request.hopLength);
  return addon.spectralFlatness(request.samples, resolvedSampleRate, fft.nFft, fft.hopLength);
}

export function spectralFlux(request: StftRequest & { lag?: number }): Float32Array;
export function spectralFlux(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  lag?: number,
): Float32Array;
export function spectralFlux(
  samples: Float32Array | (StftRequest & { lag?: number }),
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  lag = 1,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength, lag } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('spectralFlux', resolvedSampleRate);
  const fft = resolveFftOptions('spectralFlux', request.nFft, request.hopLength);
  assertPositiveInteger('spectralFlux', request.lag ?? 1, 'lag');
  return addon.spectralFlux(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.lag ?? 1,
  );
}

export function zeroCrossingRate(
  request: FeatureSamplesRequest & { frameLength?: number; hopLength?: number },
): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array | (FeatureSamplesRequest & { frameLength?: number; hopLength?: number }),
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, frameLength, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('zeroCrossingRate', resolvedSampleRate);
  // A framing window is not a transform size, so the evenness rule the FFT
  // resolver carries does not apply to it.
  assertPositiveInteger('zeroCrossingRate', request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger('zeroCrossingRate', request.hopLength ?? 512, 'hopLength');
  return addon.zeroCrossingRate(
    request.samples,
    resolvedSampleRate,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

export function rmsEnergy(
  request: FeatureSamplesRequest & { frameLength?: number; hopLength?: number },
): Float32Array;
export function rmsEnergy(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function rmsEnergy(
  samples: Float32Array | (FeatureSamplesRequest & { frameLength?: number; hopLength?: number }),
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, frameLength, hopLength } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('rmsEnergy', resolvedSampleRate);
  assertPositiveInteger('rmsEnergy', request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger('rmsEnergy', request.hopLength ?? 512, 'hopLength');
  return addon.rmsEnergy(
    request.samples,
    resolvedSampleRate,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

/**
 * Tuning parameters for {@link pcen} (per-channel energy normalization). All
 * fields are optional; omitted keys fall back to librosa-compatible defaults.
 */
export interface PcenOptions {
  /** Sample rate used to derive the smoothing time constant (default 22050). */
  sampleRate?: number;
  /** Hop length used to derive the smoothing time constant (default 512). */
  hopLength?: number;
  /** Smoothing filter time constant in seconds (default 0.4). */
  timeConstant?: number;
  /** Gain exponent applied to the smoothed energy (default 0.98). */
  gain?: number;
  /** Bias added before the power compression (default 2.0). */
  bias?: number;
  /** Power exponent of the final compression (default 0.5). */
  power?: number;
  /** Numerical floor to avoid division by zero (default 1e-6). */
  eps?: number;
}

export function pcen(
  request: ValuesRequest & { nBins: number; nFrames: number } & PcenOptions,
): Float32Array;
export function pcen(
  values: Float32Array,
  nBins?: number,
  nFrames?: number,
  options?: PcenOptions,
): Float32Array;
export function pcen(
  values: Float32Array | (ValuesRequest & { nBins: number; nFrames: number } & PcenOptions),
  nBins = 0,
  nFrames = 0,
  options: PcenOptions = {},
): Float32Array {
  const request = values instanceof Float32Array ? { values, nBins, nFrames, ...options } : values;
  const {
    values: requestValues,
    nBins: requestBins,
    nFrames: requestFrames,
    ...requestOptions
  } = request;
  // The addon's matrix-dimension rule runs on the NARROWED values, so a
  // fractional pair that divides the buffer once truncated passes it: 2.5 x 50
  // over 100 values is checked as 2 x 50 and answers as a 2-bin result.
  assertPositiveInteger('pcen', requestBins, 'nBins');
  assertPositiveInteger('pcen', requestFrames, 'nFrames');
  // Only what the caller supplied: an omitted field's default belongs to the
  // addon's options reader, so resolving one here would check this file's guess.
  if (requestOptions.sampleRate !== undefined) {
    assertSampleRate('pcen', requestOptions.sampleRate);
  }
  if (requestOptions.hopLength !== undefined) {
    assertPositiveInteger('pcen', requestOptions.hopLength, 'hopLength');
  }
  return addon.pcen(requestValues, requestBins, requestFrames, requestOptions);
}
