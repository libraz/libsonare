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
import { assertNonNegativeScalar, assertSamples } from './validation.js';

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
  return addon.stft(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
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
  return addon.stftDb(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
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
  return addon.melSpectrogram(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.mfcc(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  if (
    !Number.isInteger(request.nFeatures) ||
    !Number.isInteger(request.nFrames) ||
    request.nFeatures <= 0 ||
    request.nFrames <= 0 ||
    request.features.length !== request.nFeatures * request.nFrames
  ) {
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
  return addon.reassignedSpectrogram(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.spectralCentroid(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
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
  return addon.spectralContrast(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.polyFeatures(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.spectralBandwidth(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.spectralRolloff(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.spectralFlatness(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
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
  return addon.spectralFlux(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.zeroCrossingRate(
    request.samples,
    request.sampleRate ?? 22050,
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
  return addon.rmsEnergy(
    request.samples,
    request.sampleRate ?? 22050,
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
  return addon.pcen(requestValues, requestBins, requestFrames, requestOptions);
}
