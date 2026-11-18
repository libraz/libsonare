import { toVoicedInt32 } from './internal/effects_common.js';
import { assertMatrix2D, toMatrix2D } from './internal/feature_matrix.js';
import { resolvePositiveIntegerOption } from './internal/feature_options.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { nativeTuningToCents } from './internal/tuning_units.js';
import { addon } from './native.js';
import type {
  ChromaResult,
  CqtResult,
  Matrix2D,
  NoteSegment,
  PiptrackResult,
  PitchResult,
} from './types.js';
import type { VoicedFlags } from './types_notes.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFrequencyBounds,
  assertPositiveFloatValue,
  assertPositiveInteger,
  requestOnly,
  resolveOptionalNonNegative,
  sampleOneShot,
} from './validation.js';

/** Request for {@link chroma}. `validate: false` skips the NaN/Inf pre-scan. */
export interface ChromaRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Even FFT size in samples. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/**
 * Request for {@link chromaCens}. Constant-Q based, so there is no FFT size to
 * set; use {@link chroma} for the STFT-framed chromagram.
 */
export interface ChromaCensRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Chroma bins. Default 12. */
  nChroma?: number;
  /** Constant-Q bins per octave. Default 36. */
  binsPerOctave?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** Request for {@link chromaCqt}. */
export type ChromaCqtRequest = ChromaCensRequest;

/**
 * Request for {@link bassChroma}. Constant-Q based; the bass chroma's bin count
 * and lowest frequency are chosen together, so the resolution is not settable.
 */
export interface BassChromaRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Chroma bins. Default `12`. */
  nChroma?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/**
 * Compile-time guard for the request shapes above. A field the entry point
 * cannot forward is worse than a missing one: it type-checks, runs, and returns
 * the default silently. These aliases fail to compile if either field comes
 * back, so restoring one has to be a deliberate act.
 */
type AbsentKey<T extends never> = T;
type _ChromaCensRequestHasNoFftSize = AbsentKey<Extract<keyof ChromaCensRequest, 'nFft'>>;
type _BassChromaRequestHasNoResolutionControls = AbsentKey<
  Extract<keyof BassChromaRequest, 'nFft' | 'binsPerOctave'>
>;

/** Request for {@link cqt}. */
export interface CqtRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest bin frequency in Hz. Default C1 (32.70 Hz). */
  fminHz?: number;
  /** Number of CQT bins. Default 84. */
  nBins?: number;
  /** Bins per octave. Default 12. */
  binsPerOctave?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** Request for {@link pseudoCqt}. */
export type PseudoCqtRequest = CqtRequest;

/** Request for {@link hybridCqt}. */
export type HybridCqtRequest = CqtRequest;

/** Request for {@link vqt}. */
export interface VqtRequest extends CqtRequest {
  /** Bandwidth offset. Absent selects the automatic ERB-derived value; 0 is the constant-Q transform. */
  gamma?: number;
}

/** Request for {@link estimateTuning}. */
export interface EstimateTuningRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Search resolution as a fraction of a bin. Default 0.01. */
  resolution?: number;
  /** Bins per octave of the tuning grid. Default `12`. */
  binsPerOctave?: number;
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** Request for {@link pitchYin}. */
export interface PitchYinRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Framing window in samples. Default 2048. */
  frameLength?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest f0 searched, in Hz. Default 65. */
  f0MinHz?: number;
  /** Highest f0 searched, in Hz. Default 2093. */
  f0MaxHz?: number;
  /** YIN threshold in [0, 1] for voiced/unvoiced decisions. Default `0.1`. */
  threshold?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** Request for {@link pitchPyin}. */
export interface PitchPyinRequest extends PitchYinRequest {
  /** Return 0 for unvoiced f0 frames instead of NaN. Default `false`. */
  fillNa?: boolean;
}

