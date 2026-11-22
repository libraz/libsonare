/**
 * Source decomposition and self-similarity segmentation: NMF factorisation and
 * the masks built over it, and the recurrence structure of a track.
 */

import { refuse, withEntry } from './internal/entry.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertMatrix2D,
  optionalBoolean,
  optionalToken,
  requestOnly,
  sampleOneShot,
  toInt32Array,
  toMatrix2D,
} from './validation.js';

/** NMF initialisation: a deterministic random start or an SVD-based warm start. */
export type NmfInit = 'random' | 'nndsvd';

const NMF_INITS: readonly NmfInit[] = ['random', 'nndsvd'];

/** Request for {@link decompose}. */
export interface DecomposeRequest {
  /** Non-negative spectrogram: rows are features, cols are frames. */
  s: Matrix2D;
  /** Number of NMF components, a positive integer. */
  nComponents: number;
  /** Multiplicative-update iterations, a positive integer. Default 50. */
  nIter?: number;
  /** Beta divergence: 2 = Frobenius (default), 1 = Kullback-Leibler. */
  beta?: number;
  /** Initialisation, `'random'` (default, deterministic seed) or `'nndsvd'`. */
  init?: NmfInit;
}

/** The two NMF factors of {@link decompose}. */
export interface DecomposeResult {
  /** Component matrix: rows are features, cols are components. */
  w: Matrix2D;
  /** Activation matrix: rows are components, cols are frames. */
  h: Matrix2D;
}

/** Closed set of aggregation methods of {@link nnFilter}. */
export type NnFilterAggregate = 'mean' | 'median' | 'min' | 'max';

const NN_FILTER_AGGREGATES: readonly NnFilterAggregate[] = ['mean', 'median', 'min', 'max'];

/** Request for {@link nnFilter}. */
export interface NnFilterRequest {
  /** Spectrogram to filter: rows are features, cols are frames. */
  s: Matrix2D;
  /** Neighbour aggregation. Default `'mean'`. */
  aggregate?: NnFilterAggregate;
  /** Neighbours per frame, a positive integer. Default 7. */
  k?: number;
  /** Frames either side of a frame excluded from its neighbours, a non-negative integer. Default 1. */
  width?: number;
}

/** Cross-similarity request. */
export interface SegmentCrossSimilarityRequest {
  /** Rows are features, cols are frames. */
  x: Matrix2D;
  /** Rows are features, cols are frames; the feature count must match `x`. */
  y: Matrix2D;
  /** Nearest neighbours kept per frame, a positive integer. Absent: the librosa default, `min(n, 2 * ceil(sqrt(n)))` for `n` frames of `y`. */
  k?: number;
  /** Distance metric. Default `'cosine'`. */
  metric?: 'cosine' | 'euclidean';
  /** `'connectivity'` for a binary matrix or `'affinity'` for weighted similarity. Default `'connectivity'`. */
  mode?: 'connectivity' | 'affinity';
}

/** Self-similarity recurrence request. */
export interface SegmentRecurrenceMatrixRequest {
  /** Rows are features, cols are frames. */
  data: Matrix2D;
  /** Nearest neighbours kept per frame, a positive integer. Absent: the librosa default, `min(n - 1, 2 * ceil(sqrt(n)))` for `n` frames. */
  k?: number;
  /** Frames either side of a frame excluded from its neighbours. Default `1`. */
  width?: number;
  /** Make the matrix symmetric. Default `false`. */
  sym?: boolean;
  /** Distance metric. Default `'euclidean'`. */
  metric?: 'cosine' | 'euclidean';
  /** `'connectivity'` for a binary matrix or `'affinity'` for weighted similarity. Default `'connectivity'`. */
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
  /** Segment boundaries as frame indices. */
  boundaries: Int32Array | ArrayLike<number>;
  /** Sub-segments per segment. Default `4`. */
  nSegments?: number;
}

/** Feature-column clustering request. */
export interface SegmentAgglomerativeRequest {
  /** Rows are features, cols are frames. */
  data: Matrix2D;
  /** Number of segments to produce. */
  k: number;
  /** Cluster linkage. Default `'average'`. */
  linkage?: 'average' | 'single' | 'complete' | 'ward';
}

