/**
 * Source decomposition and self-similarity segmentation: NMF factorisation and
 * the masks built over it, and the recurrence structure of a track.
 */

import { resolveFftOptions } from './_fft_options';
import { getSonareModule } from './module_state';
import type { SegmentMatrix } from './public_types';
import type {
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmMatrix2dResult,
} from './sonare.js';
import {
  assertHpssKernels,
  assertNonNegativeInteger,
  assertPositiveInteger,
  assertSamples,
  toInt32Array,
} from './validation';

function requireModule() {
  return getSonareModule();
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
    const r = samples;
    return remixAlignedIntervals(r.samples, r.intervals, r.sampleRate, r.alignZeros ?? true);
  }
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
 * @throws SonareError (`InvalidParameter`) on a kernel that is not an integer
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
  const fftOptions = resolveFftOptions('hpssWithResidual', nFft, hopLength);
  const resolvedHardMask = resolveHardMask('hpssWithResidual', hardMask);
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