/** Request for {@link piptrack}. */
export interface PiptrackRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest frequency considered, in Hz. Default 150. */
  fminHz?: number;
  /** Highest frequency considered, in Hz. Default 4000. */
  fmaxHz?: number;
  /** Magnitude threshold, as a fraction of the frame's maximum, below which a peak is dropped. Default `0.1`. */
  threshold?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** Request for {@link noteSegments}. Give `voiced` or `voicedProb`; `voiced` takes precedence. */
export interface NoteSegmentsRequest {
  /** Per-frame F0 track in Hz, one value per frame, at `frameRate` frames per second. */
  f0Hz: Float32Array;
  /**
   * Per-frame voicing values in `[0, 1]`; anything below `voicedThreshold` is
   * unvoiced.
   *
   * Read only when `voiced` is omitted. Pass `pitchPyin`'s `voicedFlag` as
   * `voiced` instead. Do **not** pass its `voicedProb`: that value is the
   * frame's voiced observation mass and rises with F0 for a fixed `frameLength`,
   * so a fixed threshold silently returns no segments at all for low-register
   * material.
   */
  voicedProb?: Float32Array;
  /**
   * Per-frame voiced flags (truthy = voiced), the same length as `f0Hz`. Takes
   * precedence over `voicedProb`; `voicedThreshold` does not apply.
   */
  voiced?: VoicedFlags;
  /** Frames per second of the F0 track. */
  frameRate: number;
  /** Cents of pitch change that start a new segment. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest segment kept, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each segment's cents are measured against, in Hz. Default 440. */
  referenceHz?: number;
  /** Voicing threshold applied to `voicedProb`; defaults to `0.5`. */
  voicedThreshold?: number;
}

/** Request for {@link pitchTuning}. */
export interface PitchTuningRequest {
  /** Frequencies in Hz to estimate the tuning offset from. */
  frequencies: Float32Array;
  /** Search resolution as a fraction of a bin. Default 0.01. */
  resolution?: number;
  /** Bins per octave of the tuning grid. Default `12`. */
  binsPerOctave?: number;
}

/** Request for {@link tonnetz}. */
export interface TonnetzRequest {
  /** Chromagram: rows = chroma bins, cols = frames. */
  chromagram: Matrix2D;
}

/** Request for {@link nnlsChroma}. */
export interface NnlsChromaRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Blend an STFT chroma into the NNLS chroma. Default `true`. */
  enableStftBlend?: boolean;
  /** Weight of the blended STFT chroma. Default `0.55`. */
  stftBlendWeight?: number;
  /** FFT size in samples of the blended STFT chroma. Default `4096`. */
  stftBlendNFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** `false` skips the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

interface NativeChroma {
  sampleRate: number;
  hopLength: number;
  nChroma: number;
  nFrames: number;
  features: Float32Array;
  meanEnergy: number[];
}

function toChromaResult(r: NativeChroma): ChromaResult {
  return {
    sampleRate: r.sampleRate,
    hopLength: r.hopLength,
    features: toMatrix2D(r.features, r.nChroma, r.nFrames),
    meanEnergy: r.meanEnergy,
  };
}

interface NativeCqt {
  nBins: number;
  nFrames: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  frequencies: Float32Array;
}

function toCqtResult(r: NativeCqt): CqtResult {
  return {
    hopLength: r.hopLength,
    sampleRate: r.sampleRate,
    magnitude: toMatrix2D(r.magnitude, r.nBins, r.nFrames),
    frequencies: r.frequencies,
  };
}

const DEFAULT_CQT_FMIN_HZ = 32.70319566257483;

