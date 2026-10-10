import { resolvePositiveIntegerOption } from './_feature_options.js';
import { resolveFftOptions } from './_fft_options.js';
import type { FeatureSamplesRequest, StftRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { ChromaResult, CqtResult, NoteSegment, PiptrackResult, PitchResult } from './types.js';
import {
  assertAudioInput,
  assertPositiveInteger,
  requestObject,
  resolveOptionalNonNegative,
} from './validation.js';

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
  /** Cents of pitch change that start a new segment. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest segment kept, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each segment's cents are measured against. Default 440. */
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

export interface PitchTuningRequest {
  frequencies: Float32Array;
  resolution?: number;
  binsPerOctave?: number;
}

/** Input for NNLS chroma extraction. */
export interface NnlsChromaRequest extends FeatureSamplesRequest {
  enableStftBlend?: boolean;
  stftBlendWeight?: number;
  stftBlendNFft?: number;
  hopLength?: number;
}

/**
 * STFT chromagram (librosa.feature.chroma_stft).
 *
 * The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
 * librosa.feature.chroma_stft — which estimates tuning from the signal when none
 * is given — this does NOT auto-estimate and takes no tuning argument. A
 * tuning offset (a fraction of a semitone, which {@link estimateTuning} returns
 * at 12 bins per octave) is applied through `analyze`'s `tuning` option (and to
 * chords through `detectChords`); both also measure it themselves with
 * `tuning: 'auto'`.
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
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength }
      : requestObject('chroma', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('chroma', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('chroma', request.nFft, request.hopLength);
  return addon.chroma(request.samples, resolvedSampleRate, fft.nFft, fft.hopLength);
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
      : requestObject('chromaCens', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('chromaCens', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('chromaCens', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('chromaCens', request.nChroma ?? 12, 'nChroma');
  assertPositiveInteger('chromaCens', request.binsPerOctave ?? 36, 'binsPerOctave');
  return addon.chromaCens(
    request.samples,
    resolvedSampleRate,
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
      : requestObject('chromaCqt', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('chromaCqt', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('chromaCqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('chromaCqt', request.nChroma ?? 12, 'nChroma');
  assertPositiveInteger('chromaCqt', request.binsPerOctave ?? 36, 'binsPerOctave');
  return addon.chromaCqt(
    request.samples,
    resolvedSampleRate,
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
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, nChroma }
      : requestObject('bassChroma', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('bassChroma', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('bassChroma', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('bassChroma', request.nChroma ?? 12, 'nChroma');
  return addon.bassChroma(
    request.samples,
    resolvedSampleRate,
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
      : requestObject('cqt', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('cqt', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('cqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('cqt', request.nBins ?? 84, 'nBins');
  assertPositiveInteger('cqt', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.cqt(
    request.samples,
    resolvedSampleRate,
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
      : requestObject('pseudoCqt', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('pseudoCqt', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('pseudoCqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('pseudoCqt', request.nBins ?? 84, 'nBins');
  assertPositiveInteger('pseudoCqt', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.pseudoCqt(
    request.samples,
    resolvedSampleRate,
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
      : requestObject('hybridCqt', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('hybridCqt', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('hybridCqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('hybridCqt', request.nBins ?? 84, 'nBins');
  assertPositiveInteger('hybridCqt', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.hybridCqt(
    request.samples,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
  );
}

/**
 * Compute VQT magnitude. Omit `gamma` for the automatic ERB-derived value; 0 is
 * the constant-Q transform, and a negative or non-finite `gamma` is refused.
 */
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
  gamma?: number,
): CqtResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, hopLength, fmin, nBins, binsPerOctave, gamma }
      : requestObject('vqt', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('vqt', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('vqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('vqt', request.nBins ?? 84, 'nBins');
  assertPositiveInteger('vqt', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.vqt(
    request.samples,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.nBins ?? 84,
    request.binsPerOctave ?? 12,
    resolveOptionalNonNegative('vqt', request.gamma, 'gamma'),
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
    frequencies instanceof Float32Array
      ? { frequencies, resolution, binsPerOctave }
      : requestObject('pitchTuning', frequencies, 'frequencies');
  assertPositiveInteger('pitchTuning', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.pitchTuning(
    request.frequencies,
    request.resolution ?? 0.01,
    request.binsPerOctave ?? 12,
  );
}

/**
 * Tuning offset of an audio signal (librosa.estimate_tuning).
 *
 * A librosa mirror: the offset is a fraction of a bin of `binsPerOctave`, which
 * is the semitone fraction the analysis options take only at 12 (otherwise
 * multiply by `12 / binsPerOctave`). Analysis measures the semitone fraction
 * itself with `tuning: 'auto'`; {@link tuningToReferenceHz} converts it to Hz.
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
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, resolution, binsPerOctave }
      : requestObject('estimateTuning', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('estimateTuning', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('estimateTuning', request.nFft, request.hopLength);
  assertPositiveInteger('estimateTuning', request.binsPerOctave ?? 12, 'binsPerOctave');
  return addon.estimateTuning(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.resolution ?? 0.01,
    request.binsPerOctave ?? 12,
  );
}

/**
 * Reference frequency of an A4 raised by `tuning` fractions of a semitone:
 * `a4 * 2 ** (tuning / 12)`. The converter from the analysis unit (`tuning`, in
 * `[-0.5, 0.5)` for the analysis options) to the Hz a pitch reference is stated
 * in, such as `referenceHz` and the streaming `tuningRefHz`.
 *
 * @param tuning - Finite fraction of a semitone.
 * @param a4 - Finite positive concert pitch the tuning is measured from. Default 440.
 * @throws `SonareError` for a non-finite `tuning` or a non-positive `a4`.
 */
export function tuningToReferenceHz(tuning: number, a4 = 440): number {
  return addon.tuningToReferenceHz(tuning, a4);
}

/**
 * Tuning, in fractions of a semitone, of a recording whose A4 sits at `hz`:
 * `12 * log2(hz / a4)`. Inverse of {@link tuningToReferenceHz}.
 *
 * @param hz - Finite positive reference frequency.
 * @param a4 - Finite positive concert pitch. Default 440.
 * @throws `SonareError` for a non-positive `hz` or `a4`.
 */
export function referenceHzToTuning(hz: number, a4 = 440): number {
  return addon.referenceHzToTuning(hz, a4);
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
      : requestObject('piptrack', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('piptrack', request.samples, resolvedSampleRate, {});
  const fft = resolveFftOptions('piptrack', request.nFft, request.hopLength);
  return addon.piptrack(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.fmin ?? 150,
    request.fmax ?? 4000,
    request.threshold ?? 0.1,
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
      : requestObject('pitchYin', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('pitchYin', request.samples, resolvedSampleRate, {});
  // frameLength is a framing window, not a transform size, so it carries no evenness rule.
  assertPositiveInteger('pitchYin', request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger('pitchYin', request.hopLength ?? 512, 'hopLength');
  return addon.pitchYin(
    request.samples,
    resolvedSampleRate,
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
      : requestObject('pitchPyin', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('pitchPyin', request.samples, resolvedSampleRate, {});
  assertPositiveInteger('pitchPyin', request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger('pitchPyin', request.hopLength ?? 512, 'hopLength');
  return addon.pitchPyin(
    request.samples,
    resolvedSampleRate,
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
  requestObject('noteSegments', request, 'request', true);
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
    chromagram instanceof Float32Array
      ? { chromagram, nChroma, nFrames }
      : requestObject('tonnetz', chromagram, 'chromagram');
  assertPositiveInteger('tonnetz', request.nChroma, 'nChroma');
  assertPositiveInteger('tonnetz', request.nFrames, 'nFrames');
  return addon.tonnetz(request.chromagram, request.nChroma, request.nFrames);
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
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('nnlsChroma', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('nnlsChroma', request.samples, resolvedSampleRate, request);
  // stftBlendNFft defaults to 4096, not the 2048/512 pair resolveFftOptions assumes.
  assertPositiveInteger('nnlsChroma', request.stftBlendNFft ?? 4096, 'stftBlendNFft');
  return addon.nnlsChroma(
    request.samples,
    resolvedSampleRate,
    request.enableStftBlend ?? true,
    request.stftBlendWeight ?? 0.55,
    request.stftBlendNFft ?? 4096,
    resolvePositiveIntegerOption('nnlsChroma', 'hopLength', request.hopLength, 512),
  );
}