/** Diagonal path enhancement request. */
export interface SegmentPathEnhanceRequest {
  /** Square: rows = cols = frames. */
  recurrence: Matrix2D;
  /** Smoothing window length in frames; positive. */
  win: number;
  /** Largest diagonal slope tried, as a ratio. Default `2`. */
  maxRatio?: number;
  /** Smallest diagonal slope tried, as a ratio. Default `0`. */
  minRatio?: number;
  /** Number of diagonal filters. Default `7`. */
  nFilters?: number;
}

function assertSquare(matrix: Matrix2D, argName: string): void {
  if (matrix.rows !== matrix.cols) {
    throw refuse(`${argName} must be square`);
  }
}

/** Request for {@link remix} and {@link remixAlignedIntervals}. */
export interface RemixRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Flat (start, end) sample pairs (even length). */
  intervals: Int32Array | ArrayLike<number>;
  /** Snap the cut boundaries to zero crossings. Default `true`. */
  alignZeros?: boolean;
}

/**
 * Non-negative matrix factorisation of a spectrogram
 * (librosa.decompose.decompose). `init` selects the initialiser: `'random'`
 * (default, deterministic seed) or `'nndsvd'` (SVD-based warm start, which tends
 * to converge in fewer iterations).
 *
 * @throws {TypeError} when `nComponents` is missing or `s` is not a Matrix2D
 * @throws {RangeError} on a malformed matrix, a non-positive `nIter`, or an unknown `init`
 */
export function decompose(request: DecomposeRequest): DecomposeResult;
export function decompose(...args: unknown[]): DecomposeResult {
  return withEntry('decompose', () => {
    const r = requestOnly<DecomposeRequest>('decompose', args);
    const s = assertMatrix2D(r.s, 's');
    // An empty token is the embind reader's spelling of the library's initialiser.
    const init = optionalToken(r.init, 'init', NMF_INITS) ?? '';
    const native = getSonareModule().decompose(
      s.data,
      s.rows,
      s.cols,
      r.nComponents,
      r.nIter,
      r.beta ?? 2.0,
      init,
    );
    return {
      w: toMatrix2D(native.w, s.rows, r.nComponents),
      h: toMatrix2D(native.h, r.nComponents, s.cols),
    };
  });
}

