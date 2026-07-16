import { getSonareModule } from './module_state';
import type {
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmLufsResult,
  WasmMatrix2dResult,
} from './sonare.js';
import type { ValidateOptions } from './validation';
import { assertInterleavedSamples, assertSampleRate } from './validation';

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

export interface DecomposeRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  nComponents: number;
  nIter?: number;
  beta?: number;
}

export interface DecomposeWithInitRequest extends DecomposeRequest {
  init?: 'random' | 'nndsvd';
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
export interface PitchTuningRequest {
  frequencies: Float32Array;
  resolution?: number;
  binsPerOctave?: number;
}
export interface EstimateTuningRequest extends SpectralFrameRequest {
  resolution?: number;
  binsPerOctave?: number;
}
export interface NnFilterRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  aggregate?: string;
  k?: number;
  width?: number;
}
export interface RemixRequest {
  samples: Float32Array;
  intervals: Int32Array | ArrayLike<number>;
  sampleRate?: number;
  alignZeros?: boolean;
}
export interface PhaseVocoderRequest extends SpectralFrameRequest {
  rate: number;
}
export interface HpssWithResidualRequest {
  samples: Float32Array;
  sampleRate?: number;
  kernelHarmonic?: number;
  kernelPercussive?: number;
}
export interface LufsInterleavedRequest extends ValidateOptions {
  samples: Float32Array;
  channels: number;
  sampleRate?: number;
}
export interface Ebur128LoudnessRangeRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
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
 * Estimate the global tuning offset from a set of frequencies
 * (librosa.pitch_tuning). Returns a deviation in fractions of a bin.
 */
export function pitchTuning(request: PitchTuningRequest): number;
export function pitchTuning(
  frequencies: Float32Array,
  resolution?: number,
  binsPerOctave?: number,
): number;
export function pitchTuning(
  frequencies: Float32Array | PitchTuningRequest,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  if (!(frequencies instanceof Float32Array)) {
    const r = frequencies;
    return pitchTuning(r.frequencies, r.resolution, r.binsPerOctave);
  }
  return requireModule().pitchTuning(frequencies, resolution, binsPerOctave);
}

/**
 * Estimate the tuning offset of an audio signal (librosa.estimate_tuning).
 */
export function estimateTuning(request: EstimateTuningRequest): number;
export function estimateTuning(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  resolution?: number,
  binsPerOctave?: number,
): number;
export function estimateTuning(
  samples: Float32Array | EstimateTuningRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return estimateTuning(
      r.samples,
      r.sampleRate,
      r.nFft,
      r.hopLength,
      r.resolution,
      r.binsPerOctave,
    );
  }
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
export function decompose(request: DecomposeRequest): WasmDecomposeResult;
export function decompose(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  nComponents: number,
  nIter?: number,
  beta?: number,
): WasmDecomposeResult;
export function decompose(
  s: Float32Array | DecomposeRequest,
  nFeatures = 0,
  nFrames = 0,
  nComponents = 0,
  nIter = 50,
  beta = 2.0,
): WasmDecomposeResult {
  if (!(s instanceof Float32Array)) {
    const request = s;
    return decompose(
      request.s,
      request.nFeatures,
      request.nFrames,
      request.nComponents,
      request.nIter,
      request.beta,
    );
  }
  return requireModule().decompose(s, nFeatures, nFrames, nComponents, nIter, beta);
}

/**
 * Non-negative matrix factorisation with a selectable initialiser
 * (librosa.decompose.decompose, `init`). Identical to {@link decompose} but
 * exposes the initialisation strategy: `'random'` (default, deterministic seed)
 * or `'nndsvd'` (SVD-based warm start, which tends to converge in fewer
 * iterations). Returns the W and H factors.
 */
export function decomposeWithInit(request: DecomposeWithInitRequest): WasmDecomposeResult;
export function decomposeWithInit(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  nComponents: number,
  nIter?: number,
  beta?: number,
  init?: 'random' | 'nndsvd',
): WasmDecomposeResult;
export function decomposeWithInit(
  s: Float32Array | DecomposeWithInitRequest,
  nFeatures = 0,
  nFrames = 0,
  nComponents = 0,
  nIter = 50,
  beta = 2.0,
  init: 'random' | 'nndsvd' = 'random',
): WasmDecomposeResult {
  if (!(s instanceof Float32Array)) {
    const request = s;
    return decomposeWithInit(
      request.s,
      request.nFeatures,
      request.nFrames,
      request.nComponents,
      request.nIter,
      request.beta,
      request.init,
    );
  }
  return requireModule().decomposeWithInit(s, nFeatures, nFrames, nComponents, nIter, beta, init);
}

