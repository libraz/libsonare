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
  PitchResult,
  StftDbResult,
  StftResult,
  TempogramMode,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

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
export interface ChromaRequest extends StftRequest {
  nChroma?: number;
}
export interface CqtRequest extends FeatureSamplesRequest {
  hopLength?: number;
  fmin?: number;
  nBins?: number;
  binsPerOctave?: number;
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
export interface MfccToMelRequest {
  mfcc: Float32Array;
  nMfcc: number;
  nFrames: number;
  nMels?: number;
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
  fillNa?: boolean;
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
}
export interface TempogramRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  winLength?: number;
  mode?: TempogramMode;
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
/** Input for pre/de-emphasis filters. `zi` is the initial delay value. */
export interface EmphasisRequest {
  samples: Float32Array;
  coef?: number;
  zi?: number;
}
/** Input for NNLS chroma extraction. */
export interface NnlsChromaRequest extends FeatureSamplesRequest {}
/** Input for LUFS feature functions, including optional input validation control. */
export interface LufsRequest extends FeatureSamplesRequest, ValidateOptions {}

export function trim(request: FeatureSamplesRequest & { thresholdDb?: number }): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate?: number,
  thresholdDb?: number,
): Float32Array;
export function trim(
  samples: Float32Array | (FeatureSamplesRequest & { thresholdDb?: number }),
  sampleRate = 22050,
  thresholdDb = -60.0,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, thresholdDb } : samples;
  return addon.trim(request.samples, request.sampleRate ?? 22050, request.thresholdDb ?? -60.0);
}

// -- Features --

export function stft(request: StftRequest): StftResult;
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
  samples: Float32Array | ChromaRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
): ChromaResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, hopLength, nChroma } : samples;
  return addon.chromaCens(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.nChroma ?? 12,
  );
}

export function chromaCqt(request: ChromaRequest): ChromaResult;
export function chromaCqt(
  samples: Float32Array | ChromaRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
): ChromaResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, hopLength, nChroma } : samples;
  return addon.chromaCqt(
    request.samples,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.nChroma ?? 12,
  );
}

export function bassChroma(request: ChromaRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array | ChromaRequest,
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

/** Compute the Variable-Q Transform magnitude (`gamma` controls Q). */
export function vqt(request: CqtRequest): CqtResult;
export function vqt(
  samples: Float32Array | CqtRequest,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  gamma = 0.0,
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
    request.gamma ?? 0,
  );
}

/** Reconstruct mono audio from a row-major CQT magnitude via Griffin-Lim. */
export function cqtToAudio(request: CqtToAudioRequest): Float32Array;
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
  magnitude: Float32Array | VqtToAudioRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  binsPerOctave = 12,
  gamma = 0,
  nIter = 32,
): Float32Array {
  const request =
    magnitude instanceof Float32Array
      ? { magnitude, nBins, nFrames, sampleRate, hopLength, fmin, binsPerOctave, gamma, nIter }
      : magnitude;
  return addon.vqtToAudio(
    request.magnitude,
    request.nBins,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.binsPerOctave ?? 12,
    request.gamma ?? 0,
    request.nIter ?? 32,
  );
}

/** Reconstruct STFT power from a mel spectrogram. */
export function melToStft(request: MelToStftRequest): InverseStftResult;
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

/** Reconstruct a mel power spectrogram from MFCCs (`nMels` mel bands). */
export function mfccToMel(request: MfccToMelRequest): InverseMelResult;
export function mfccToMel(
  mfcc: Float32Array | MfccToMelRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
): InverseMelResult {
  const request = mfcc instanceof Float32Array ? { mfcc, nMfcc, nFrames, nMels } : mfcc;
  return addon.mfccToMel(request.mfcc, request.nMfcc, request.nFrames, request.nMels ?? 128);
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
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
): Float32Array {
  const request =
    mfcc instanceof Float32Array
      ? { mfcc, nMfcc, nFrames, nMels, sampleRate, nFft, hopLength, fmin, fmax, nIter, htk }
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
  );
}

export function spectralCentroid(request: StftRequest): Float32Array;
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

/**
 * NMF of a flattened [nFeatures x nFrames] spectrogram (librosa.decompose.decompose).
 *
 * `init` selects the initialiser: `'random'` (default, deterministic seed) or
 * `'nndsvd'` (SVD-based warm start, which tends to converge in fewer iterations).
 */
export function decompose(request: DecomposeRequest): { w: Matrix2D; h: Matrix2D };
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

/** Nearest-neighbour filtering of a flattened [nFeatures x nFrames] spectrogram. */
export function nnFilter(request: NnFilterRequest): Matrix2D;
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

/** Reorder/concatenate a signal by (start,end) interval slices (librosa.effects.remix). */
export function remix(
  request: FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean },
): Float32Array;
export function remix(
  samples: Float32Array | (FeatureSamplesRequest & { intervals: Int32Array; alignZeros?: boolean }),
  intervals = new Int32Array(),
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

/** Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower). */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array | PhaseVocoderRequest,
  rate = Number.NaN,
  sampleRate = 22050,
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
  samples: Float32Array | HpssWithResidualRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
  sampleRate: number;
} {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive }
      : samples;
  return addon.hpssWithResidual(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
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

export function spectralBandwidth(request: StftRequest): Float32Array;
export function spectralBandwidth(
  samples: Float32Array | StftRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength } : samples;
  return addon.spectralBandwidth(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}

export function spectralRolloff(request: StftRequest & { rollPercent?: number }): Float32Array;
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

export function zeroCrossingRate(
  request: FeatureSamplesRequest & { frameLength?: number; hopLength?: number },
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
  samples: Float32Array | PitchRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
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
    request.threshold ?? 0.3,
    request.fillNa ?? false,
  );
}

export function pitchPyin(request: PitchRequest): PitchResult;
export function pitchPyin(
  samples: Float32Array | PitchRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
  fillNa = false,
): PitchResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold, fillNa }
      : samples;
  return addon.pitchPyin(
    request.samples,
    request.sampleRate ?? 22050,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 65,
    request.fmax ?? 2093,
    request.threshold ?? 0.3,
    request.fillNa ?? false,
  );
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

export function frameSignal(request: FrameSignalRequest): { nFrames: number; frames: Float32Array };
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
  postMax = 0,
  preAvg = 0,
  postAvg = 0,
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
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
): { nFrames: number; winLength: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, mode }
      : onsetEnvelope;
  return addon.tempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.mode ?? 'autocorrelation',
  );
}

export function cyclicTempogram(request: CyclicTempogramRequest): {
  nFrames: number;
  nBins: number;
  data: Float32Array;
};
export function cyclicTempogram(
  onsetEnvelope: Float32Array | CyclicTempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  bpmMin = 60.0,
  nBins = 60,
): { nFrames: number; nBins: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, bpmMin, nBins }
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
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
): { nBins: number; nFrames: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength }
      : onsetEnvelope;
  return addon.fourierTempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.winLength ?? 384,
  );
}

export function tempogramRatio(request: TempogramRatioRequest): Float32Array;
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
): { nChroma: number; nFrames: number; data: Float32Array };
export function nnlsChroma(
  samples: Float32Array | NnlsChromaRequest,
  sampleRate = 22050,
): { nChroma: number; nFrames: number; data: Float32Array } {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.nnlsChroma(request.samples, request.sampleRate ?? 22050);
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
