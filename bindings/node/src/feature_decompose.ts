import { resolvePositiveIntegerOption } from './_feature_options.js';
import { resolveFftOptions } from './_fft_options.js';
import type { FeatureSamplesRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { Matrix2D } from './types.js';
import { assertHpssKernels, assertInt32, assertSampleRate, assertSamples } from './validation.js';

function resolveHardMaskOption(fnName: string, value: unknown): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

/** Row-major matrix returned by segmentation APIs. */
export interface SegmentMatrix {
  rows: number;
  cols: number;
  values: Float32Array;
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
  boundaries: Int32Array | number[];
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

export interface DecomposeRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  nComponents: number;
  nIter?: number;
  beta?: number;
  init?: 'random' | 'nndsvd';
}
export interface DecomposeStemsRequest extends FeatureSamplesRequest {
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
export interface NnFilterRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  aggregate?: string;
  k?: number;
  width?: number;
}

export interface HpssWithResidualRequest extends FeatureSamplesRequest {
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

/** Column-wise cross-similarity matrix (librosa.segment.cross_similarity). */
export function segmentCrossSimilarity(request: SegmentCrossSimilarityRequest): SegmentMatrix {
  const { x, xRows, xCols, y, yRows, yCols } = request;
  if (x.length !== xRows * xCols || y.length !== yRows * yCols || xRows !== yRows) {
    throw new RangeError('segmentCrossSimilarity: invalid matrix dimensions');
  }
  return addon.segmentCrossSimilarity(
    x,
    xRows,
    xCols,
    y,
    yRows,
    yCols,
    request.k ?? 0,
    request.metric ?? 'cosine',
    request.mode ?? 'connectivity',
  );
}

/** Self-similarity recurrence matrix (librosa.segment.recurrence_matrix). */
export function segmentRecurrenceMatrix(request: SegmentRecurrenceMatrixRequest): SegmentMatrix {
  if (request.data.length !== request.rows * request.cols) {
    throw new RangeError('segmentRecurrenceMatrix: invalid matrix dimensions');
  }
  return addon.segmentRecurrenceMatrix(
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

/** Convert an `n × n` recurrence matrix to its lag representation. */
export function segmentRecurrenceToLag(request: SegmentRecurrenceToLagRequest): SegmentMatrix {
  if (request.recurrence.length !== request.n * request.n) {
    throw new RangeError('segmentRecurrenceToLag: invalid matrix dimensions');
  }
  return addon.segmentRecurrenceToLag(request.recurrence, request.n, request.pad ?? false);
}

/** Convert a lag matrix back to an `n × n` recurrence matrix. */
export function segmentLagToRecurrence(request: SegmentLagToRecurrenceRequest): SegmentMatrix {
  if (request.lag.length !== request.rows * request.lags) {
    throw new RangeError('segmentLagToRecurrence: invalid matrix dimensions');
  }
  return addon.segmentLagToRecurrence(request.lag, request.rows, request.lags);
}

/** Refine frame boundaries by clustering within each parent segment. */
export function segmentSubsegment(request: SegmentSubsegmentRequest): Int32Array {
  if (request.data.length !== request.rows * request.cols) {
    throw new RangeError('segmentSubsegment: invalid matrix dimensions');
  }
  return addon.segmentSubsegment(
    request.data,
    request.rows,
    request.cols,
    request.boundaries,
    request.nSegments ?? 4,
  );
}

/** Cluster feature columns and return one label per column. */
export function segmentAgglomerative(request: SegmentAgglomerativeRequest): Int32Array {
  if (request.data.length !== request.rows * request.cols) {
    throw new RangeError('segmentAgglomerative: invalid matrix dimensions');
  }
  return addon.segmentAgglomerative(
    request.data,
    request.rows,
    request.cols,
    request.k,
    request.linkage ?? 'average',
  );
}

/** Enhance diagonal paths in an `n × n` recurrence matrix. */
export function segmentPathEnhance(request: SegmentPathEnhanceRequest): SegmentMatrix {
  if (request.recurrence.length !== request.n * request.n) {
    throw new RangeError('segmentPathEnhance: invalid matrix dimensions');
  }
  return addon.segmentPathEnhance(
    request.recurrence,
    request.n,
    request.win,
    request.maxRatio ?? 2,
    request.minRatio ?? 0,
    request.nFilters ?? 7,
  );
}

/**
 * NMF of a flattened [nFeatures x nFrames] spectrogram (librosa.decompose.decompose).
 *
 * `init` selects the initialiser: `'random'` (default, deterministic seed) or
 * `'nndsvd'` (SVD-based warm start, which tends to converge in fewer iterations).
 */
export function decompose(request: DecomposeRequest): { w: Matrix2D; h: Matrix2D };
export function decompose(
  s: Float32Array,
  nFeatures?: number,
  nFrames?: number,
  nComponents?: number,
  nIter?: number,
  beta?: number,
  init?: 'random' | 'nndsvd',
): { w: Matrix2D; h: Matrix2D };
export function decompose(
  s: Float32Array | DecomposeRequest,
  nFeatures = 0,
  nFrames = 0,
  nComponents = 0,
  nIter = 50,
  beta = 2.0,
  init: 'random' | 'nndsvd' = 'random',
): { w: Matrix2D; h: Matrix2D } {
  const request =
    s instanceof Float32Array ? { s, nFeatures, nFrames, nComponents, nIter, beta, init } : s;
  // Positivity only: 0 iterations returns the raw init matrices, which the C ABI
  // refuses for that reason and bounds no further.
  const resolvedNIter = resolvePositiveIntegerOption('decompose', 'nIter', request.nIter, 50);
  return addon.decompose(
    request.s,
    request.nFeatures,
    request.nFrames,
    request.nComponents,
    resolvedNIter,
    request.beta ?? 2,
    request.init ?? 'random',
  );
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
  assertSamples('decomposeStems', request.samples, true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('decomposeStems', resolvedSampleRate);
  // Each of the four is its own "0 => the documented default" on the C side, and
  // the addon's narrowing truncates onto that 0.
  for (const field of ['nComponents', 'nFft', 'hopLength', 'nIter'] as const) {
    const value = request[field];
    if (value !== undefined) {
      assertInt32('decomposeStems', value, field);
    }
  }
  return addon.decomposeStems(request.samples, resolvedSampleRate, {
    nComponents: request.nComponents,
    nFft: request.nFft,
    hopLength: request.hopLength,
    nIter: request.nIter,
    beta: request.beta,
    init: request.init,
    maskPower: request.maskPower,
  });
}

/** Nearest-neighbour filtering of a flattened [nFeatures x nFrames] spectrogram. */
export function nnFilter(request: NnFilterRequest): Matrix2D;
export function nnFilter(
  s: Float32Array,
  nFeatures?: number,
  nFrames?: number,
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
  const request = s instanceof Float32Array ? { s, nFeatures, nFrames, aggregate, k, width } : s;
  return addon.nnFilter(
    request.s,
    request.nFeatures,
    request.nFrames,
    request.aggregate ?? 'mean',
    request.k ?? 7,
    request.width ?? 1,
  );
}

export interface RemixRequest extends FeatureSamplesRequest {
  intervals: Int32Array | number[];
  alignZeros?: boolean;
}

/**
 * Reorder/concatenate a signal by (start,end) interval slices (librosa.effects.remix).
 *
 * With `alignZeros` the boundaries snap to the signal's zero-crossings, which is
 * a per-signal decision: calling this per channel snaps each channel to a
 * different frame and drifts a stereo take apart. Resolve one cut set with
 * {@link remixAlignedIntervals} and apply it to every channel instead.
 */
export function remix(request: RemixRequest): Float32Array;
export function remix(
  samples: Float32Array,
  intervals: Int32Array | number[],
  sampleRate?: number,
  alignZeros?: boolean,
): Float32Array;
export function remix(
  samples: Float32Array | RemixRequest,
  intervals: Int32Array | number[] = new Int32Array(),
  sampleRate = 22050,
  alignZeros = false,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, intervals, sampleRate, alignZeros } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('remix', resolvedSampleRate);
  return addon.remix(
    request.samples,
    request.intervals,
    resolvedSampleRate,
    request.alignZeros ?? false,
  );
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
 */
export function remixAlignedIntervals(request: RemixRequest): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array,
  intervals: Int32Array | number[],
  sampleRate?: number,
  alignZeros?: boolean,
): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array | RemixRequest,
  intervals: Int32Array | number[] = new Int32Array(),
  sampleRate = 22050,
  alignZeros = true,
): Int32Array {
  const request =
    samples instanceof Float32Array ? { samples, intervals, sampleRate, alignZeros } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('remixAlignedIntervals', resolvedSampleRate);
  return addon.remixAlignedIntervals(
    request.samples,
    request.intervals,
    resolvedSampleRate,
    request.alignZeros ?? true,
  );
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
 */
export function hpssWithResidual(request: HpssWithResidualRequest): {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
  sampleRate: number;
};
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
  sampleRate: number;
};
export function hpssWithResidual(
  samples: Float32Array | HpssWithResidualRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
  sampleRate: number;
} {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive, nFft, hopLength, hardMask }
      : samples;
  const fftOptions = resolveFftOptions('hpssWithResidual', request.nFft, request.hopLength);
  const resolvedHardMask = resolveHardMaskOption('hpssWithResidual', request.hardMask);
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpssWithResidual', resolvedKernelHarmonic, resolvedKernelPercussive);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('hpssWithResidual', resolvedSampleRate);
  return addon.hpssWithResidual(
    request.samples,
    resolvedSampleRate,
    resolvedKernelHarmonic,
    resolvedKernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}
