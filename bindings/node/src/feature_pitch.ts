import { resolvePositiveIntegerOption } from './_feature_options.js';
import { resolveFftOptions } from './_fft_options.js';
import type { FeatureSamplesRequest, StftRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { ChromaResult, CqtResult, NoteSegment, PiptrackResult, PitchResult } from './types.js';
import { assertPositiveInteger, assertSampleRate, assertSamples } from './validation.js';

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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('chroma', resolvedSampleRate);
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
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('chromaCens', resolvedSampleRate);
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
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('chromaCqt', resolvedSampleRate);
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
    samples instanceof Float32Array ? { samples, sampleRate, hopLength, nChroma } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('bassChroma', resolvedSampleRate);
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
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('cqt', resolvedSampleRate);
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
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('pseudoCqt', resolvedSampleRate);
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
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('hybridCqt', resolvedSampleRate);
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

/** Compute VQT magnitude (a negative or NaN `gamma` selects the automatic ERB-derived value). */
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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('vqt', resolvedSampleRate);
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
    request.gamma ?? -1,
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
  assertPositiveInteger('pitchTuning', request.binsPerOctave ?? 12, 'binsPerOctave');
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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('estimateTuning', resolvedSampleRate);
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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('piptrack', resolvedSampleRate);
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
      : samples;
  assertSamples('pitchYin', request.samples, true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('pitchYin', resolvedSampleRate);
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
      : samples;
  assertSamples('pitchPyin', request.samples, true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('pitchPyin', resolvedSampleRate);
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
    chromagram instanceof Float32Array ? { chromagram, nChroma, nFrames } : chromagram;
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('nnlsChroma', resolvedSampleRate);
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