// Shared guard of the CQT family: audio, then the three counts every entry point refuses at zero.
function checkCqtRequest(
  fnName: 'cqt' | 'pseudoCqt' | 'hybridCqt' | 'vqt',
  request: CqtRequest,
): void {
  assertAudioInput(fnName, request.samples, request.sampleRate, request);
  assertPositiveInteger(fnName, request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger(fnName, request.nBins ?? 84, 'nBins');
  assertPositiveInteger(fnName, request.binsPerOctave ?? 12, 'binsPerOctave');
  assertPositiveFloatValue(fnName, request.fminHz ?? DEFAULT_CQT_FMIN_HZ, 'fminHz');
}

/**
 * STFT chromagram (librosa.feature.chroma_stft). Returns `features` as rows =
 * chroma bins, cols = frames.
 *
 * The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
 * librosa.feature.chroma_stft — which estimates tuning from the signal when none
 * is given — this does NOT auto-estimate and takes no tuning argument. A tuning
 * offset in cents ({@link estimateTuning} returns one) is applied through
 * `analyze`'s `tuningCents` option (and to chords through `detectChords`); both
 * also measure it themselves with `tuningCents: 'auto'`.
 */
export function chroma(request: ChromaRequest): ChromaResult;
export function chroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaRequest>,
): ChromaResult;
export function chroma(...args: unknown[]): ChromaResult {
  const request = sampleOneShot<ChromaRequest>('chroma', args);
  assertAudioInput('chroma', request.samples, request.sampleRate, request);
  const fft = resolveFftOptions('chroma', request.nFft, request.hopLength);
  return toChromaResult(addon.chroma(request.samples, request.sampleRate, fft.nFft, fft.hopLength));
}

/** CENS chromagram. Returns `features` as rows = chroma bins, cols = frames. */
export function chromaCens(request: ChromaCensRequest): ChromaResult;
export function chromaCens(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaCensRequest>,
): ChromaResult;
export function chromaCens(...args: unknown[]): ChromaResult {
  const request = sampleOneShot<ChromaCensRequest>('chromaCens', args);
  assertAudioInput('chromaCens', request.samples, request.sampleRate, request);
  assertPositiveInteger('chromaCens', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('chromaCens', request.nChroma ?? 12, 'nChroma');
  assertPositiveInteger('chromaCens', request.binsPerOctave ?? 36, 'binsPerOctave');
  return toChromaResult(
    addon.chromaCens(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.nChroma ?? 12,
      request.binsPerOctave ?? 36,
    ),
  );
}

/** Constant-Q chromagram. Returns `features` as rows = chroma bins, cols = frames. */
export function chromaCqt(request: ChromaCqtRequest): ChromaResult;
export function chromaCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaCqtRequest>,
): ChromaResult;
export function chromaCqt(...args: unknown[]): ChromaResult {
  const request = sampleOneShot<ChromaCqtRequest>('chromaCqt', args);
  assertAudioInput('chromaCqt', request.samples, request.sampleRate, request);
  assertPositiveInteger('chromaCqt', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('chromaCqt', request.nChroma ?? 12, 'nChroma');
  assertPositiveInteger('chromaCqt', request.binsPerOctave ?? 36, 'binsPerOctave');
  return toChromaResult(
    addon.chromaCqt(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.nChroma ?? 12,
      request.binsPerOctave ?? 36,
    ),
  );
}

/** Bass-focused chromagram. Returns `features` as rows = chroma bins, cols = frames. */
export function bassChroma(request: BassChromaRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<BassChromaRequest>,
): ChromaResult;
export function bassChroma(...args: unknown[]): ChromaResult {
  const request = sampleOneShot<BassChromaRequest>('bassChroma', args);
  assertAudioInput('bassChroma', request.samples, request.sampleRate, request);
  assertPositiveInteger('bassChroma', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('bassChroma', request.nChroma ?? 12, 'nChroma');
  return toChromaResult(
    addon.bassChroma(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.nChroma ?? 12,
    ),
  );
}

/** Constant-Q transform magnitude. `magnitude` is rows = CQT bins, cols = frames. */
export function cqt(request: CqtRequest): CqtResult;
export function cqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<CqtRequest>,
): CqtResult;
export function cqt(...args: unknown[]): CqtResult {
  const request = sampleOneShot<CqtRequest>('cqt', args);
  checkCqtRequest('cqt', request);
  return toCqtResult(
    addon.cqt(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.fminHz ?? DEFAULT_CQT_FMIN_HZ,
      request.nBins ?? 84,
      request.binsPerOctave ?? 12,
    ),
  );
}

/** Faster pseudo-CQT magnitude approximation. `magnitude` is rows = CQT bins, cols = frames. */
export function pseudoCqt(request: PseudoCqtRequest): CqtResult;
export function pseudoCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PseudoCqtRequest>,
): CqtResult;
export function pseudoCqt(...args: unknown[]): CqtResult {
  const request = sampleOneShot<PseudoCqtRequest>('pseudoCqt', args);
  checkCqtRequest('pseudoCqt', request);
  return toCqtResult(
    addon.pseudoCqt(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.fminHz ?? DEFAULT_CQT_FMIN_HZ,
      request.nBins ?? 84,
      request.binsPerOctave ?? 12,
    ),
  );
}

