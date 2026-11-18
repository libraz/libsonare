import type { HpssWithResidualResult } from './effects_separation.js';
import { assertMatrix2D, toMatrix2D, toNativeIndexList } from './internal/feature_matrix.js';
import { resolveEffectFftOptions } from './internal/fft_options.js';
import { addon } from './native.js';
import type { Matrix2D } from './types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertClosedToken,
  assertHpssKernels,
  assertInt32,
  assertIntegerValue,
  assertPositiveInteger,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** NMF initialisation: a deterministic random start or an SVD-based warm start. */
export type NmfInit = 'random' | 'nndsvd';

const NMF_INITS: readonly NmfInit[] = ['random', 'nndsvd'];

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
  /** Nearest neighbours kept per frame, a positive integer. Absent: the librosa default, `min(n, 2 * ceil(sqrt(n)))` for `n` frames of `y`. */
  k?: number;
  /** Distance metric. Default `'cosine'`. */
  metric?: 'cosine' | 'euclidean';
  /** `'connectivity'` for a binary matrix or `'affinity'` for weighted similarity. Default `'connectivity'`. */
  mode?: 'connectivity' | 'affinity';
}

/** Request for {@link segmentRecurrenceMatrix}. */
export interface SegmentRecurrenceMatrixRequest {
  /** Feature matrix: rows = features, cols = frames. */
  data: Matrix2D;
  /** Nearest neighbours kept per frame, a positive integer. Absent: the librosa default, derived from the frame count. */
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
  /** Segment boundaries as frame indices. */
  boundaries: Int32Array | ArrayLike<number>;
  /** Sub-segments per segment. Default `4`. */
  nSegments?: number;
}

/** Request for {@link segmentAgglomerative}. */
export interface SegmentAgglomerativeRequest {
  /** Feature matrix: rows = features, cols = frames. */
  data: Matrix2D;
  /** Number of segments to produce. */
  k: number;
  /** Cluster linkage. Default `'average'`. */
  linkage?: 'average' | 'single' | 'complete' | 'ward';
}

