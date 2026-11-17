/**
 * Source decomposition and self-similarity segmentation: NMF factorisation and
 * the masks built over it, and the recurrence structure of a track.
 */

import { resolveEffectFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import type { WasmDecomposeResult, WasmHpssWithResidualResult } from './sonare.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertHpssKernels,
  assertMatrix2D,
  assertNonNegativeInteger,
  assertPositiveInteger,
  optionalBoolean,
  requestObject,
  requestOnly,
  requireBoolean,
  toInt32Array,
  toMatrix2D,
} from './validation.js';

function requireModule() {
  return getSonareModule();
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

export interface NnFilterRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  aggregate?: string;
  k?: number;
  width?: number;
}

/** Cross-similarity request. */
export interface SegmentCrossSimilarityRequest {
  /** Rows are features, cols are frames. */
  x: Matrix2D;
  /** Rows are features, cols are frames; the feature count must match `x`. */
  y: Matrix2D;
  k?: number;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}

/** Self-similarity recurrence request. */
export interface SegmentRecurrenceMatrixRequest {
  /** Rows are features, cols are frames. */
  data: Matrix2D;
  k?: number;
  width?: number;
  sym?: boolean;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}

/** Recurrence-to-lag request. */
export interface SegmentRecurrenceToLagRequest {
  /** Square: rows = cols = frames. */
  recurrence: Matrix2D;
  /** Zero-pad so negative lags stay apart: a `2n × n` result (default true, as librosa). */
  pad?: boolean;
}

/** Lag-to-recurrence request. */
export interface SegmentLagToRecurrenceRequest {
  /** Rows are lags (frames, or twice as many when padded), cols are frames. */
  lag: Matrix2D;
}

/** Boundary refinement request. */
export interface SegmentSubsegmentRequest {
  /** Rows are features, cols are frames. */
  data: Matrix2D;
  boundaries: Int32Array | ArrayLike<number>;
  nSegments?: number;
}

/** Feature-column clustering request. */
export interface SegmentAgglomerativeRequest {
  /** Rows are features, cols are frames. */
  data: Matrix2D;
  k: number;
  linkage?: 'average' | 'single' | 'complete' | 'ward';
}

/** Diagonal path enhancement request. */
export interface SegmentPathEnhanceRequest {
  /** Square: rows = cols = frames. */
  recurrence: Matrix2D;
  win: number;
  maxRatio?: number;
  minRatio?: number;
  nFilters?: number;
}

function assertSquare(fnName: string, matrix: Matrix2D, argName: string): void {
  if (matrix.rows !== matrix.cols) {
    throw new RangeError(`${fnName}: ${argName} must be square`);
  }
}

export interface RemixRequest {
  samples: Float32Array;
  intervals: Int32Array | ArrayLike<number>;
  sampleRate?: number;
  alignZeros?: boolean;
}

export interface HpssWithResidualRequest {
  samples: Float32Array;
  sampleRate?: number;
  /**
   * Horizontal median filter size, in STFT frames: a positive odd integer at
   * most 524287. Default 31. The ceiling is 524288 and an even kernel is
   * refused, so 524287 is the largest legal value.
   */
  kernelHarmonic?: number;
  /** Vertical median filter size, in STFT bins, under the same rule. Default 31. */
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
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
    requestObject('decompose', s, 's');
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
    requestObject('decomposeWithInit', s, 's');
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
  sampleRate?: number;
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
  requestObject('decomposeStems', request, 'request', true);
  assertAudioInput('decomposeStems', request.samples, request.sampleRate ?? 22050);
  return requireModule().decomposeStems(request.samples, request.sampleRate ?? 22050, {
    nComponents: request.nComponents,
    nFft: request.nFft,
    hopLength: request.hopLength,
    nIter: request.nIter,
    beta: request.beta,
    init: request.init,
    maskPower: request.maskPower,
  });
}

/** Request form of {@link decomposeStemsLinked}. */
export interface DecomposeStemsLinkedRequest {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  sampleRate?: number;
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
  /** Soft-mask exponent (default 1); see {@link DecomposeStemsRequest.maskPower}. */
  maskPower?: number;
}

/** One time-domain signal per (component, channel), plus the factorisation. */
export interface DecomposeStemsLinkedResult {
  /**
   * Component signals: `components[k][c]` is component `k`'s signal on
   * channel `c`, each the length of the input.
   */
  components: Float32Array[][];
  /** Component matrix [nBins x nComponents], row-major. */
  w: Float32Array;
  /** Activation matrix [nComponents x nFrames], row-major. */
  h: Float32Array;
  sampleRate: number;
}

/**
 * Multi-channel form of {@link decomposeStems}: one NMF model and one soft
 * mask shared across every channel, built from the channels' averaged
 * magnitude spectrogram and applied UNCHANGED to each channel's own complex
 * spectrum, so no interchannel level or phase difference moves. A single
 * channel reproduces {@link decomposeStems} bit for bit.
 *
 * @throws On a null/empty channel set, mismatched channel lengths, a channel
 *   count above the core's ceiling, or an invalid option.
 */