/** Hybrid CQT magnitude. `magnitude` is rows = CQT bins, cols = frames. */
export function hybridCqt(request: HybridCqtRequest): CqtResult;
export function hybridCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HybridCqtRequest>,
): CqtResult;
export function hybridCqt(...args: unknown[]): CqtResult {
  const request = sampleOneShot<HybridCqtRequest>('hybridCqt', args);
  checkCqtRequest('hybridCqt', request);
  return toCqtResult(
    addon.hybridCqt(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.fminHz ?? DEFAULT_CQT_FMIN_HZ,
      request.nBins ?? 84,
      request.binsPerOctave ?? 12,
    ),
  );
}

/**
 * VQT magnitude. Omit `gamma` for the automatic ERB-derived value; 0 is the
 * constant-Q transform, and a negative or non-finite `gamma` is refused.
 * `magnitude` is rows = VQT bins, cols = frames.
 */
export function vqt(request: VqtRequest): CqtResult;
export function vqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<VqtRequest>,
): CqtResult;
export function vqt(...args: unknown[]): CqtResult {
  const request = sampleOneShot<VqtRequest>('vqt', args);
  checkCqtRequest('vqt', request);
  return toCqtResult(
    addon.vqt(
      request.samples,
      request.sampleRate,
      request.hopLength ?? 512,
      request.fminHz ?? DEFAULT_CQT_FMIN_HZ,
      request.nBins ?? 84,
      request.binsPerOctave ?? 12,
      resolveOptionalNonNegative('vqt', request.gamma, 'gamma'),
    ),
  );
}

/** Global tuning offset, in cents, from a set of frequencies (librosa.pitch_tuning). */
export function pitchTuning(request: PitchTuningRequest): number;
export function pitchTuning(...args: unknown[]): number {
  const r = requestOnly<PitchTuningRequest>('pitchTuning', args);
  const binsPerOctave = r.binsPerOctave ?? 12;
  assertPositiveInteger('pitchTuning', binsPerOctave, 'binsPerOctave');
  return nativeTuningToCents(
    addon.pitchTuning(r.frequencies, r.resolution ?? 0.01, binsPerOctave),
    binsPerOctave,
  );
}

/**
 * Tuning offset of an audio signal, in cents (librosa.estimate_tuning).
 *
 * Analysis measures the same quantity itself with `tuningCents: 'auto'`;
 * {@link tuningToReferenceHz} converts it to Hz.
 */
export function estimateTuning(request: EstimateTuningRequest): number;
export function estimateTuning(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<EstimateTuningRequest>,
): number;
export function estimateTuning(...args: unknown[]): number {
  const request = sampleOneShot<EstimateTuningRequest>('estimateTuning', args);
  assertAudioInput('estimateTuning', request.samples, request.sampleRate, request);
  const fft = resolveFftOptions('estimateTuning', request.nFft, request.hopLength);
  const binsPerOctave = request.binsPerOctave ?? 12;
  assertPositiveInteger('estimateTuning', binsPerOctave, 'binsPerOctave');
  return nativeTuningToCents(
    addon.estimateTuning(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.resolution ?? 0.01,
      binsPerOctave,
    ),
    binsPerOctave,
  );
}