/** Options for {@link decomposeStems}. */
export interface DecomposeStemsRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
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
  init?: NmfInit;
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
  /** Sample rate of the component signals in Hz. */
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
export function decomposeStems(request: DecomposeStemsRequest): DecomposeStemsResult;
export function decomposeStems(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DecomposeStemsRequest>,
): DecomposeStemsResult;
export function decomposeStems(...args: unknown[]): DecomposeStemsResult {
  return withEntry('decomposeStems', () => {
    const r = sampleOneShot<DecomposeStemsRequest>('decomposeStems', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return getSonareModule().decomposeStems(r.samples, r.sampleRate, {
      nComponents: r.nComponents,
      nFft: r.nFft,
      hopLength: r.hopLength,
      nIter: r.nIter,
      beta: r.beta,
      init: r.init,
      maskPower: r.maskPower,
    });
  });
}

/** Request form of {@link decomposeStemsLinked}. */
export interface DecomposeStemsLinkedRequest {
  /** At least one channel; all the same length. */
  channels: Float32Array[];
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
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
  init?: NmfInit;
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
  /** Sample rate of the component signals in Hz. */
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
  return withEntry('decomposeStemsLinked', () => {
    const r = requestOnly<DecomposeStemsLinkedRequest>('decomposeStemsLinked', [request]);
    assertAudioChannels(r.channels, r.sampleRate, r);
    return getSonareModule().decomposeStemsLinked(r.channels, r.sampleRate, {
      nComponents: r.nComponents,
      nFft: r.nFft,
      hopLength: r.hopLength,
      nIter: r.nIter,
      beta: r.beta,
      init: r.init,
      maskPower: r.maskPower,
    });
  });
}

/**
 * Nearest-neighbour filtering of a spectrogram (librosa.decompose.nn_filter).
 *
 * @returns {@link Matrix2D} of the input's shape: rows are features, cols are frames.
 * @throws {TypeError} when `s` is not a Matrix2D
 * @throws {RangeError} on a malformed matrix, a non-positive `k`, a negative `width` or an `aggregate` outside {@link NnFilterAggregate}
 */
export function nnFilter(request: NnFilterRequest): Matrix2D;
export function nnFilter(...args: unknown[]): Matrix2D {
  return withEntry('nnFilter', () => {
    const r = requestOnly<NnFilterRequest>('nnFilter', args);
    const s = assertMatrix2D(r.s, 's');
    // An empty token is the embind reader's spelling of the library's aggregate.
    const aggregate = optionalToken(r.aggregate, 'aggregate', NN_FILTER_AGGREGATES) ?? '';
    return getSonareModule().nnFilter(s.data, s.rows, s.cols, aggregate, r.k, r.width);
  });
}

/**
 * Reorder/concatenate a signal by interval slices (librosa.effects.remix).
 *
 * With `alignZeros` the boundaries snap to the signal's zero-crossings. That is
 * a per-signal decision, so calling this per channel snaps each channel to a
 * different frame and drifts a stereo take apart; resolve one cut set with
 * {@link remixAlignedIntervals} and apply it to every channel instead.
 *
 * @throws {TypeError} when `sampleRate` or `intervals` is missing
 * @throws {RangeError} on audio or intervals out of domain
 */
export function remix(request: RemixRequest): Float32Array;
export function remix(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<RemixRequest>,
): Float32Array;
export function remix(...args: unknown[]): Float32Array {
  return withEntry('remix', () => {
    const r = sampleOneShot<RemixRequest>('remix', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const alignZeros = optionalBoolean(r.alignZeros, 'alignZeros', true);
    // Sample indices must reach the native side as exact 32-bit integers, and a
    // boundary the conversion changed would cut the slice somewhere the caller
    // never named.
    const intervals = toInt32Array(r.intervals, 'intervals');
    return getSonareModule().remix(r.samples, intervals, r.sampleRate, alignZeros);
  });
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
 * @throws {TypeError} when `sampleRate` or `intervals` is missing
 * @throws {RangeError} on audio or intervals out of domain
 */
export function remixAlignedIntervals(request: RemixRequest): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<RemixRequest>,
): Int32Array;
export function remixAlignedIntervals(...args: unknown[]): Int32Array {
  return withEntry('remixAlignedIntervals', () => {
    const r = sampleOneShot<RemixRequest>('remixAlignedIntervals', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const alignZeros = optionalBoolean(r.alignZeros, 'alignZeros', true);
    const intervals = toInt32Array(r.intervals, 'intervals');
    return getSonareModule().remixAlignedIntervals(r.samples, intervals, r.sampleRate, alignZeros);
  });
}

/**
 * Column-wise cross-similarity (librosa.segment.cross_similarity).
 *
 * @returns {@link Matrix2D} whose rows are `y` frames and cols are `x` frames.
 */
export function segmentCrossSimilarity(request: SegmentCrossSimilarityRequest): Matrix2D;
export function segmentCrossSimilarity(...args: unknown[]): Matrix2D {
  return withEntry('segmentCrossSimilarity', () => {
    const r = requestOnly<SegmentCrossSimilarityRequest>('segmentCrossSimilarity', args);
    const x = assertMatrix2D(r.x, 'x');
    const y = assertMatrix2D(r.y, 'y');
    if (x.rows !== y.rows) {
      throw refuse('feature dimensions must match');
    }
    const native = getSonareModule().segmentCrossSimilarity(
      x.data,
      x.rows,
      x.cols,
      y.data,
      y.rows,
      y.cols,
      r.k,
      r.metric ?? 'cosine',
      r.mode ?? 'connectivity',
    );
    return toMatrix2D(native.values, native.rows, native.cols);
  });
}

/**
 * Self-similarity recurrence matrix (librosa.segment.recurrence_matrix).
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentRecurrenceMatrix(request: SegmentRecurrenceMatrixRequest): Matrix2D;
export function segmentRecurrenceMatrix(...args: unknown[]): Matrix2D {
  return withEntry('segmentRecurrenceMatrix', () => {
    const r = requestOnly<SegmentRecurrenceMatrixRequest>('segmentRecurrenceMatrix', args);
    const data = assertMatrix2D(r.data, 'data');
    const native = getSonareModule().segmentRecurrenceMatrix(
      data.data,
      data.rows,
      data.cols,
      r.k,
      r.width ?? 1,
      optionalBoolean(r.sym, 'sym', false),
      r.metric ?? 'euclidean',
      r.mode ?? 'connectivity',
    );
    return toMatrix2D(native.values, native.rows, native.cols);
  });
}

/**
 * Convert a square recurrence matrix to a lag-by-time matrix, as librosa does.
 *
 * @returns {@link Matrix2D} whose rows are lags and cols are frames.
 */
export function segmentRecurrenceToLag(request: SegmentRecurrenceToLagRequest): Matrix2D;
export function segmentRecurrenceToLag(...args: unknown[]): Matrix2D {
  return withEntry('segmentRecurrenceToLag', () => {
    const r = requestOnly<SegmentRecurrenceToLagRequest>('segmentRecurrenceToLag', args);
    const recurrence = assertMatrix2D(r.recurrence, 'recurrence');
    assertSquare(recurrence, 'recurrence');
    const native = getSonareModule().segmentRecurrenceToLag(
      recurrence.data,
      recurrence.rows,
      optionalBoolean(r.pad, 'pad', true),
    );
    return toMatrix2D(native.values, native.rows, native.cols);
  });
}

/**
 * Convert a lag matrix (lags = frames, or twice as many padded) to a recurrence.
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentLagToRecurrence(request: SegmentLagToRecurrenceRequest): Matrix2D;
export function segmentLagToRecurrence(...args: unknown[]): Matrix2D {
  return withEntry('segmentLagToRecurrence', () => {
    const r = requestOnly<SegmentLagToRecurrenceRequest>('segmentLagToRecurrence', args);
    const lag = assertMatrix2D(r.lag, 'lag');
    const native = getSonareModule().segmentLagToRecurrence(lag.data, lag.cols, lag.rows);
    return toMatrix2D(native.values, native.rows, native.cols);
  });
}

/** Refine frame boundaries by clustering within each parent segment. */
export function segmentSubsegment(request: SegmentSubsegmentRequest): Int32Array;
export function segmentSubsegment(...args: unknown[]): Int32Array {
  return withEntry('segmentSubsegment', () => {
    const r = requestOnly<SegmentSubsegmentRequest>('segmentSubsegment', args);
    const data = assertMatrix2D(r.data, 'data');
    return getSonareModule().segmentSubsegment(
      data.data,
      data.rows,
      data.cols,
      toInt32Array(r.boundaries, 'boundaries'),
      r.nSegments ?? 4,
    );
  });
}

/** Cluster feature columns and return one label per column. */
export function segmentAgglomerative(request: SegmentAgglomerativeRequest): Int32Array;
export function segmentAgglomerative(...args: unknown[]): Int32Array {
  return withEntry('segmentAgglomerative', () => {
    const r = requestOnly<SegmentAgglomerativeRequest>('segmentAgglomerative', args);
    const data = assertMatrix2D(r.data, 'data');
    return getSonareModule().segmentAgglomerative(
      data.data,
      data.rows,
      data.cols,
      r.k,
      r.linkage ?? 'average',
    );
  });
}

/**
 * Enhance diagonal paths in a square recurrence matrix.
 *
 * @returns Square {@link Matrix2D}: rows = cols = frames.
 */
export function segmentPathEnhance(request: SegmentPathEnhanceRequest): Matrix2D;
export function segmentPathEnhance(...args: unknown[]): Matrix2D {
  return withEntry('segmentPathEnhance', () => {
    const r = requestOnly<SegmentPathEnhanceRequest>('segmentPathEnhance', args);
    const recurrence = assertMatrix2D(r.recurrence, 'recurrence');
    assertSquare(recurrence, 'recurrence');
    const native = getSonareModule().segmentPathEnhance(
      recurrence.data,
      recurrence.rows,
      r.win,
      r.maxRatio ?? 2,
      r.minRatio ?? 0,
      r.nFilters ?? 7,
    );
    return toMatrix2D(native.values, native.rows, native.cols);
  });
}