export function decomposeStemsLinked(
  request: DecomposeStemsLinkedRequest,
): DecomposeStemsLinkedResult {
  requestObject('decomposeStemsLinked', request, 'request', true);
  assertAudioChannels('decomposeStemsLinked', request.channels, request.sampleRate ?? 22050);
  return requireModule().decomposeStemsLinked(request.channels, request.sampleRate ?? 22050, {
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
 *
 * @returns {@link Matrix2D} of the input's shape: rows are features, cols are frames.
 */
export function nnFilter(request: NnFilterRequest): Matrix2D;
export function nnFilter(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  aggregate?: string,
  k?: number,
  width?: number,
): Matrix2D;
export function nnFilter(
  s: Float32Array | NnFilterRequest,
  nFeatures = 0,
  nFrames = 0,
  aggregate = 'mean',
  k = 7,
  width = 1,
): Matrix2D {
  if (!(s instanceof Float32Array)) {
    requestObject('nnFilter', s, 's');
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
    requestObject('remix', samples);
    const r = samples;
    return remix(r.samples, r.intervals, r.sampleRate, r.alignZeros);
  }
  assertAudioInput('remix', samples, sampleRate);
  requireBoolean('remix', alignZeros, 'alignZeros');
  // Sample indices must reach the native side as exact 32-bit integers, and a
  // boundary the conversion changed would cut the slice somewhere the caller
  // never named.
  const intervalsI32 = toInt32Array('remix', intervals as ArrayLike<number>, 'intervals');
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
    requestObject('remixAlignedIntervals', samples);
    const r = samples;
    return remixAlignedIntervals(r.samples, r.intervals, r.sampleRate, r.alignZeros ?? true);
  }
  assertAudioInput('remixAlignedIntervals', samples, sampleRate);
  requireBoolean('remixAlignedIntervals', alignZeros, 'alignZeros');
  const intervalsI32 = toInt32Array(
    'remixAlignedIntervals',
    intervals as ArrayLike<number>,
    'intervals',
  );
  return requireModule().remixAlignedIntervals(samples, intervalsI32, sampleRate, alignZeros);
}

/**
 * HPSS into harmonic / percussive / residual signals.
 *
 * The three outputs always add back up to the input. `residual` is silent under
 * the default soft mask, whose two masks sum to one, so `harmonic` and
 * `percussive` already carry everything; it is returned anyway so the result
 * shape does not change with the mask. Pass `hardMask: true` for a residual that
 * holds signal — its thresholded masks leave the band where neither component
 * dominates, measured at 3 % of the input energy on a voice-plus-kick signal.
 *
 * @example
 * ```ts
 * const soft = hpssWithResidual({ samples, sampleRate });
 * // soft.residual is silence
 * const hard = hpssWithResidual({ samples, sampleRate, hardMask: true });
 * // hard.residual carries what neither component claimed
 * ```
 *
 * @throws RangeError on a kernel that is not an integer
 *   within the signed 32-bit range, or one the core rejects as even,
 *   non-positive or above its ceiling
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
    requestObject('hpssWithResidual', samples);
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
  assertAudioInput('hpssWithResidual', samples, sampleRate);
  const fftOptions = resolveEffectFftOptions('hpssWithResidual', nFft, hopLength);
  const resolvedHardMask = optionalBoolean('hpssWithResidual', hardMask, 'hardMask', false);
  assertHpssKernels('hpssWithResidual', kernelHarmonic, kernelPercussive);
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
 * Column-wise cross-similarity (librosa.segment.cross_similarity).
 *
 * @returns {@link Matrix2D} whose rows are `y` frames and cols are `x` frames.
 */
export function segmentCrossSimilarity(request: SegmentCrossSimilarityRequest): Matrix2D;
export function segmentCrossSimilarity(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentCrossSimilarityRequest>('segmentCrossSimilarity', args);
  const x = assertMatrix2D('segmentCrossSimilarity', r.x, 'x');
  const y = assertMatrix2D('segmentCrossSimilarity', r.y, 'y');
  if (x.rows !== y.rows) {
    throw new RangeError('segmentCrossSimilarity: feature dimensions must match');
  }
  assertNonNegativeInteger('segmentCrossSimilarity', r.k ?? 0, 'k');
  const native = requireModule().segmentCrossSimilarity(
    x.data,
    x.rows,
    x.cols,
    y.data,
    y.rows,
    y.cols,
    r.k ?? 0,
    r.metric ?? 'cosine',
    r.mode ?? 'connectivity',
  );
  return toMatrix2D(native.values, native.rows, native.cols);
}

/**
 * Self-similarity recurrence matrix (librosa.segment.recurrence_matrix).
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentRecurrenceMatrix(request: SegmentRecurrenceMatrixRequest): Matrix2D;
export function segmentRecurrenceMatrix(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentRecurrenceMatrixRequest>('segmentRecurrenceMatrix', args);
  const data = assertMatrix2D('segmentRecurrenceMatrix', r.data, 'data');
  assertNonNegativeInteger('segmentRecurrenceMatrix', r.k ?? 0, 'k');
  assertNonNegativeInteger('segmentRecurrenceMatrix', r.width ?? 1, 'width');
  const native = requireModule().segmentRecurrenceMatrix(
    data.data,
    data.rows,
    data.cols,
    r.k ?? 0,
    r.width ?? 1,
    optionalBoolean('segmentRecurrenceMatrix', r.sym, 'sym', false),
    r.metric ?? 'euclidean',
    r.mode ?? 'connectivity',
  );
  return toMatrix2D(native.values, native.rows, native.cols);
}

/**
 * Convert a square recurrence matrix to a lag-by-time matrix, as librosa does.
 *
 * @returns {@link Matrix2D} whose rows are lags and cols are frames.
 */
export function segmentRecurrenceToLag(request: SegmentRecurrenceToLagRequest): Matrix2D;
export function segmentRecurrenceToLag(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentRecurrenceToLagRequest>('segmentRecurrenceToLag', args);
  const recurrence = assertMatrix2D('segmentRecurrenceToLag', r.recurrence, 'recurrence');
  assertSquare('segmentRecurrenceToLag', recurrence, 'recurrence');
  const native = requireModule().segmentRecurrenceToLag(
    recurrence.data,
    recurrence.rows,
    optionalBoolean('segmentRecurrenceToLag', r.pad, 'pad', true),
  );
  return toMatrix2D(native.values, native.rows, native.cols);
}

/**
 * Convert a lag matrix (lags = frames, or twice as many padded) to a recurrence.
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentLagToRecurrence(request: SegmentLagToRecurrenceRequest): Matrix2D;
export function segmentLagToRecurrence(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentLagToRecurrenceRequest>('segmentLagToRecurrence', args);
  const lag = assertMatrix2D('segmentLagToRecurrence', r.lag, 'lag');
  const native = requireModule().segmentLagToRecurrence(lag.data, lag.cols, lag.rows);
  return toMatrix2D(native.values, native.rows, native.cols);
}

/** Refine frame boundaries by clustering within each parent segment. */
export function segmentSubsegment(request: SegmentSubsegmentRequest): Int32Array;
export function segmentSubsegment(...args: unknown[]): Int32Array {
  const r = requestOnly<SegmentSubsegmentRequest>('segmentSubsegment', args);
  const data = assertMatrix2D('segmentSubsegment', r.data, 'data');
  assertPositiveInteger('segmentSubsegment', r.nSegments ?? 4, 'nSegments');
  return requireModule().segmentSubsegment(
    data.data,
    data.rows,
    data.cols,
    toInt32Array('segmentSubsegment', r.boundaries, 'boundaries'),
    r.nSegments ?? 4,
  );
}

/** Cluster feature columns and return one label per column. */
export function segmentAgglomerative(request: SegmentAgglomerativeRequest): Int32Array;
export function segmentAgglomerative(...args: unknown[]): Int32Array {
  const r = requestOnly<SegmentAgglomerativeRequest>('segmentAgglomerative', args);
  const data = assertMatrix2D('segmentAgglomerative', r.data, 'data');
  assertPositiveInteger('segmentAgglomerative', r.k, 'k');
  return requireModule().segmentAgglomerative(
    data.data,
    data.rows,
    data.cols,
    r.k,
    r.linkage ?? 'average',
  );
}

/**
 * Enhance diagonal paths in a square recurrence matrix.
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentPathEnhance(request: SegmentPathEnhanceRequest): Matrix2D;
export function segmentPathEnhance(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentPathEnhanceRequest>('segmentPathEnhance', args);
  const recurrence = assertMatrix2D('segmentPathEnhance', r.recurrence, 'recurrence');
  assertSquare('segmentPathEnhance', recurrence, 'recurrence');
  assertPositiveInteger('segmentPathEnhance', r.win, 'win');
  assertPositiveInteger('segmentPathEnhance', r.maxRatio ?? 2, 'maxRatio');
  assertNonNegativeInteger('segmentPathEnhance', r.minRatio ?? 0, 'minRatio');
  assertPositiveInteger('segmentPathEnhance', r.nFilters ?? 7, 'nFilters');
  const native = requireModule().segmentPathEnhance(
    recurrence.data,
    recurrence.rows,
    r.win,
    r.maxRatio ?? 2,
    r.minRatio ?? 0,
    r.nFilters ?? 7,
  );
  return toMatrix2D(native.values, native.rows, native.cols);
}
