import { assertMatrix2D, toMatrix2D, toNativeIndexList } from './internal/feature_matrix.js';
import { resolvePositiveIntegerOption } from './internal/feature_options.js';
import { resolveEffectFftOptions } from './internal/fft_options.js';
import { addon } from './native.js';
import type { Matrix2D } from './types.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertHpssKernels,
  assertInt32,
  requestObject,
  requestOnly,
} from './validation.js';

function resolveHardMaskOption(fnName: string, value: unknown): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

/** Request for {@link segmentCrossSimilarity}. */
export interface SegmentCrossSimilarityRequest {
  /** Feature matrix: rows = features, cols = frames. */
  x: Matrix2D;
  /** Feature matrix: rows = features, cols = frames. */
  y: Matrix2D;
  k?: number;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}

/** Request for {@link segmentRecurrenceMatrix}. */
export interface SegmentRecurrenceMatrixRequest {
  /** Feature matrix: rows = features, cols = frames. */
  data: Matrix2D;
  k?: number;
  width?: number;
  sym?: boolean;
  metric?: 'cosine' | 'euclidean';
  mode?: 'connectivity' | 'affinity';
}

/** Request for {@link segmentRecurrenceToLag}. */
export interface SegmentRecurrenceToLagRequest {
  /** Recurrence matrix: square, rows = cols = frames. */
  recurrence: Matrix2D;
  /** Zero-pad so negative lags stay apart: a `2n × n` result (default true, as librosa). */
  pad?: boolean;
}

/** Request for {@link segmentLagToRecurrence}. */
export interface SegmentLagToRecurrenceRequest {
  /** Lag matrix: rows = lags (frames, or 2 × frames padded), cols = frames. */
  lag: Matrix2D;
}

/** Request for {@link segmentSubsegment}. */
export interface SegmentSubsegmentRequest {
  /** Feature matrix: rows = features, cols = frames. */
  data: Matrix2D;
  boundaries: Int32Array | ArrayLike<number>;
  nSegments?: number;
}

/** Request for {@link segmentAgglomerative}. */
export interface SegmentAgglomerativeRequest {
  /** Feature matrix: rows = features, cols = frames. */
  data: Matrix2D;
  k: number;
  linkage?: 'average' | 'single' | 'complete' | 'ward';
}

