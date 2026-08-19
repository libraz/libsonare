import { getSonareModule } from './module_state';
import type { SegmentMatrix } from './public_types';
import type {
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmLufsResult,
  WasmMatrix2dResult,
} from './sonare.js';
import type { ValidateOptions } from './validation';
import {
  assertInterleavedSamples,
  assertNonNegativeInteger,
  assertPositiveInteger,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

function resolveEffectFftOptions(
  fnName: string,
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  const resolvedNFft = nFft === undefined ? 2048 : nFft;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  if (typeof resolvedNFft !== 'number' || !Number.isInteger(resolvedNFft)) {
    throw new TypeError(`${fnName}: nFft must be an integer`);
  }
  if (resolvedNFft < 2 || resolvedNFft > 2 ** 30) {
    throw new RangeError(`${fnName}: nFft must be an even power of two >= 2`);
  }
  if ((resolvedNFft & (resolvedNFft - 1)) !== 0) {
    throw new RangeError(`${fnName}: nFft must be an even power of two >= 2`);
  }
  if (typeof resolvedHopLength !== 'number' || !Number.isInteger(resolvedHopLength)) {
    throw new TypeError(`${fnName}: hopLength must be an integer`);
  }
  if (resolvedHopLength <= 0 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: hopLength must be a positive integer`);
  }
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}

function resolveHardMask(fnName: string, value: unknown): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
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
export interface SegmentCrossSimilarityRequest {
  x: Float32Array;
  xRows: number;
  xCols: number;
  y: Float32Array;
  yRows: number;
  yCols: number;
  k?: number;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}
export interface SegmentRecurrenceMatrixRequest {
  data: Float32Array;
  rows: number;
  cols: number;
  k?: number;
  width?: number;
  sym?: boolean;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}
export interface SegmentRecurrenceToLagRequest {
  recurrence: Float32Array;
  n: number;
  pad?: boolean;
}
export interface SegmentLagToRecurrenceRequest {
  lag: Float32Array;
  rows: number;
  lags: number;
}
export interface SegmentSubsegmentRequest {
  data: Float32Array;
  rows: number;
  cols: number;
  boundaries: Int32Array;
  nSegments?: number;
}
export interface SegmentAgglomerativeRequest {
  data: Float32Array;
  rows: number;
  cols: number;
  k: number;
  linkage?: 'average' | 'single' | 'complete' | 'ward';
}
export interface SegmentPathEnhanceRequest {
  recurrence: Float32Array;
  n: number;
  win: number;
  maxRatio?: number;
  minRatio?: number;
  nFilters?: number;
}

function validateSegmentMatrix(
  fnName: string,
  data: Float32Array,
  rows: number,
  cols: number,
  dataName: string,
): void {
  assertPositiveInteger(fnName, rows, 'rows');
  assertPositiveInteger(fnName, cols, 'cols');
  assertSamples(fnName, data, true, dataName);
  const expected = rows * cols;
  if (!Number.isSafeInteger(expected) || data.length !== expected) {
    throw new RangeError(`${fnName}: ${dataName} length must equal rows * cols`);
  }
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
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
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

/** Options for {@link decomposeStems}. */
export interface DecomposeStemsRequest {
  samples: Float32Array;
  sampleRate: number;
  /** Number of NMF components (default 4). */
  nComponents?: number;
  /** STFT size (default 2048). */
  nFft?: number;
  /** STFT hop (default 512). */
  hopLength?: number;
  /** NMF multiplicative-update iterations (default 100). */
  nIter?: number;
  /** Beta divergence: 2 = Frobenius (default), 1 = Kullback-Leibler. */
  beta?: number;
  /** NMF initialisation (default `'random'`). */
  init?: 'random' | 'nndsvd';
  /**
   * Soft-mask exponent (default 1). 1 keeps the magnitude ratio; 2 is the
   * Wiener-style power ratio, which separates harder at the cost of more
   * artefacts on overlapping partials. Must be >= 1.
   */
  maskPower?: number;
}

/** One time-domain signal per NMF component, plus the factorisation. */
export interface DecomposeStemsResult {
  /** Component signals, each the length of the input. */
  components: Float32Array[];
  /** Component matrix [nBins x nComponents], row-major. */
  w: Float32Array;
  /** Activation matrix [nComponents x nFrames], row-major. */
  h: Float32Array;
  sampleRate: number;
}

/**
 * NMF separation that **carries the original phase**, so each component is
 * directly listenable.
 *
 * {@link decompose} returns the W/H factors of a magnitude spectrogram, which
 * have no phase; reconstructing from them needs a phase estimator
 * ({@link griffinLim}), and an estimated phase does not hold up as a stem. This
 * instead builds a per-component soft mask from the factorisation and applies
 * it to the original complex spectrogram. The masks sum to one wherever the
 * model has energy and the inverse STFT is linear, so the components sum back
 * to the input.
 */
export function decomposeStems(request: DecomposeStemsRequest): DecomposeStemsResult {
  return requireModule().decomposeStems(request.samples, request.sampleRate, {
    nComponents: request.nComponents,
    nFft: request.nFft,
    hopLength: request.hopLength,
    nIter: request.nIter,
    beta: request.beta,
    init: request.init,
    maskPower: request.maskPower,
  });
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
 * With `alignZeros` the boundaries snap to the signal's zero-crossings. That is
 * a per-signal decision, so calling this per channel snaps each channel to a
 * different frame and drifts a stereo take apart; resolve one cut set with
 * {@link remixAlignedIntervals} and apply it to every channel instead.
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
 * Resolve the cut points {@link remix} would use, without cutting.
 *
 * Returns a flat `Int32Array` of one clamped `(start, end)` pair per input
 * interval. With `alignZeros` each boundary snaps to the nearest zero-crossing,
 * with two guards that stop a slice from vanishing: a signal with no sign
 * change at all (silence, a DC offset, any constant) is not snapped, and a
 * slice that had content but collapses to empty after snapping keeps its
 * unsnapped boundaries.
 *
 * Use this to cut a multichannel take on one common frame set: resolve once
 * from one channel, then slice every channel with the returned pairs.
 *
 * @param intervals - Flat (start, end) sample pairs (even length).
 */
export function remixAlignedIntervals(request: RemixRequest): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array,
  intervals: Int32Array | ArrayLike<number>,
  sampleRate?: number,
  alignZeros?: boolean,
): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array | RemixRequest,
  intervals?: Int32Array | ArrayLike<number>,
  sampleRate = 22050,
  alignZeros = true,
): Int32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return remixAlignedIntervals(r.samples, r.intervals, r.sampleRate, r.alignZeros ?? true);
  }
  const intervalsI32 =
    intervals instanceof Int32Array
      ? intervals
      : Int32Array.from(intervals as ArrayLike<number>, (v) => Math.trunc(v));
  return requireModule().remixAlignedIntervals(samples, intervalsI32, sampleRate, alignZeros);
}

/**
 * Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower).
 */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function phaseVocoder(
  samples: Float32Array | PhaseVocoderRequest,
  sampleRate = 22050,
  rate = 1,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return phaseVocoder(r.samples, r.sampleRate ?? 22050, r.rate, r.nFft, r.hopLength);
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
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): WasmHpssWithResidualResult;
export function hpssWithResidual(
  samples: Float32Array | HpssWithResidualRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): WasmHpssWithResidualResult {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return hpssWithResidual(
      r.samples,
      r.sampleRate,
      r.kernelHarmonic,
      r.kernelPercussive,
      r.nFft,
      r.hopLength,
      r.hardMask,
    );
  }
  const fftOptions = resolveEffectFftOptions('hpssWithResidual', nFft, hopLength);
  const resolvedHardMask = resolveHardMask('hpssWithResidual', hardMask);
  return requireModule().hpssWithResidualEx(
    samples,
    sampleRate,
    kernelHarmonic,
    kernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
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

/** Column-wise cross-similarity (librosa.segment.cross_similarity). */
export function segmentCrossSimilarity(request: SegmentCrossSimilarityRequest): SegmentMatrix {
  validateSegmentMatrix('segmentCrossSimilarity', request.x, request.xRows, request.xCols, 'x');
  validateSegmentMatrix('segmentCrossSimilarity', request.y, request.yRows, request.yCols, 'y');
  if (request.xRows !== request.yRows) {
    throw new RangeError('segmentCrossSimilarity: feature dimensions must match');
  }
  assertNonNegativeInteger('segmentCrossSimilarity', request.k ?? 0, 'k');
  return requireModule().segmentCrossSimilarity(
    request.x,
    request.xRows,
    request.xCols,
    request.y,
    request.yRows,
    request.yCols,
    request.k ?? 0,
    request.metric ?? 'cosine',
    request.mode ?? 'connectivity',
  );
}

/** Self-similarity recurrence matrix (librosa.segment.recurrence_matrix). */
export function segmentRecurrenceMatrix(request: SegmentRecurrenceMatrixRequest): SegmentMatrix {
  validateSegmentMatrix(
    'segmentRecurrenceMatrix',
    request.data,
    request.rows,
    request.cols,
    'data',
  );
  assertNonNegativeInteger('segmentRecurrenceMatrix', request.k ?? 0, 'k');
  assertNonNegativeInteger('segmentRecurrenceMatrix', request.width ?? 1, 'width');
  return requireModule().segmentRecurrenceMatrix(
    request.data,
    request.rows,
    request.cols,
    request.k ?? 0,
    request.width ?? 1,
    request.sym ?? false,
    request.metric ?? 'euclidean',
    request.mode ?? 'connectivity',
  );
}

/** Convert an `n × n` recurrence matrix to a lag matrix. */
export function segmentRecurrenceToLag(request: SegmentRecurrenceToLagRequest): SegmentMatrix {
  validateSegmentMatrix(
    'segmentRecurrenceToLag',
    request.recurrence,
    request.n,
    request.n,
    'recurrence',
  );
  return requireModule().segmentRecurrenceToLag(
    request.recurrence,
    request.n,
    request.pad ?? false,
  );
}

/** Convert a lag matrix back to an `n × n` recurrence matrix. */
export function segmentLagToRecurrence(request: SegmentLagToRecurrenceRequest): SegmentMatrix {
  validateSegmentMatrix('segmentLagToRecurrence', request.lag, request.rows, request.lags, 'lag');
  return requireModule().segmentLagToRecurrence(request.lag, request.rows, request.lags);
}

/** Refine frame boundaries by clustering within each parent segment. */
export function segmentSubsegment(request: SegmentSubsegmentRequest): Int32Array {
  validateSegmentMatrix('segmentSubsegment', request.data, request.rows, request.cols, 'data');
  assertPositiveInteger('segmentSubsegment', request.nSegments ?? 4, 'nSegments');
  return requireModule().segmentSubsegment(
    request.data,
    request.rows,
    request.cols,
    request.boundaries,
    request.nSegments ?? 4,
  );
}

/** Cluster feature columns and return one label per column. */
export function segmentAgglomerative(request: SegmentAgglomerativeRequest): Int32Array {
  validateSegmentMatrix('segmentAgglomerative', request.data, request.rows, request.cols, 'data');
  assertPositiveInteger('segmentAgglomerative', request.k, 'k');
  return requireModule().segmentAgglomerative(
    request.data,
    request.rows,
    request.cols,
    request.k,
    request.linkage ?? 'average',
  );
}

/** Enhance diagonal paths in an `n × n` recurrence matrix. */
export function segmentPathEnhance(request: SegmentPathEnhanceRequest): SegmentMatrix {
  validateSegmentMatrix(
    'segmentPathEnhance',
    request.recurrence,
    request.n,
    request.n,
    'recurrence',
  );
  assertPositiveInteger('segmentPathEnhance', request.win, 'win');
  assertPositiveInteger('segmentPathEnhance', request.maxRatio ?? 2, 'maxRatio');
  assertNonNegativeInteger('segmentPathEnhance', request.minRatio ?? 0, 'minRatio');
  assertPositiveInteger('segmentPathEnhance', request.nFilters ?? 7, 'nFilters');
  return requireModule().segmentPathEnhance(
    request.recurrence,
    request.n,
    request.win,
    request.maxRatio ?? 2,
    request.minRatio ?? 0,
    request.nFilters ?? 7,
  );
}