/**
 * Nearest-neighbour filtering of a flattened [nFeatures x nFrames] spectrogram
 * (librosa.decompose.nn_filter).
 */
export function nnFilter(request: NnFilterRequest): WasmMatrix2dResult;
export function nnFilter(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  aggregate?: string,
  k?: number,
  width?: number,
): WasmMatrix2dResult;
export function nnFilter(
  s: Float32Array | NnFilterRequest,
  nFeatures = 0,
  nFrames = 0,
  aggregate = 'mean',
  k = 7,
  width = 1,
): WasmMatrix2dResult {
  if (!(s instanceof Float32Array)) {
    const r = s;
    return nnFilter(r.s, r.nFeatures, r.nFrames, r.aggregate, r.k, r.width);
  }
  return requireModule().nnFilter(s, nFeatures, nFrames, aggregate, k, width);
}

/**
 * Reorder/concatenate a signal by interval slices (librosa.effects.remix).
 *
 * @param intervals - Flat (start, end) sample pairs (even length).
 */
export function remix(request: RemixRequest): Float32Array;
export function remix(
  samples: Float32Array,
  intervals: Int32Array | ArrayLike<number>,
  sampleRate?: number,
  alignZeros?: boolean,
): Float32Array;
export function remix(
  samples: Float32Array | RemixRequest,
  intervals?: Int32Array | ArrayLike<number>,
  sampleRate = 22050,
  alignZeros = false,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return remix(r.samples, r.intervals, r.sampleRate, r.alignZeros);
  }
  // Sample indices must reach the native side as exact 32-bit integers. Passing
  // a Float32Array (or a number[] holding fractional/large values) would round
  // boundaries above 2^24 and misalign the slice. Coerce to an Int32Array,
  // truncating toward zero, so callers can hand us any numeric array safely.
  const intervalsI32 =
    intervals instanceof Int32Array
      ? intervals
      : Int32Array.from(intervals as ArrayLike<number>, (v) => Math.trunc(v));
  return requireModule().remix(samples, intervalsI32, sampleRate, alignZeros);
}

/**
 * Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower).
 */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array,
  rate: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function phaseVocoder(
  samples: Float32Array | PhaseVocoderRequest,
  rate = 1,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return phaseVocoder(r.samples, r.rate, r.sampleRate, r.nFft, r.hopLength);
  }
  return requireModule().phaseVocoder(samples, sampleRate, rate, nFft, hopLength);
}

/**
 * HPSS into harmonic / percussive / residual signals.
 */
export function hpssWithResidual(request: HpssWithResidualRequest): WasmHpssWithResidualResult;
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
): WasmHpssWithResidualResult;
export function hpssWithResidual(
  samples: Float32Array | HpssWithResidualRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): WasmHpssWithResidualResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return hpssWithResidual(r.samples, r.sampleRate, r.kernelHarmonic, r.kernelPercussive);
  }
  return requireModule().hpssWithResidual(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Channel-weighted multichannel integrated loudness + LRA (ITU-R BS.1770 /
 * EBU R128) from an interleaved buffer of `frames * channels` samples. The
 * per-channel frame count is derived from the buffer length and `channels`.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 */
export function lufsInterleaved(request: LufsInterleavedRequest): WasmLufsResult;
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
  options?: ValidateOptions,
): WasmLufsResult;
export function lufsInterleaved(
  samples: Float32Array | LufsInterleavedRequest,
  channels = 0,
  sampleRate = 22050,
  options: ValidateOptions = {},
): WasmLufsResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return lufsInterleaved(r.samples, r.channels, r.sampleRate, r);
  }
  assertSampleRate('lufsInterleaved', sampleRate);
  assertInterleavedSamples('lufsInterleaved', samples, channels, options.validate !== false);
  return requireModule().lufsInterleaved(samples, channels, sampleRate);
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard and K-weighting is
 * sample-rate dependent.
 */
export function ebur128LoudnessRange(request: Ebur128LoudnessRangeRequest): number;
export function ebur128LoudnessRange(samples: Float32Array, sampleRate?: number): number;
export function ebur128LoudnessRange(
  samples: Float32Array | Ebur128LoudnessRangeRequest,
  sampleRate = 22050,
): number {
  if (!(samples instanceof Float32Array)) {
    return ebur128LoudnessRange(samples.samples, samples.sampleRate);
  }
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
export function spectralBandwidth(request: SpectralFrameRequest): Float32Array;
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function spectralBandwidth(
  samples: Float32Array | SpectralFrameRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return spectralBandwidth(samples.samples, samples.sampleRate, samples.nFft, samples.hopLength);
  }
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