/** Request for {@link segmentPathEnhance}. */
export interface SegmentPathEnhanceRequest {
  /** Recurrence matrix: square, rows = cols = frames. */
  recurrence: Matrix2D;
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
export interface NnFilterRequest {
  s: Float32Array;
  nFeatures: number;
  nFrames: number;
  aggregate?: string;
  k?: number;
  width?: number;
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

interface NativeSegmentMatrix {
  rows: number;
  cols: number;
  values: Float32Array;
}

function toSegmentMatrix(m: NativeSegmentMatrix): Matrix2D {
  return toMatrix2D(m.values, m.rows, m.cols);
}

function assertSquareMatrix(fnName: string, matrix: Matrix2D, argName: string): void {
  if (matrix.rows !== matrix.cols) {
    throw new RangeError(`${fnName}: ${argName} must be square`);
  }
}

/** Column-wise cross-similarity matrix (librosa.segment.cross_similarity). Returns rows = y frames, cols = x frames. */
export function segmentCrossSimilarity(request: SegmentCrossSimilarityRequest): Matrix2D;
export function segmentCrossSimilarity(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentCrossSimilarityRequest>('segmentCrossSimilarity', args);
  assertMatrix2D('segmentCrossSimilarity', r.x, 'x');
  assertMatrix2D('segmentCrossSimilarity', r.y, 'y');
  if (r.x.rows !== r.y.rows) {
    throw new RangeError('segmentCrossSimilarity: x and y must have the same number of rows');
  }
  return toSegmentMatrix(
    addon.segmentCrossSimilarity(
      r.x.data,
      r.x.rows,
      r.x.cols,
      r.y.data,
      r.y.rows,
      r.y.cols,
      r.k ?? 0,
      r.metric ?? 'cosine',
      r.mode ?? 'connectivity',
    ),
  );
}

/** Self-similarity recurrence matrix (librosa.segment.recurrence_matrix). Returns square, rows = cols = frames. */
export function segmentRecurrenceMatrix(request: SegmentRecurrenceMatrixRequest): Matrix2D;
export function segmentRecurrenceMatrix(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentRecurrenceMatrixRequest>('segmentRecurrenceMatrix', args);
  assertMatrix2D('segmentRecurrenceMatrix', r.data, 'data');
  return toSegmentMatrix(
    addon.segmentRecurrenceMatrix(
      r.data.data,
      r.data.rows,
      r.data.cols,
      r.k ?? 0,
      r.width ?? 1,
      r.sym ?? false,
      r.metric ?? 'euclidean',
      r.mode ?? 'connectivity',
    ),
  );
}

/** Convert a square recurrence matrix to a lag-by-time matrix, as librosa does. Returns rows = lags, cols = frames. */
export function segmentRecurrenceToLag(request: SegmentRecurrenceToLagRequest): Matrix2D;
export function segmentRecurrenceToLag(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentRecurrenceToLagRequest>('segmentRecurrenceToLag', args);
  assertMatrix2D('segmentRecurrenceToLag', r.recurrence, 'recurrence');
  assertSquareMatrix('segmentRecurrenceToLag', r.recurrence, 'recurrence');
  return toSegmentMatrix(
    addon.segmentRecurrenceToLag(r.recurrence.data, r.recurrence.rows, r.pad ?? true),
  );
}

/** Convert a lag matrix (lags = frames, or 2 × frames padded) to a recurrence. Returns square, rows = cols = frames. */
export function segmentLagToRecurrence(request: SegmentLagToRecurrenceRequest): Matrix2D;
export function segmentLagToRecurrence(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentLagToRecurrenceRequest>('segmentLagToRecurrence', args);
  assertMatrix2D('segmentLagToRecurrence', r.lag, 'lag');
  return toSegmentMatrix(addon.segmentLagToRecurrence(r.lag.data, r.lag.cols, r.lag.rows));
}

/** Refine frame boundaries by clustering within each parent segment. */
export function segmentSubsegment(request: SegmentSubsegmentRequest): Int32Array;
export function segmentSubsegment(...args: unknown[]): Int32Array {
  const r = requestOnly<SegmentSubsegmentRequest>('segmentSubsegment', args);
  assertMatrix2D('segmentSubsegment', r.data, 'data');
  return addon.segmentSubsegment(
    r.data.data,
    r.data.rows,
    r.data.cols,
    toNativeIndexList(r.boundaries),
    r.nSegments ?? 4,
  );
}

/** Cluster feature columns and return one label per column. */
export function segmentAgglomerative(request: SegmentAgglomerativeRequest): Int32Array;
export function segmentAgglomerative(...args: unknown[]): Int32Array {
  const r = requestOnly<SegmentAgglomerativeRequest>('segmentAgglomerative', args);
  assertMatrix2D('segmentAgglomerative', r.data, 'data');
  return addon.segmentAgglomerative(
    r.data.data,
    r.data.rows,
    r.data.cols,
    r.k,
    r.linkage ?? 'average',
  );
}

/** Enhance diagonal paths in a square recurrence matrix. Returns square, rows = cols = frames. */
export function segmentPathEnhance(request: SegmentPathEnhanceRequest): Matrix2D;
export function segmentPathEnhance(...args: unknown[]): Matrix2D {
  const r = requestOnly<SegmentPathEnhanceRequest>('segmentPathEnhance', args);
  assertMatrix2D('segmentPathEnhance', r.recurrence, 'recurrence');
  assertSquareMatrix('segmentPathEnhance', r.recurrence, 'recurrence');
  return toSegmentMatrix(
    addon.segmentPathEnhance(
      r.recurrence.data,
      r.recurrence.rows,
      r.win,
      r.maxRatio ?? 2,
      r.minRatio ?? 0,
      r.nFilters ?? 7,
    ),
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
    s instanceof Float32Array
      ? { s, nFeatures, nFrames, nComponents, nIter, beta, init }
      : requestObject('decompose', s, 's');
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
  requestObject('decomposeStems', request, 'request', true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('decomposeStems', request.samples, resolvedSampleRate, {});
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

export interface DecomposeStemsLinkedRequest {
  /** One plane per channel, all of the same length. At least one. */
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
  /**
   * Soft-mask exponent (default 1). 1 keeps the magnitude ratio; 2 is the
   * Wiener-style power ratio, which separates harder at the cost of more
   * artefacts on overlapping partials. Must be >= 1.
   */
  maskPower?: number;
}
/** One time-domain signal per (component, channel) pair, plus the factorisation. */
export interface DecomposeStemsLinkedResult {
  /** `components[k][c]` is component `k`, channel `c`, the length of the input. */
  components: Float32Array[][];
  /** Component matrix [nBins x nComponents], row-major, from the channel-averaged plane. */
  w: Float32Array;
  /** Activation matrix [nComponents x nFrames], row-major. */
  h: Float32Array;
  sampleRate: number;
}

/**
 * Multi-channel form of {@link decomposeStems}: one NMF model and one soft
 * mask, built from the channels' averaged magnitude, applied unchanged to
 * every channel's own complex spectrum -- so no interchannel level or phase
 * difference moves, the same guarantee `masteringRepairDenoiseClassicalLinked`
 * gives its channel set.
 *
 * A single channel reproduces {@link decomposeStems} bit for bit: the average
 * over one channel is a division by 1, which does not change the bits.
 */
export function decomposeStemsLinked(
  request: DecomposeStemsLinkedRequest,
): DecomposeStemsLinkedResult {
  requestObject('decomposeStemsLinked', request, 'request', true);
  if (!Array.isArray(request.channels) || request.channels.length === 0) {
    throw new RangeError('decomposeStemsLinked: channels must be a non-empty array');
  }
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioChannels('decomposeStemsLinked', request.channels, resolvedSampleRate, request);
  const [first, ...rest] = request.channels;
  for (const channel of rest) {
    if (channel.length !== first.length) {
      throw new RangeError('decomposeStemsLinked: every channel must have the same length');
    }
  }
  for (const field of ['nComponents', 'nFft', 'hopLength', 'nIter'] as const) {
    const value = request[field];
    if (value !== undefined) {
      assertInt32('decomposeStemsLinked', value, field);
    }
  }
  return addon.decomposeStemsLinked(request.channels, resolvedSampleRate, {
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
  const request =
    s instanceof Float32Array
      ? { s, nFeatures, nFrames, aggregate, k, width }
      : requestObject('nnFilter', s, 's');
  return addon.nnFilter(
    request.s,
    request.nFeatures,
    request.nFrames,
    request.aggregate ?? 'mean',
    request.k ?? 7,
    request.width ?? 1,
  );
}

export interface RemixRequest {
  samples: Float32Array;
  sampleRate?: number;
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
    samples instanceof Float32Array
      ? { samples, intervals, sampleRate, alignZeros }
      : requestObject('remix', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('remix', request.samples, resolvedSampleRate, request);
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
    samples instanceof Float32Array
      ? { samples, intervals, sampleRate, alignZeros }
      : requestObject('remixAlignedIntervals', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('remixAlignedIntervals', request.samples, resolvedSampleRate, request);
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
      : requestObject('hpssWithResidual', samples);
  const fftOptions = resolveEffectFftOptions('hpssWithResidual', request.nFft, request.hopLength);
  const resolvedHardMask = resolveHardMaskOption('hpssWithResidual', request.hardMask);
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpssWithResidual', resolvedKernelHarmonic, resolvedKernelPercussive);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('hpssWithResidual', request.samples, resolvedSampleRate, request);
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
