import { addon } from './native.js';
import type {
  ChromaResult,
  CqtResult,
  InverseMelResult,
  InverseStftResult,
  LufsResult,
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  NoteSegment,
  PiptrackResult,
  PitchResult,
  ReassignedSpectrogramResult,
  StftDbResult,
  StftResult,
  TempogramMode,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples, resolveFftOptions } from './validation.js';

function resolvePositiveIntegerOption(
  fnName: string,
  name: string,
  value: unknown,
  fallback: number,
): number {
  const resolved = value === undefined ? fallback : value;
  if (typeof resolved !== 'number' || !Number.isInteger(resolved)) {
    throw new TypeError(`${fnName}: ${name} must be an integer`);
  }
  if (resolved <= 0 || resolved > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: ${name} must be a positive integer`);
  }
  return resolved;
}

function resolveHardMaskOption(fnName: string, value: unknown): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

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
export interface OnsetBacktrackRequest {
  events: Int32Array | number[];
  energy: Float32Array;
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
export interface ReassignedSpectrogramRequest extends StftRequest {
  refPower?: number;
  fillNan?: boolean;
}
/**
 * Options for the constant-Q chroma variants.
 *
 * Deliberately not an `StftRequest`: `chromaCens` and `chromaCqt` are built on
 * a constant-Q transform, which resolves frequency through per-bin filter
 * lengths rather than a single framed FFT, so there is no FFT size to set. The
 * core config carries none either. Use `chroma` for the STFT-framed chromagram,
 * which does take `nFft`.
 */
export interface ChromaRequest extends FeatureSamplesRequest {
  hopLength?: number;
  nChroma?: number;
  binsPerOctave?: number;
}

/**
 * Options for the bass-focused chroma.
 *
 * Like {@link ChromaRequest} this is constant-Q based and takes no `nFft`. It
 * also takes no `binsPerOctave`: the bass chroma's bin count and its lowest
 * frequency are chosen together, so the resolution is not independently
 * settable through the exposed entry point.
 */
export interface BassChromaRequest extends FeatureSamplesRequest {
  hopLength?: number;
  nChroma?: number;
}

/**
 * Compile-time guard for the two request shapes above. A field the entry point
 * cannot forward is worse than a missing one: it type-checks, runs, and returns
 * the default silently. These aliases fail to compile if either field comes
 * back, so restoring one has to be a deliberate act.
 */
type AbsentKey<T extends never> = T;
type _ChromaRequestHasNoFftSize = AbsentKey<Extract<keyof ChromaRequest, 'nFft'>>;
type _BassChromaRequestHasNoResolutionControls = AbsentKey<
  Extract<keyof BassChromaRequest, 'nFft' | 'binsPerOctave'>
>;
export interface CqtRequest extends FeatureSamplesRequest {
  hopLength?: number;
  fmin?: number;
  nBins?: number;
  binsPerOctave?: number;
}
export interface VqtRequest extends CqtRequest {
  gamma?: number;
}
export interface CqtToAudioRequest {
  magnitude: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  hopLength?: number;
  fmin?: number;
  binsPerOctave?: number;
  nIter?: number;
}
export interface VqtToAudioRequest extends CqtToAudioRequest {
  gamma?: number;
}
export interface MelToStftRequest {
  mel: Float32Array;
  nMels: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}
export interface MelToAudioRequest extends MelToStftRequest {
  hopLength?: number;
  nIter?: number;
}
export interface GriffinLimRequest {
  magnitude: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nIter?: number;
  momentum?: number;
}
export interface MfccToMelRequest {
  mfcc: Float32Array;
  nMfcc: number;
  nFrames: number;
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}
export interface MfccToAudioRequest extends MfccToMelRequest {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  nIter?: number;
  htk?: boolean;
}
export interface SpectralContrastRequest extends StftRequest {
  nBands?: number;
  fmin?: number;
  quantile?: number;
}
export interface PolyFeaturesRequest extends StftRequest {
  order?: number;
}
export interface EstimateTuningRequest extends StftRequest {
  resolution?: number;
  binsPerOctave?: number;
}
export interface PitchRequest extends FeatureSamplesRequest {
  frameLength?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  threshold?: number;
  /** pYIN: fill unvoiced f0 with zero. Retained but ignored by YIN, which always estimates f0. */
  fillNa?: boolean;
}
export interface PiptrackRequest extends StftRequest {
  fmin?: number;
  fmax?: number;
  threshold?: number;
}
export interface NoteSegmentsConfig {
  segmentationThresholdCents?: number;
  minNoteMs?: number;
  referenceHz?: number;
  /** Voicing threshold applied to `voicedProb`; defaults to `0.5`. */
  voicedThreshold?: number;
}
export interface NoteSegmentsRequest extends NoteSegmentsConfig {
  f0Hz: Float32Array;
  /**
   * Per-frame voicing values in `[0, 1]`; anything below `voicedThreshold` is
   * unvoiced.
   *
   * Pass `pitchPyin`'s `voicedFlag` converted to `0`/`1`. Do **not** pass its
   * `voicedProb`: that value is the frame's voiced observation mass and rises
   * with F0 for a fixed `frameLength`, so a fixed threshold silently returns no
   * segments at all for low-register material.
   */
  voicedProb: Float32Array;
  frameRate: number;
  /** @deprecated Use the flat tuning fields on this request instead. */
  config?: NoteSegmentsConfig;
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
export interface PhaseVocoderRequest extends FeatureSamplesRequest {
  rate: number;
  nFft?: number;
  hopLength?: number;
}
export interface HpssWithResidualRequest extends FeatureSamplesRequest {
  kernelHarmonic?: number;
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
}
export interface TempogramRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  winLength?: number;
  mode?: TempogramMode;
  center?: boolean;
  norm?: boolean;
}
export interface CyclicTempogramRequest extends TempogramRequest {
  bpmMin?: number;
  nBins?: number;
}
export interface PlpRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  tempoMin?: number;
  tempoMax?: number;
  winLength?: number;
}
export interface OnsetEnvelopeRequest extends MelSpectrogramRequest {}
export interface OnsetStrengthMultiRequest extends MelSpectrogramRequest {
  nBands?: number;
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
export interface TempogramRatioRequest {
  tempogramData: Float32Array;
  winLength?: number;
  sampleRate?: number;
  hopLength?: number;
  factors?: Float32Array | number[];
}
export interface TrimSilenceRequest {
  samples: Float32Array;
  topDb?: number;
  frameLength?: number;
  hopLength?: number;
}
export interface FrameSignalRequest {
  samples: Float32Array;
  frameLength: number;
  hopLength: number;
}
export interface ValuesRequest {
  values: Float32Array;
}
export interface ToneRequest {
  frequency?: number;
  sampleRate?: number;
  duration?: number;
  phase?: number;
  amplitude?: number;
}
export interface ChirpRequest {
  fmin?: number;
  fmax?: number;
  sampleRate?: number;
  duration?: number;
  linear?: boolean;
}
export interface ClicksRequest {
  times: Float32Array;
  sampleRate?: number;
  length?: number;
  frequency?: number;
  clickDuration?: number;
}
/** Input for pre/de-emphasis filters. `zi` is the initial delay value. */
export interface EmphasisRequest {
  samples: Float32Array;
  coef?: number;
  zi?: number;
}
/** Input for NNLS chroma extraction. */
export interface NnlsChromaRequest extends FeatureSamplesRequest {
  enableStftBlend?: boolean;
  stftBlendWeight?: number;
  stftBlendNFft?: number;
  hopLength?: number;
}
/** Input for LUFS feature functions, including optional input validation control. */
export interface LufsRequest extends FeatureSamplesRequest, ValidateOptions {}

export function trim(
  request: FeatureSamplesRequest & {
    thresholdDb?: number;
    frameLength?: number;
    hopLength?: number;
  },
): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate?: number,
  thresholdDb?: number,
  frameLength?: number,
  hopLength?: number,
): Float32Array;
export function trim(
  samples:
    | Float32Array
    | (FeatureSamplesRequest & {
        thresholdDb?: number;
        frameLength?: number;
        hopLength?: number;
      }),
  sampleRate = 22050,
  thresholdDb = -60.0,
  frameLength?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, thresholdDb, frameLength, hopLength }
      : samples;
  const resolvedFrameLength = resolvePositiveIntegerOption(
    'trim',
    'frameLength',
    request.frameLength,
    2048,
  );
  const resolvedHopLength = resolvePositiveIntegerOption(
    'trim',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.trim(
    request.samples,
    request.sampleRate ?? 22050,
    request.thresholdDb ?? -60.0,
    resolvedFrameLength,
    resolvedHopLength,
  );
}

// -- Features --

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
  if (!Number.isFinite(request.refPower ?? 1e-6) || (request.refPower ?? 1e-6) < 0) {
    throw new RangeError('reassignedSpectrogram: refPower must be finite and non-negative');
  }
  return addon.reassignedSpectrogram(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.refPower ?? 1e-6,
    request.fillNan ?? false,
  );
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
 * STFT chromagram (librosa.feature.chroma_stft).
 *
 * The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
 * librosa.feature.chroma_stft — which estimates tuning from the signal when none
 * is given — this does NOT auto-estimate and exposes no tuning argument, so
 * sharp/flat (non-A440) recordings smear across pitch classes. Estimate tuning
 * separately via {@link estimateTuning} if a non-A440 reference matters.
 */
export function chroma(request: StftRequest): ChromaResult;
export function chroma(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
): ChromaResult;
export function chroma(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): ChromaResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  return addon.chroma(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}

export function chromaCens(request: ChromaRequest): ChromaResult;
export function chromaCens(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  binsPerOctave?: number,
): ChromaResult;
export function chromaCens(
  samples: Float32Array | ChromaRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  binsPerOctave = 36,
): ChromaResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, nChroma, binsPerOctave }
      : samples;
  return addon.chromaCens(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.nChroma ?? 12,
    request.binsPerOctave ?? 36,
  );
}

export function chromaCqt(request: ChromaRequest): ChromaResult;
export function chromaCqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  binsPerOctave?: number,
): ChromaResult;
export function chromaCqt(
  samples: Float32Array | ChromaRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  binsPerOctave = 36,
): ChromaResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, nChroma, binsPerOctave }
      : samples;
  return addon.chromaCqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.nChroma ?? 12,
    request.binsPerOctave ?? 36,
  );
}

export function bassChroma(request: BassChromaRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
): ChromaResult;
export function bassChroma(
  samples: Float32Array | BassChromaRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
): ChromaResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, hopLength, nChroma } : samples;
  return addon.bassChroma(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.nChroma ?? 12,
  );
}

/** Compute the Constant-Q Transform magnitude. */
export function cqt(request: CqtRequest): CqtResult;
export function cqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  nBins?: number,
  binsPerOctave?: number,
): CqtResult;
export function cqt(
  samples: Float32Array | CqtRequest,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, fmin, nBins, binsPerOctave }
      : samples;
  return addon.cqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
  );
}

/** Compute a faster pseudo-CQT magnitude approximation. */
export function pseudoCqt(request: CqtRequest): CqtResult;
export function pseudoCqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  nBins?: number,
  binsPerOctave?: number,
): CqtResult;
export function pseudoCqt(
  samples: Float32Array | CqtRequest,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, fmin, nBins, binsPerOctave }
      : samples;
  return addon.pseudoCqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
  );
}

/** Compute the hybrid CQT magnitude. */
export function hybridCqt(request: CqtRequest): CqtResult;
export function hybridCqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  nBins?: number,
  binsPerOctave?: number,
): CqtResult;
export function hybridCqt(
  samples: Float32Array | CqtRequest,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, fmin, nBins, binsPerOctave }
      : samples;
  return addon.hybridCqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
  );
}

/** Compute VQT magnitude (`gamma < 0` selects the automatic ERB-derived value). */
export function vqt(request: VqtRequest): CqtResult;
export function vqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  nBins?: number,
  binsPerOctave?: number,
  gamma?: number,
): CqtResult;
export function vqt(
  samples: Float32Array | VqtRequest,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  gamma = -1.0,
): CqtResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, fmin, nBins, binsPerOctave, gamma }
      : samples;
  return addon.vqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
    request.gamma ?? -1,
  );
}

/** Reconstruct mono audio from a row-major CQT magnitude via Griffin-Lim. */
export function cqtToAudio(request: CqtToAudioRequest): Float32Array;
export function cqtToAudio(
  magnitude: Float32Array,
  nBins?: number,
  nFrames?: number,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  binsPerOctave?: number,
  nIter?: number,
): Float32Array;
export function cqtToAudio(
  magnitude: Float32Array | CqtToAudioRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  binsPerOctave = 12,
  nIter = 32,
): Float32Array {
  const request =
    magnitude instanceof Float32Array
      ? { magnitude, nBins, nFrames, sampleRate, hopLength, fmin, binsPerOctave, nIter }
      : magnitude;
  return addon.cqtToAudio(
    request.magnitude,
    request.nBins,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.binsPerOctave ?? 12,
    request.nIter ?? 32,
  );
}

/** Reconstruct mono audio from a row-major VQT magnitude via Griffin-Lim. */
export function vqtToAudio(request: VqtToAudioRequest): Float32Array;
export function vqtToAudio(
  magnitude: Float32Array,
  nBins?: number,
  nFrames?: number,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  binsPerOctave?: number,
  gamma?: number,
  nIter?: number,
): Float32Array;
export function vqtToAudio(
  magnitude: Float32Array | VqtToAudioRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  binsPerOctave = 12,
  gamma = -1,
  nIter = 32,
): Float32Array {
  // The request form delegates to the positional form so the defaults live in
  // exactly one place: `gamma` in particular must reach the core as the
  // automatic-VQT sentinel (-1), not as the constant-Q value (0).
  if (!(magnitude instanceof Float32Array)) {
    return vqtToAudio(
      magnitude.magnitude,
      magnitude.nBins,
      magnitude.nFrames,
      magnitude.sampleRate,
      magnitude.hopLength,
      magnitude.fmin,
      magnitude.binsPerOctave,
      magnitude.gamma,
      magnitude.nIter,
    );
  }
  return addon.vqtToAudio(
    magnitude,
    nBins,
    nFrames,
    sampleRate,
    hopLength,
    fmin,
    binsPerOctave,
    gamma,
    nIter,
  );
}

/** Reconstruct STFT power from a mel spectrogram. */
export function melToStft(request: MelToStftRequest): InverseStftResult;
export function melToStft(
  mel: Float32Array,
  nMels?: number,
  nFrames?: number,
  sampleRate?: number,
  nFft?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
): InverseStftResult;
export function melToStft(
  mel: Float32Array | MelToStftRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  fmin = 0,
  fmax = 0,
  htk = false,
): InverseStftResult {
  const request =
    mel instanceof Float32Array ? { mel, nMels, nFrames, sampleRate, nFft, fmin, fmax, htk } : mel;
  return addon.melToStft(
    request.mel,
    request.nMels,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.htk ?? false,
  );
}

/** Reconstruct audio from a mel spectrogram via Griffin-Lim. */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(
  mel: Float32Array,
  nMels?: number,
  nFrames?: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
): Float32Array;
export function melToAudio(
  mel: Float32Array | MelToAudioRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
): Float32Array {
  const request =
    mel instanceof Float32Array
      ? { mel, nMels, nFrames, sampleRate, nFft, hopLength, fmin, fmax, nIter, htk }
      : mel;
  return addon.melToAudio(
    request.mel,
    request.nMels,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.nIter ?? 32,
    request.htk ?? false,
  );
}

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(
  magnitude: Float32Array,
  nBins: number,
  nFrames: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nIter?: number,
  momentum?: number,
): Float32Array;
export function griffinLim(
  magnitude: Float32Array | GriffinLimRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nIter = 32,
  momentum = 0.99,
): Float32Array {
  const request =
    magnitude instanceof Float32Array
      ? { magnitude, nBins, nFrames, sampleRate, nFft, hopLength, nIter, momentum }
      : magnitude;
  return addon.griffinLim(
    request.magnitude,
    request.nBins,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.nIter ?? 32,
    request.momentum ?? 0.99,
  );
}

/** Reconstruct a mel power spectrogram from MFCCs (`nMels` mel bands). */
export function mfccToMel(request: MfccToMelRequest): InverseMelResult;
export function mfccToMel(
  mfcc: Float32Array,
  nMfcc?: number,
  nFrames?: number,
  nMels?: number,
  lifter?: number,
): InverseMelResult;
export function mfccToMel(
  mfcc: Float32Array | MfccToMelRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  lifter = 0,
): InverseMelResult {
  const request = mfcc instanceof Float32Array ? { mfcc, nMfcc, nFrames, nMels, lifter } : mfcc;
  return addon.mfccToMel(
    request.mfcc,
    request.nMfcc,
    request.nFrames,
    request.nMels ?? 128,
    request.lifter ?? 0,
  );
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(
  mfcc: Float32Array,
  nMfcc?: number,
  nFrames?: number,
  nMels?: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
  lifter?: number,
): Float32Array;
export function mfccToAudio(
  mfcc: Float32Array | MfccToAudioRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
  lifter = 0,
): Float32Array {
  const request =
    mfcc instanceof Float32Array
      ? { mfcc, nMfcc, nFrames, nMels, sampleRate, nFft, hopLength, fmin, fmax, nIter, htk, lifter }
      : mfcc;
  return addon.mfccToAudio(
    request.mfcc,
    request.nMfcc,
    request.nFrames,
    request.nMels ?? 128,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.nIter ?? 32,
    request.htk ?? false,
    request.lifter ?? 0,
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

/** Spectral contrast (librosa.feature.spectral_contrast); (nBands+1) x nFrames. */
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

/** Global tuning offset from a set of frequencies (librosa.pitch_tuning). */
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
  const request =
    frequencies instanceof Float32Array ? { frequencies, resolution, binsPerOctave } : frequencies;
  return addon.pitchTuning(
    request.frequencies,
    request.resolution ?? 0.01,
    request.binsPerOctave ?? 12,
  );
}

/** Tuning offset of an audio signal (librosa.estimate_tuning). */
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
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, resolution, binsPerOctave }
      : samples;
  return addon.estimateTuning(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.resolution ?? 0.01,
    request.binsPerOctave ?? 12,
  );
}

/** Per-bin spectral pitch candidates and their peak magnitudes (librosa.piptrack). */
export function piptrack(request: PiptrackRequest): PiptrackResult;
export function piptrack(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
): PiptrackResult;
export function piptrack(
  samples: Float32Array | PiptrackRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 150,
  fmax = 4000,
  threshold = 0.1,
): PiptrackResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, fmin, fmax, threshold }
      : samples;
  assertSamples('piptrack', request.samples, true);
  return addon.piptrack(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 150,
    request.fmax ?? 4000,
    request.threshold ?? 0.1,
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
  return addon.decompose(
    request.s,
    request.nFeatures,
    request.nFrames,
    request.nComponents,
    request.nIter ?? 50,
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
  return addon.decomposeStems(request.samples, request.sampleRate ?? 22050, {
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

/**
 * Reorder/concatenate a signal by (start,end) interval slices (librosa.effects.remix).
 *
 * With `alignZeros` the boundaries snap to the signal's zero-crossings, which is
 * a per-signal decision: calling this per channel snaps each channel to a
 * different frame and drifts a stereo take apart. Resolve one cut set with
 * {@link remixAlignedIntervals} and apply it to every channel instead.
 */
export function remix(
  request: FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean },
): Float32Array;
export function remix(
  samples: Float32Array,
  intervals: Int32Array,
  sampleRate?: number,
  alignZeros?: boolean,
): Float32Array;
export function remix(
  samples: Float32Array | (FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean }),
  intervals: Int32Array = new Int32Array(),
  sampleRate = 22050,
  alignZeros = false,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, intervals, sampleRate, alignZeros } : samples;
  return addon.remix(
    request.samples,
    request.intervals,
    request.sampleRate ?? 22050,
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
export function remixAlignedIntervals(
  request: FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean },
): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array,
  intervals: Int32Array,
  sampleRate?: number,
  alignZeros?: boolean,
): Int32Array;
export function remixAlignedIntervals(
  samples: Float32Array | (FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean }),
  intervals: Int32Array = new Int32Array(),
  sampleRate = 22050,
  alignZeros = true,
): Int32Array {
  const request =
    samples instanceof Float32Array ? { samples, intervals, sampleRate, alignZeros } : samples;
  return addon.remixAlignedIntervals(
    request.samples,
    request.intervals,
    request.sampleRate ?? 22050,
    request.alignZeros ?? true,
  );
}

/** Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower). */
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
  rate = Number.NaN,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, rate, nFft, hopLength } : samples;
  if (typeof request.rate !== 'number' || !Number.isFinite(request.rate)) {
    throw new TypeError('phaseVocoder: rate must be a finite number');
  }
  return addon.phaseVocoder(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}

/** HPSS into harmonic / percussive / residual signals. */
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
  return addon.hpssWithResidual(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}

/**
 * Channel-weighted multichannel loudness + LRA (BS.1770 / EBU R128) from an
 * interleaved buffer of `frames * channels` samples. The per-channel frame
 * count is derived from the buffer length and `channels`.
 *
 * Pass the buffer's actual `sampleRate`: the default (22050) is non-standard for
 * audio, and K-weighting is sample-rate dependent, so a wrong rate yields wrong
 * loudness.
 */
export function lufsInterleaved(request: FeatureSamplesRequest & { channels: number }): LufsResult;
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate?: number,
): LufsResult;
export function lufsInterleaved(
  samples: Float32Array | (FeatureSamplesRequest & { channels: number }),
  channels = 0,
  sampleRate = 22050,
): LufsResult {
  const request = samples instanceof Float32Array ? { samples, channels, sampleRate } : samples;
  return addon.lufsInterleaved(request.samples, request.channels, request.sampleRate ?? 22050);
}

/**
 * Standards-compliant EBU R128 loudness range (LRA) in LU. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard and K-weighting is
 * sample-rate dependent.
 */
export function ebur128LoudnessRange(request: FeatureSamplesRequest): number;
export function ebur128LoudnessRange(samples: Float32Array, sampleRate?: number): number;
export function ebur128LoudnessRange(
  samples: Float32Array | FeatureSamplesRequest,
  sampleRate = 22050,
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.ebur128LoudnessRange(request.samples, request.sampleRate ?? 22050);
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

export function pitchYin(request: PitchRequest): PitchResult;
export function pitchYin(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
  fillNa?: boolean,
): PitchResult;
export function pitchYin(
  samples: Float32Array | PitchRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.1,
  fillNa = false,
): PitchResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold, fillNa }
      : samples;
  assertSamples('pitchYin', request.samples, true);
  return addon.pitchYin(
    request.samples,
    request.sampleRate ?? 22050,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 65,
    request.fmax ?? 2093,
    request.threshold ?? 0.1,
    request.fillNa ?? false,
  );
}

export function pitchPyin(request: PitchRequest): PitchResult;
export function pitchPyin(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
  fillNa?: boolean,
): PitchResult;
export function pitchPyin(
  samples: Float32Array | PitchRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.1,
  fillNa = false,
): PitchResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold, fillNa }
      : samples;
  assertSamples('pitchPyin', request.samples, true);
  return addon.pitchPyin(
    request.samples,
    request.sampleRate ?? 22050,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 65,
    request.fmax ?? 2093,
    request.threshold ?? 0.1,
    request.fillNa ?? false,
  );
}

/** Segment a host-supplied monophonic F0 track into stable note regions. */
export function noteSegments(request: NoteSegmentsRequest): NoteSegment[] {
  const { config, segmentationThresholdCents, minNoteMs, referenceHz, voicedThreshold } = request;
  const hasFlatTuningOptions =
    segmentationThresholdCents !== undefined ||
    minNoteMs !== undefined ||
    referenceHz !== undefined ||
    voicedThreshold !== undefined;
  if (config !== undefined && hasFlatTuningOptions) {
    throw new RangeError(
      'noteSegments: specify tuning options either flat or in the deprecated config object, not both',
    );
  }

  const baseRequest = {
    f0Hz: request.f0Hz,
    voicedProb: request.voicedProb,
    frameRate: request.frameRate,
  };
  if (config !== undefined) {
    return addon.noteSegments({ ...baseRequest, config });
  }
  if (!hasFlatTuningOptions) {
    return addon.noteSegments(baseRequest);
  }
  return addon.noteSegments({
    ...baseRequest,
    config: { segmentationThresholdCents, minNoteMs, referenceHz, voicedThreshold },
  });
}

// -- Core --

export function hzToMel(hz: number): number {
  return addon.hzToMel(hz);
}

export function melToHz(mel: number): number {
  return addon.melToHz(mel);
}

export function hzToMidi(hz: number): number {
  return addon.hzToMidi(hz);
}

export function midiToHz(midi: number): number {
  return addon.midiToHz(midi);
}

export function hzToNote(hz: number): string {
  return addon.hzToNote(hz);
}

export function noteToHz(note: string): number {
  return addon.noteToHz(note);
}

export function framesToTime(request: { frames: number; sr?: number; hopLength?: number }): number;
export function framesToTime(frames: number, sr?: number, hopLength?: number): number;
export function framesToTime(
  frames: number | { frames: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof frames === 'number' ? { frames, sr, hopLength } : frames;
  return addon.framesToTime(request.frames, request.sr ?? 22050, request.hopLength ?? 512);
}

export function timeToFrames(request: { time: number; sr?: number; hopLength?: number }): number;
export function timeToFrames(time: number, sr?: number, hopLength?: number): number;
export function timeToFrames(
  time: number | { time: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof time === 'number' ? { time, sr, hopLength } : time;
  return addon.timeToFrames(request.time, request.sr ?? 22050, request.hopLength ?? 512);
}

export function framesToSamples(request: {
  frames: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function framesToSamples(frames: number, hopLength?: number, nFft?: number): number;
export function framesToSamples(
  frames: number | { frames: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof frames === 'number' ? { frames, hopLength, nFft } : frames;
  return addon.framesToSamples(request.frames, request.hopLength ?? 512, request.nFft ?? 0);
}

export function samplesToFrames(request: {
  samples: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function samplesToFrames(samples: number, hopLength?: number, nFft?: number): number;
export function samplesToFrames(
  samples: number | { samples: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof samples === 'number' ? { samples, hopLength, nFft } : samples;
  return addon.samplesToFrames(request.samples, request.hopLength ?? 512, request.nFft ?? 0);
}

export function powerToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function powerToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function powerToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.powerToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-10,
    request.topDb ?? 80,
  );
}

export function amplitudeToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function amplitudeToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function amplitudeToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.amplitudeToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-5,
    request.topDb ?? 80,
  );
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToAmplitude(values, ref);
}

export function preemphasis(request: EmphasisRequest): Float32Array;
export function preemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function preemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, coef, zi } : samples;
  return request.zi === undefined
    ? addon.preemphasis(request.samples, request.coef ?? 0.97)
    : addon.preemphasis(request.samples, request.coef ?? 0.97, request.zi);
}

export function deemphasis(request: EmphasisRequest): Float32Array;
export function deemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function deemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, coef, zi } : samples;
  return request.zi === undefined
    ? addon.deemphasis(request.samples, request.coef ?? 0.97)
    : addon.deemphasis(request.samples, request.coef ?? 0.97, request.zi);
}

export function trimSilence(request: TrimSilenceRequest): {
  audio: Float32Array;
  startSample: number;
  endSample: number;
};
export function trimSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): { audio: Float32Array; startSample: number; endSample: number };
export function trimSilence(
  samples: Float32Array | TrimSilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): { audio: Float32Array; startSample: number; endSample: number } {
  const request =
    samples instanceof Float32Array ? { samples, topDb, frameLength, hopLength } : samples;
  return addon.trimSilence(
    request.samples,
    request.topDb ?? 60,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

export function splitSilence(request: TrimSilenceRequest): Int32Array;
export function splitSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): Int32Array;
export function splitSilence(
  samples: Float32Array | TrimSilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  const request =
    samples instanceof Float32Array ? { samples, topDb, frameLength, hopLength } : samples;
  return addon.splitSilence(
    request.samples,
    request.topDb ?? 60,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

/** Generate a sine tone. */
export function tone(request?: ToneRequest): Float32Array;
export function tone(
  frequency?: number,
  sampleRate?: number,
  duration?: number,
  phase?: number,
  amplitude?: number,
): Float32Array;
export function tone(
  frequency: number | ToneRequest = 440,
  sampleRate = 22050,
  duration = 1,
  phase = 0,
  amplitude = 1,
): Float32Array {
  const request =
    typeof frequency === 'number'
      ? { frequency, sampleRate, duration, phase, amplitude }
      : frequency;
  return addon.tone(
    request.frequency ?? 440,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.phase ?? 0,
    request.amplitude ?? 1,
  );
}

/** Generate a linear or exponential chirp. */
export function chirp(request?: ChirpRequest): Float32Array;
export function chirp(
  fmin?: number,
  fmax?: number,
  sampleRate?: number,
  duration?: number,
  linear?: boolean,
): Float32Array;
export function chirp(
  fmin: number | ChirpRequest = 440,
  fmax = 880,
  sampleRate = 22050,
  duration = 1,
  linear = true,
): Float32Array {
  const request = typeof fmin === 'number' ? { fmin, fmax, sampleRate, duration, linear } : fmin;
  return addon.chirp(
    request.fmin ?? 440,
    request.fmax ?? 880,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.linear ?? true,
  );
}

/** Generate a decaying sine click track at times in seconds. */
export function clicks(request: ClicksRequest): Float32Array;
export function clicks(
  times: Float32Array,
  sampleRate?: number,
  length?: number,
  frequency?: number,
  clickDuration?: number,
): Float32Array;
export function clicks(
  times: Float32Array | ClicksRequest,
  sampleRate = 22050,
  length = 0,
  frequency = 1000,
  clickDuration = 0.1,
): Float32Array {
  const request =
    times instanceof Float32Array ? { times, sampleRate, length, frequency, clickDuration } : times;
  return addon.clicks(
    request.times,
    request.sampleRate ?? 22050,
    request.length ?? 0,
    request.frequency ?? 1000,
    request.clickDuration ?? 0.1,
  );
}

export function frameSignal(request: FrameSignalRequest): { nFrames: number; frames: Float32Array };
export function frameSignal(
  samples: Float32Array,
  frameLength?: number,
  hopLength?: number,
): { nFrames: number; frames: Float32Array };
export function frameSignal(
  samples: Float32Array | FrameSignalRequest,
  frameLength = 0,
  hopLength = 0,
): { nFrames: number; frames: Float32Array } {
  const request = samples instanceof Float32Array ? { samples, frameLength, hopLength } : samples;
  return addon.frameSignal(request.samples, request.frameLength, request.hopLength);
}

export function padCenter(
  request: ValuesRequest & { targetSize: number; padValue?: number },
): Float32Array;
export function padCenter(
  values: Float32Array,
  targetSize?: number,
  padValue?: number,
): Float32Array;
export function padCenter(
  values: Float32Array | (ValuesRequest & { targetSize: number; padValue?: number }),
  targetSize = 0,
  padValue = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, targetSize, padValue } : values;
  return addon.padCenter(request.values, request.targetSize, request.padValue ?? 0);
}

export function fixLength(
  request: ValuesRequest & { targetSize: number; padValue?: number },
): Float32Array;
export function fixLength(
  values: Float32Array,
  targetSize?: number,
  padValue?: number,
): Float32Array;
export function fixLength(
  values: Float32Array | (ValuesRequest & { targetSize: number; padValue?: number }),
  targetSize = 0,
  padValue = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, targetSize, padValue } : values;
  return addon.fixLength(request.values, request.targetSize, request.padValue ?? 0);
}

export function fixFrames(request: {
  frames: Int32Array | number[];
  xMin?: number;
  xMax?: number;
  pad?: boolean;
}): Int32Array;
export function fixFrames(
  frames: Int32Array | number[],
  xMin?: number,
  xMax?: number,
  pad?: boolean,
): Int32Array;
export function fixFrames(
  frames:
    | Int32Array
    | number[]
    | { frames: Int32Array | number[]; xMin?: number; xMax?: number; pad?: boolean },
  xMin = 0,
  xMax = -1,
  pad = true,
): Int32Array {
  const request =
    frames instanceof Int32Array || Array.isArray(frames) ? { frames, xMin, xMax, pad } : frames;
  return addon.fixFrames(
    request.frames,
    request.xMin ?? 0,
    request.xMax ?? -1,
    request.pad ?? true,
  );
}

export function onsetBacktrack(request: OnsetBacktrackRequest): Int32Array;
export function onsetBacktrack(events: Int32Array | number[], energy: Float32Array): Int32Array;
export function onsetBacktrack(
  events: Int32Array | number[] | OnsetBacktrackRequest,
  energy?: Float32Array,
): Int32Array {
  const request =
    events instanceof Int32Array || Array.isArray(events) ? { events, energy } : events;
  return addon.onsetBacktrack(request.events, request.energy);
}

export function peakPick(
  request: ValuesRequest & {
    preMax: number;
    postMax: number;
    preAvg: number;
    postAvg: number;
    delta: number;
    wait: number;
  },
): Int32Array;
export function peakPick(
  values: Float32Array,
  preMax: number,
  postMax: number,
  preAvg: number,
  postAvg: number,
  delta: number,
  wait: number,
): Int32Array;
export function peakPick(
  values:
    | Float32Array
    | (ValuesRequest & {
        preMax: number;
        postMax: number;
        preAvg: number;
        postAvg: number;
        delta: number;
        wait: number;
      }),
  preMax = 0,
  postMax = 1,
  preAvg = 0,
  postAvg = 1,
  delta = 0,
  wait = 0,
): Int32Array {
  const request =
    values instanceof Float32Array
      ? { values, preMax, postMax, preAvg, postAvg, delta, wait }
      : values;
  return addon.peakPick(
    request.values,
    request.preMax,
    request.postMax,
    request.preAvg,
    request.postAvg,
    request.delta,
    request.wait,
  );
}

export function vectorNormalize(
  request: ValuesRequest & { normType?: number; threshold?: number },
): Float32Array;
export function vectorNormalize(
  values: Float32Array,
  normType?: number,
  threshold?: number,
): Float32Array;
export function vectorNormalize(
  values: Float32Array | (ValuesRequest & { normType?: number; threshold?: number }),
  normType = 0,
  threshold = 0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, normType, threshold } : values;
  return addon.vectorNormalize(request.values, request.normType ?? 0, request.threshold ?? 0);
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

export function tonnetz(request: {
  chromagram: Float32Array;
  nChroma: number;
  nFrames: number;
}): Float32Array;
export function tonnetz(chromagram: Float32Array, nChroma?: number, nFrames?: number): Float32Array;
export function tonnetz(
  chromagram: Float32Array | { chromagram: Float32Array; nChroma: number; nFrames: number },
  nChroma = 0,
  nFrames = 0,
): Float32Array {
  const request =
    chromagram instanceof Float32Array ? { chromagram, nChroma, nFrames } : chromagram;
  return addon.tonnetz(request.chromagram, request.nChroma, request.nFrames);
}

export function tempogram(request: TempogramRequest): {
  nFrames: number;
  winLength: number;
  data: Float32Array;
};
export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  mode?: TempogramMode,
  center?: boolean,
  norm?: boolean,
): { nFrames: number; winLength: number; data: Float32Array };
export function tempogram(
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
  center = true,
  norm = true,
): { nFrames: number; winLength: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, mode, center, norm }
      : onsetEnvelope;
  return addon.tempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.mode ?? 'autocorrelation',
    request.center ?? true,
    request.norm ?? true,
  );
}

export function cyclicTempogram(request: CyclicTempogramRequest): {
  nFrames: number;
  nBins: number;
  data: Float32Array;
};
export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  center?: boolean,
  norm?: boolean,
  bpmMin?: number,
  nBins?: number,
): { nFrames: number; nBins: number; data: Float32Array };
export function cyclicTempogram(
  onsetEnvelope: Float32Array | CyclicTempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  center = true,
  norm = true,
  bpmMin = 60.0,
  nBins = 60,
): { nFrames: number; nBins: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, center, norm, bpmMin, nBins }
      : onsetEnvelope;
  return addon.cyclicTempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.bpmMin ?? 60,
    request.nBins ?? 60,
  );
}

export function plp(request: PlpRequest): Float32Array;
export function plp(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  tempoMin?: number,
  tempoMax?: number,
  winLength?: number,
): Float32Array;
export function plp(
  onsetEnvelope: Float32Array | PlpRequest,
  sampleRate = 22050,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength }
      : onsetEnvelope;
  return addon.plp(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.tempoMin ?? 30,
    request.tempoMax ?? 300,
    request.winLength ?? 384,
  );
}

export function onsetEnvelope(request: OnsetEnvelopeRequest): Float32Array;
export function onsetEnvelope(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
): Float32Array;
export function onsetEnvelope(
  samples: Float32Array | OnsetEnvelopeRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength, nMels } : samples;
  return addon.onsetEnvelope(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.nMels ?? 128,
  );
}

export function onsetStrengthMulti(request: OnsetStrengthMultiRequest): {
  nBands: number;
  nFrames: number;
  data: Float32Array;
};
export function onsetStrengthMulti(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  nBands?: number,
): { nBands: number; nFrames: number; data: Float32Array };
export function onsetStrengthMulti(
  samples: Float32Array | OnsetStrengthMultiRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nBands = 3,
): { nBands: number; nFrames: number; data: Float32Array } {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, nMels, nBands }
      : samples;
  return addon.onsetStrengthMulti(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.nMels ?? 128,
    request.nBands ?? 3,
  );
}

export function fourierTempogram(request: TempogramRequest): {
  nBins: number;
  nFrames: number;
  data: Float32Array;
};
export function fourierTempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  center?: boolean,
  norm?: boolean,
): { nBins: number; nFrames: number; data: Float32Array };
export function fourierTempogram(
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  center = true,
  norm = true,
): { nBins: number; nFrames: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, center, norm }
      : onsetEnvelope;
  return addon.fourierTempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.center ?? true,
    request.norm ?? true,
  );
}

export function tempogramRatio(request: TempogramRatioRequest): Float32Array;
export function tempogramRatio(
  tempogramData: Float32Array,
  winLength?: number,
  sampleRate?: number,
  hopLength?: number,
  factors?: Float32Array,
): Float32Array;
export function tempogramRatio(
  tempogramData: Float32Array | TempogramRatioRequest,
  winLength = 384,
  sampleRate = 22050,
  hopLength = 512,
  factors?: Float32Array | number[],
): Float32Array {
  const request =
    tempogramData instanceof Float32Array
      ? { tempogramData, winLength, sampleRate, hopLength, factors }
      : tempogramData;
  return addon.tempogramRatio(
    request.tempogramData,
    request.winLength ?? 384,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.factors,
  );
}

export function nnlsChroma(request: NnlsChromaRequest): {
  nChroma: number;
  nFrames: number;
  data: Float32Array;
};
export function nnlsChroma(
  samples: Float32Array,
  sampleRate?: number,
  options?: Omit<NnlsChromaRequest, 'samples' | 'sampleRate'>,
): { nChroma: number; nFrames: number; data: Float32Array };
export function nnlsChroma(
  samples: Float32Array | NnlsChromaRequest,
  sampleRate = 22050,
  options: Omit<NnlsChromaRequest, 'samples' | 'sampleRate'> = {},
): { nChroma: number; nFrames: number; data: Float32Array } {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.nnlsChroma(
    request.samples,
    request.sampleRate ?? 22050,
    request.enableStftBlend ?? true,
    request.stftBlendWeight ?? 0.55,
    request.stftBlendNFft ?? 4096,
    resolvePositiveIntegerOption('nnlsChroma', 'hopLength', request.hopLength, 512),
  );
}

/**
 * Integrated/momentary/short-term LUFS + loudness range. Pass the buffer's
 * actual `sampleRate`: the default (22050) is non-standard for audio, and
 * K-weighting is sample-rate dependent, so a wrong rate yields wrong loudness.
 */
export function lufs(request: LufsRequest): LufsResult;
export function lufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): LufsResult;
export function lufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): LufsResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('lufs', request.samples, request.validate !== false);
  return addon.lufs(request.samples, request.sampleRate ?? 22050);
}

/**
 * Per-block momentary LUFS series. Pass the buffer's actual `sampleRate`: the
 * default (22050) is non-standard and K-weighting is sample-rate dependent.
 */
export function momentaryLufs(request: LufsRequest): Float32Array;
export function momentaryLufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function momentaryLufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('momentaryLufs', request.samples, request.validate !== false);
  return addon.momentaryLufs(request.samples, request.sampleRate ?? 22050);
}

/**
 * Per-block short-term LUFS series. Pass the buffer's actual `sampleRate`: the
 * default (22050) is non-standard and K-weighting is sample-rate dependent.
 */
export function shortTermLufs(request: LufsRequest): Float32Array;
export function shortTermLufs(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function shortTermLufs(
  samples: Float32Array | LufsRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('shortTermLufs', request.samples, request.validate !== false);
  return addon.shortTermLufs(request.samples, request.sampleRate ?? 22050);
}
