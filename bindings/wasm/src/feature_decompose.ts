/**
 * Source decomposition and self-similarity segmentation: NMF factorisation and
 * the masks built over it, and the recurrence structure of a track.
 */

import { resolveEffectFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertHpssKernels,
  assertIntegerValue,
  assertMatrix2D,
  assertNonNegativeInteger,
  assertPositiveInteger,
  optionalBoolean,
  optionalToken,
  requestOnly,
  sampleOneShot,
  toInt32Array,
  toMatrix2D,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

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

function assertSquare(fnName: string, matrix: Matrix2D, argName: string): void {
  if (matrix.rows !== matrix.cols) {
    throw new RangeError(`${fnName}: ${argName} must be square`);
  }
}

/** Request for {@link remix} and {@link remixAlignedIntervals}. */
export interface RemixRequest extends ValidateOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Flat (start, end) sample pairs (even length). */
  intervals: Int32Array | ArrayLike<number>;
  /** Snap the cut boundaries to zero crossings. Default `true`. */
  alignZeros?: boolean;
}

/** Request for {@link hpssWithResidual}. */
export interface HpssWithResidualRequest extends ValidateOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Horizontal median filter size, in STFT frames: a positive odd integer at
   * most 524287. Default 31. The ceiling is 524288 and an even kernel is
   * refused, so 524287 is the largest legal value.
   */
  kernelHarmonic?: number;
  /** Vertical median filter size, in STFT bins, under the same rule. Default 31. */
  kernelPercussive?: number;
  /** FFT size in samples. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Threshold the masks into binary ones instead of the soft split. Default `false`. */
  hardMask?: boolean;
}

/** Harmonic / percussive / residual signals from {@link hpssWithResidual}. */
export interface HpssWithResidualResult {
  /** Harmonic component, the length of the input. */
  harmonic: Float32Array;
  /** Percussive component, the length of the input. */
  percussive: Float32Array;
  /** Remainder of the input after subtracting the harmonic and percussive components. */
  residual: Float32Array;
  /** Sample rate of the returned signals in Hz. */
  sampleRate: number;
}

/**
 * Non-negative matrix factorisation of a spectrogram
 * (librosa.decompose.decompose). `init` selects the initialiser: `'random'`
 * (default, deterministic seed) or `'nndsvd'` (SVD-based warm start, which tends
 * to converge in fewer iterations).
 *
 * @throws {TypeError} when `nComponents` is missing or `s` is not a Matrix2D
 * @throws {RangeError} on a malformed matrix, a non-positive `nComponents` or `nIter`, or an unknown `init`
 */
export function decompose(request: DecomposeRequest): DecomposeResult;
export function decompose(...args: unknown[]): DecomposeResult {
  const r = requestOnly<DecomposeRequest>('decompose', args);
  const s = assertMatrix2D('decompose', r.s, 's');
  assertIntegerValue('decompose', r.nComponents, 'nComponents');
  assertPositiveInteger('decompose', r.nComponents, 'nComponents');
  // An empty token is the embind reader's spelling of the library's initialiser.
  const init = optionalToken('decompose', r.init, 'init', NMF_INITS) ?? '';
  const native = requireModule().decomposeWithInit(
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
}

/** Options for {@link decomposeStems}. */
export interface DecomposeStemsRequest extends ValidateOptions {
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
  const r = sampleOneShot<DecomposeStemsRequest>('decomposeStems', args);
  assertAudioInput('decomposeStems', r.samples, r.sampleRate, r);
  return requireModule().decomposeStems(r.samples, r.sampleRate, {
    nComponents: r.nComponents,
    nFft: r.nFft,
    hopLength: r.hopLength,
    nIter: r.nIter,
    beta: r.beta,
    init: r.init,
    maskPower: r.maskPower,
  });
}

/** Request form of {@link decomposeStemsLinked}. */
export interface DecomposeStemsLinkedRequest extends ValidateOptions {
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
  const r = requestOnly<DecomposeStemsLinkedRequest>('decomposeStemsLinked', [request]);
  assertAudioChannels('decomposeStemsLinked', r.channels, r.sampleRate, r);
  return requireModule().decomposeStemsLinked(r.channels, r.sampleRate, {
    nComponents: r.nComponents,
    nFft: r.nFft,
    hopLength: r.hopLength,
    nIter: r.nIter,
    beta: r.beta,
    init: r.init,
    maskPower: r.maskPower,
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
  const r = requestOnly<NnFilterRequest>('nnFilter', args);
  const s = assertMatrix2D('nnFilter', r.s, 's');
  // An empty token is the embind reader's spelling of the library's aggregate.
  const aggregate = optionalToken('nnFilter', r.aggregate, 'aggregate', NN_FILTER_AGGREGATES) ?? '';
  return requireModule().nnFilter(s.data, s.rows, s.cols, aggregate, r.k, r.width);
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
  const r = sampleOneShot<RemixRequest>('remix', args);
  assertAudioInput('remix', r.samples, r.sampleRate, r);
  const alignZeros = optionalBoolean('remix', r.alignZeros, 'alignZeros', true);
  // Sample indices must reach the native side as exact 32-bit integers, and a
  // boundary the conversion changed would cut the slice somewhere the caller
  // never named.
  const intervals = toInt32Array('remix', r.intervals, 'intervals');
  return requireModule().remix(r.samples, intervals, r.sampleRate, alignZeros);
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
  const r = sampleOneShot<RemixRequest>('remixAlignedIntervals', args);
  assertAudioInput('remixAlignedIntervals', r.samples, r.sampleRate, r);
  const alignZeros = optionalBoolean('remixAlignedIntervals', r.alignZeros, 'alignZeros', true);
  const intervals = toInt32Array('remixAlignedIntervals', r.intervals, 'intervals');
  return requireModule().remixAlignedIntervals(r.samples, intervals, r.sampleRate, alignZeros);
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
export function hpssWithResidual(request: HpssWithResidualRequest): HpssWithResidualResult;
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HpssWithResidualRequest>,
): HpssWithResidualResult;
export function hpssWithResidual(...args: unknown[]): HpssWithResidualResult {
  const r = sampleOneShot<HpssWithResidualRequest>('hpssWithResidual', args);
  assertAudioInput('hpssWithResidual', r.samples, r.sampleRate, r);
  const fftOptions = resolveEffectFftOptions('hpssWithResidual', r.nFft, r.hopLength);
  const hardMask = optionalBoolean('hpssWithResidual', r.hardMask, 'hardMask', false);
  const kernelHarmonic = r.kernelHarmonic ?? 31;
  const kernelPercussive = r.kernelPercussive ?? 31;
  assertHpssKernels('hpssWithResidual', kernelHarmonic, kernelPercussive);
  return requireModule().hpssWithResidualEx(
    r.samples,
    r.sampleRate,
    kernelHarmonic,
    kernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    hardMask,
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
  if (r.k !== undefined) {
    assertPositiveInteger('segmentCrossSimilarity', r.k, 'k');
  }
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
  if (r.k !== undefined) {
    assertPositiveInteger('segmentRecurrenceMatrix', r.k, 'k');
  }
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