/**
 * Reference frequency of an A4 raised by `tuningCents`: `a4Hz * 2 ** (tuningCents / 1200)`.
 * The converter from the analysis unit (`tuningCents`) to the Hz a pitch
 * reference is stated in, such as `referenceHz`.
 *
 * @param tuningCents - Finite offset in cents.
 * @param a4Hz - Finite positive concert pitch the tuning is measured from. Default 440.
 * @throws `SonareError` for a non-finite `tuningCents` or a non-positive `a4Hz`.
 */
export function tuningToReferenceHz(tuningCents: number, a4Hz = 440): number {
  return addon.tuningToReferenceHz(tuningCents, a4Hz);
}

/**
 * Tuning, in cents, of a recording whose A4 sits at `referenceHz`:
 * `1200 * log2(referenceHz / a4Hz)`. Inverse of {@link tuningToReferenceHz}.
 *
 * @param referenceHz - Finite positive reference frequency.
 * @param a4Hz - Finite positive concert pitch. Default 440.
 * @throws `SonareError` for a non-positive `referenceHz` or `a4Hz`.
 */
export function referenceHzToTuning(referenceHz: number, a4Hz = 440): number {
  return addon.referenceHzToTuning(referenceHz, a4Hz);
}

/**
 * Per-bin spectral pitch candidates and their peak magnitudes (librosa.piptrack).
 * Both matrices are rows = FFT bins, cols = frames.
 */
export function piptrack(request: PiptrackRequest): PiptrackResult;
export function piptrack(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PiptrackRequest>,
): PiptrackResult;
export function piptrack(...args: unknown[]): PiptrackResult {
  const request = sampleOneShot<PiptrackRequest>('piptrack', args);
  assertAudioInput('piptrack', request.samples, request.sampleRate, request);
  const fft = resolveFftOptions('piptrack', request.nFft, request.hopLength);
  const fminHz = request.fminHz ?? 150;
  const fmaxHz = request.fmaxHz ?? 4000;
  assertFrequencyBounds('piptrack', fminHz, fmaxHz, 'fminHz', 'fmaxHz');
  const r = addon.piptrack(
    request.samples,
    request.sampleRate,
    fft.nFft,
    fft.hopLength,
    fminHz,
    fmaxHz,
    request.threshold ?? 0.1,
  );
  return {
    pitches: toMatrix2D(r.pitches, r.nBins, r.nFrames),
    magnitudes: toMatrix2D(r.magnitudes, r.nBins, r.nFrames),
  };
}