/** Request for {@link segmentPathEnhance}. */
export interface SegmentPathEnhanceRequest {
  /** Recurrence matrix: square, rows = cols = frames. */
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

/** Request for {@link decompose}. */
export interface DecomposeRequest {
  /** Non-negative spectrogram: rows = features, cols = frames. */
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
  /** Component matrix: rows = features, cols = components. */
  w: Matrix2D;
  /** Activation matrix: rows = components, cols = frames. */
  h: Matrix2D;
}

/** Request for {@link decomposeStems}. */
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
/** Closed set of aggregation methods of {@link nnFilter}. */
export type NnFilterAggregate = 'mean' | 'median' | 'min' | 'max';

const NN_FILTER_AGGREGATES: readonly NnFilterAggregate[] = ['mean', 'median', 'min', 'max'];

/** Request for {@link nnFilter}. */
export interface NnFilterRequest {
  /** Spectrogram to filter: rows = features, cols = frames. */
  s: Matrix2D;
  /** Neighbour aggregation. Default `'mean'`. */
  aggregate?: NnFilterAggregate;
  /** Neighbours per frame, a positive integer. Default 7. */
  k?: number;
  /** Frames either side of a frame excluded from its neighbours, a non-negative integer. Default 1. */
  width?: number;
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
  if (r.k !== undefined) {
    assertPositiveInteger('segmentCrossSimilarity', r.k, 'k');
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
  if (r.k !== undefined) {
    assertPositiveInteger('segmentRecurrenceMatrix', r.k, 'k');
  }
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
    toNativeIndexList('segmentSubsegment', r.boundaries, 'boundaries'),
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
 * Non-negative matrix factorisation of a spectrogram (librosa.decompose.decompose).
 *
 * `init` selects the initialiser: `'random'` (default, deterministic seed) or
 * `'nndsvd'` (SVD-based warm start, which tends to converge in fewer iterations).
 *
 * @throws {TypeError} when `nComponents` is missing or `s` is not a Matrix2D
 * @throws {RangeError} on a malformed matrix, a non-positive `nComponents` or `nIter`, or an unknown `init`
 */
export function decompose(request: DecomposeRequest): DecomposeResult;
export function decompose(...args: unknown[]): DecomposeResult {
  const r = requestOnly<DecomposeRequest>('decompose', args);
  assertMatrix2D('decompose', r.s, 's');
  assertIntegerValue('decompose', r.nComponents, 'nComponents');
  assertPositiveInteger('decompose', r.nComponents, 'nComponents');
  if (r.init !== undefined) {
    assertClosedToken('decompose', r.init, NMF_INITS, 'init');
  }
  return addon.decompose(r.s.data, r.s.rows, r.s.cols, r.nComponents, r.nIter, r.beta ?? 2, r.init);
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
  const request = sampleOneShot<DecomposeStemsRequest>('decomposeStems', args);
  assertAudioInput('decomposeStems', request.samples, request.sampleRate, request);
  // Each of the four is its own "0 => the documented default" on the C side, and
  // the addon's narrowing truncates onto that 0.
  for (const field of ['nComponents', 'nFft', 'hopLength', 'nIter'] as const) {
    const value = request[field];
    if (value !== undefined) {
      assertInt32('decomposeStems', value, field);
    }
  }
  return addon.decomposeStems(request.samples, request.sampleRate, {
    nComponents: request.nComponents,
    nFft: request.nFft,
    hopLength: request.hopLength,
    nIter: request.nIter,
    beta: request.beta,
    init: request.init,
    maskPower: request.maskPower,
  });
}

/** Request for {@link decomposeStemsLinked}. */
export interface DecomposeStemsLinkedRequest extends ValidateOptions {
  /** One plane per channel, all of the same length. At least one. */
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
  /** Sample rate of the component signals in Hz. */
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
  requestOnly<DecomposeStemsLinkedRequest>('decomposeStemsLinked', [request]);
  if (!Array.isArray(request.channels) || request.channels.length === 0) {
    throw new RangeError('decomposeStemsLinked: channels must be a non-empty array');
  }
  assertAudioChannels('decomposeStemsLinked', request.channels, request.sampleRate, request);
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
  return addon.decomposeStemsLinked(request.channels, request.sampleRate, {
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
 * Nearest-neighbour filtering of a spectrogram (librosa.decompose.nn_filter).
 *
 * @returns {@link Matrix2D} of the input's shape: rows = features, cols = frames.
 * @throws {TypeError} when `s` is not a Matrix2D
 * @throws {RangeError} on a malformed matrix, a non-positive `k`, a negative `width` or an `aggregate` outside {@link NnFilterAggregate}
 */
export function nnFilter(request: NnFilterRequest): Matrix2D;
export function nnFilter(...args: unknown[]): Matrix2D {
  const r = requestOnly<NnFilterRequest>('nnFilter', args);
  assertMatrix2D('nnFilter', r.s, 's');
  if (r.aggregate !== undefined) {
    assertClosedToken('nnFilter', r.aggregate, NN_FILTER_AGGREGATES, 'aggregate');
  }
  return addon.nnFilter(r.s.data, r.s.rows, r.s.cols, r.aggregate, r.k, r.width);
}

/** Request for {@link remix} and {@link remixAlignedIntervals}. */
export interface RemixRequest extends ValidateOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Flat (start, end) sample pairs (even length). */
  intervals: Int32Array | number[];
  /** Snap the cut boundaries to zero crossings. Default `true`. */
  alignZeros?: boolean;
}

function resolveAlignZeros(fnName: string, value: unknown): boolean {
  if (value === undefined) {
    return true;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: alignZeros must be a boolean`);
  }
  return value;
}

/**
 * Reorder/concatenate a signal by (start,end) interval slices (librosa.effects.remix).
 *
 * With `alignZeros` (the default) the boundaries snap to the signal's
 * zero-crossings, which is a per-signal decision: calling this per channel snaps
 * each channel to a different frame and drifts a stereo take apart. Resolve one
 * cut set with {@link remixAlignedIntervals} and apply it to every channel instead.
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
  const request = sampleOneShot<RemixRequest>('remix', args);
  assertAudioInput('remix', request.samples, request.sampleRate, request);
  const alignZeros = resolveAlignZeros('remix', request.alignZeros);
  return addon.remix(
    request.samples,
    toNativeIndexList('remix', request.intervals, 'intervals'),
    request.sampleRate,
    alignZeros,
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
  const request = sampleOneShot<RemixRequest>('remixAlignedIntervals', args);
  assertAudioInput('remixAlignedIntervals', request.samples, request.sampleRate, request);
  const alignZeros = resolveAlignZeros('remixAlignedIntervals', request.alignZeros);
  return addon.remixAlignedIntervals(
    request.samples,
    toNativeIndexList('remixAlignedIntervals', request.intervals, 'intervals'),
    request.sampleRate,
    alignZeros,
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
export function hpssWithResidual(request: HpssWithResidualRequest): HpssWithResidualResult;
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HpssWithResidualRequest>,
): HpssWithResidualResult;
export function hpssWithResidual(...args: unknown[]): HpssWithResidualResult {
  const request = sampleOneShot<HpssWithResidualRequest>('hpssWithResidual', args);
  assertAudioInput('hpssWithResidual', request.samples, request.sampleRate, request);
  const fftOptions = resolveEffectFftOptions('hpssWithResidual', request.nFft, request.hopLength);
  const hardMask = resolveHardMaskOption('hpssWithResidual', request.hardMask);
  const kernelHarmonic = request.kernelHarmonic ?? 31;
  const kernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpssWithResidual', kernelHarmonic, kernelPercussive);
  return addon.hpssWithResidual(
    request.samples,
    request.sampleRate,
    kernelHarmonic,
    kernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    hardMask,
  );
}