function pitchTrack(fnName: 'pitchYin' | 'pitchPyin', args: unknown[]): PitchResult {
  const request = sampleOneShot<PitchPyinRequest>(fnName, args);
  assertAudioInput(fnName, request.samples, request.sampleRate, request);
  // frameLength is a framing window, not a transform size, so it carries no evenness rule.
  assertPositiveInteger(fnName, request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger(fnName, request.hopLength ?? 512, 'hopLength');
  const f0MinHz = request.f0MinHz ?? 65;
  const f0MaxHz = request.f0MaxHz ?? 2093;
  assertFrequencyBounds(fnName, f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const positional = [
    request.samples,
    request.sampleRate,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
    f0MinHz,
    f0MaxHz,
    request.threshold ?? 0.1,
  ] as const;
  // Only pYIN takes a fill flag; the YIN request refuses fillNa as an unknown key.
  const native: PitchResult & { nFrames: number } =
    fnName === 'pitchPyin'
      ? addon.pitchPyin(...positional, request.fillNa ?? false)
      : addon.pitchYin(...positional);
  // The frame count is the length of every per-frame array; the result does not repeat it.
  const { nFrames: _nFrames, ...result } = native;
  return result;
}

/** YIN fundamental-frequency track. */
export function pitchYin(request: PitchYinRequest): PitchResult;
export function pitchYin(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PitchYinRequest>,
): PitchResult;
export function pitchYin(...args: unknown[]): PitchResult {
  return pitchTrack('pitchYin', args);
}

/** pYIN fundamental-frequency track. */
export function pitchPyin(request: PitchPyinRequest): PitchResult;
export function pitchPyin(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PitchPyinRequest>,
): PitchResult;
export function pitchPyin(...args: unknown[]): PitchResult {
  return pitchTrack('pitchPyin', args);
}

/** Segment a host-supplied monophonic F0 track into stable note regions. */
export function noteSegments(request: NoteSegmentsRequest): NoteSegment[];
export function noteSegments(...args: unknown[]): NoteSegment[] {
  const r = requestOnly<NoteSegmentsRequest>('noteSegments', args);
  const { segmentationThresholdCents, minNoteMs, referenceHz, voicedThreshold } = r;
  const hasTuningOptions =
    segmentationThresholdCents !== undefined ||
    minNoteMs !== undefined ||
    referenceHz !== undefined ||
    voicedThreshold !== undefined;
  if (r.voiced == null && r.voicedProb == null) {
    throw new TypeError('noteSegments: one of voiced and voicedProb is required');
  }
  // The native reader takes 0/1 bytes; voiced wins, so voicedProb is not sent beside it.
  const flags = r.voiced != null ? Uint8Array.from(toVoicedInt32('noteSegments', r.voiced)) : null;
  const prob = flags === null ? r.voicedProb : null;
  const voicing = flags ?? prob;
  if (
    ArrayBuffer.isView(r.f0Hz) &&
    ArrayBuffer.isView(voicing) &&
    r.f0Hz.length !== voicing.length
  ) {
    throw new RangeError(
      `noteSegments: ${flags ? 'voiced' : 'voicedProb'} must have the same length as f0Hz`,
    );
  }
  return addon.noteSegments(
    r.f0Hz,
    prob ?? null,
    flags,
    r.frameRate,
    hasTuningOptions
      ? { segmentationThresholdCents, minNoteMs, referenceHz, voicedThreshold }
      : undefined,
  );
}

/** Tonal centroid features of a chromagram. Returns rows = 6 tonnetz dims, cols = frames. */
export function tonnetz(request: TonnetzRequest): Matrix2D;
export function tonnetz(...args: unknown[]): Matrix2D {
  const r = requestOnly<TonnetzRequest>('tonnetz', args);
  assertMatrix2D('tonnetz', r.chromagram, 'chromagram');
  const out = addon.tonnetz(r.chromagram.data, r.chromagram.rows, r.chromagram.cols);
  return toMatrix2D(out, 6, r.chromagram.cols);
}

/** NNLS chroma. Returns rows = 12 chroma bins, cols = frames. */
export function nnlsChroma(request: NnlsChromaRequest): Matrix2D;
export function nnlsChroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<NnlsChromaRequest>,
): Matrix2D;
export function nnlsChroma(...args: unknown[]): Matrix2D {
  const request = sampleOneShot<NnlsChromaRequest>('nnlsChroma', args);
  assertAudioInput('nnlsChroma', request.samples, request.sampleRate, request);
  // stftBlendNFft defaults to 4096, not the 2048/512 pair resolveFftOptions assumes.
  assertPositiveInteger('nnlsChroma', request.stftBlendNFft ?? 4096, 'stftBlendNFft');
  const r = addon.nnlsChroma(
    request.samples,
    request.sampleRate,
    request.enableStftBlend ?? true,
    request.stftBlendWeight ?? 0.55,
    request.stftBlendNFft ?? 4096,
    resolvePositiveIntegerOption('nnlsChroma', 'hopLength', request.hopLength, 512),
  );
  return toMatrix2D(r.data, r.nChroma, r.nFrames);
}
