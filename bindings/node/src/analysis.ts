import { resolveFftOptions } from './_fft_options.js';
import { addon } from './native.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisProgressCallback,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  BoundaryOptions,
  BoundaryResult,
  BpmAnalysisResult,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  ChordFunction,
  ChordFunctionsInput,
  ChordFunctionsKey,
  DetectKeyOptions,
  DynamicsResult,
  FunctionalChord,
  KeyCandidate,
  KeyDetection,
  KeyDetectionOptions,
  MelodyOptions,
  MelodyResult,
  MeterEstimate,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryFromEstimateOptions,
  RoomGeometryOptions,
  RoomMorphOptions,
  RoomMorphResult,
  Section,
  TimbreResult,
} from './types.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertInt32,
  assertInt64,
  assertNonNegativeSafeInteger,
  assertPositiveInteger,
  assertSampleRate,
  requestObject,
} from './validation.js';

/**
 * Check the room-option fields whose zero requests the library default, before
 * the addon narrows them. Truncation lands a fractional value on that zero, so
 * the call runs at the default and reports success.
 *
 * `materialPreset` is deliberately absent: it is a name, and its `'none'` is a
 * mode of its own rather than a stand-in for an absent value. So is
 * {@link analyzeImpulseResponse}'s `nOctaveBands`, which is passed through as a
 * literal band count and analyses no bands at zero.
 */
function assertRoomOptions(
  fnName: string,
  options: { seed?: number; nOctaveBands?: number },
): void {
  if (options.seed !== undefined) {
    assertInt64(fnName, options.seed, 'seed');
  }
  if (options.nOctaveBands !== undefined) {
    assertInt32(fnName, options.nOctaveBands, 'nOctaveBands');
  }
}

export interface SamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

/** Peak-picking configuration for {@link detectOnsets}. */
export interface OnsetDetectOptions {
  nFft?: number;
  hopLength?: number;
  threshold?: number;
  preMax?: number;
  postMax?: number;
  preAvg?: number;
  postAvg?: number;
  delta?: number;
  wait?: number;
  backtrack?: boolean;
  backtrackRange?: number;
}
export interface DetectOnsetsRequest extends SamplesRequest, OnsetDetectOptions {}

export interface DetectKeyRequest extends DetectKeyOptions, SamplesRequest {}
export interface DetectKeyCandidatesRequest extends KeyDetectionOptions, SamplesRequest {}

export interface RoomEstimateRequest extends RoomEstimateOptions, SamplesRequest {}
export interface RoomMorphRequest extends RoomMorphOptions, SamplesRequest {}
/** The room fields of an estimate that {@link roomGeometryFromEstimate} reads. */
export type RoomGeometryEstimate = Pick<RoomEstimateResult, 'lengthM' | 'widthM' | 'heightM'> & {
  bandAbsorption?: Float32Array | number[];
};
export interface RoomGeometryFromEstimateRequest extends RoomGeometryFromEstimateOptions {
  estimate: RoomGeometryEstimate;
}
export interface ChordFunctionsRequest<T extends ChordFunctionsInput = Chord> {
  /** `detectChords`' timed result or its `chords` array, or the same shape built by hand. */
  chords: readonly T[] | { chords: readonly T[] };
  key: ChordFunctionsKey;
}

export interface AnalyzeWithProgressRequest extends SamplesRequest, MusicAnalyzeOptions {
  onProgress?: AnalysisProgressCallback;
  cancel?: () => boolean;
  /**
   * Analysis options, with the same fields and defaults {@link analyze} takes.
   * Equivalent to setting the same fields directly on this request object (the
   * flattened form {@link analyze}/{@link analyzeAsync} take); a field set both
   * ways takes the value set here.
   */
  options?: MusicAnalyzeOptions;
}

export interface MusicAnalyzeOptions {
  nFft?: number;
  hopLength?: number;
  bpmMin?: number;
  bpmMax?: number;
  startBpm?: number;
  useTriadsOnly?: boolean;
  useHpss?: boolean;
  chromaHighpassHz?: number;
  useBassWeighted?: boolean;
  chromaHopMultiplier?: number;
  useChordHmm?: boolean;
  useChordKeyContext?: boolean;
  chordHmmBeamWidth?: number;
  detectChordInversions?: boolean;
  /** Track a locally updated tempo prior through beat tracking. Default false. */
  adaptiveTempo?: boolean;
  /**
   * Local tempo context length in beats, used only when `adaptiveTempo` is
   * true. Must be positive. Default 8.
   */
  tempoUpdateIntervalBeats?: number;
  /**
   * Decode a per-beat local tempo curve into the result's `beatLocalBpm`.
   *
   * Off by default because it is an extra output rather than a better analysis:
   * nothing else in the result changes, and a caller that does not read the
   * curve would pay a decode over the beat grid for nothing.
   *
   * The curve describes the beat grid it was decoded from, and beat tracking
   * holds a fixed tempo prior unless `adaptiveTempo` is also set, so measuring
   * a tempo that moves needs both. Default false.
   */
  computeTempoCurve?: boolean;
  /**
   * Meter numerators the estimator scores. At most 16 entries, each in
   * `[2, 32]`; an empty list is rejected rather than restoring the default.
   * Widening the set does not force a wider meter. Default `[3, 4, 6]`.
   */
  meterCandidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter; a power of two in `[1, 32]`.
   * The estimator still reports 8 on its own when it resolves a compound
   * meter. Default 4.
   */
  meterDenominator?: number;
  /**
   * Tuning offset of the recording in fractions of a semitone; must be in
   * `[-0.5, 0.5)`. Every chroma the analysis builds (key, chords, sections) is
   * shifted by it, so a recording that is not at A440 reads its key and chords
   * on its own pitch grid. `'auto'` measures it from the audio. The value used,
   * given or measured, is the result's `tuning`; {@link referenceHzToTuning}
   * converts a reference pitch to this unit. Default 0 (concert A440).
   */
  tuning?: number | 'auto';
}
export interface MusicAnalyzeRequest extends SamplesRequest, MusicAnalyzeOptions {}

/**
 * Every {@link MusicAnalyzeOptions} key, kept in sync with the interface by
 * the type checker: `Record<keyof MusicAnalyzeOptions, true>` fails to
 * compile if this object's keys and that interface's disagree in either
 * direction.
 */
const MUSIC_ANALYZE_OPTION_KEYS: Record<keyof MusicAnalyzeOptions, true> = {
  nFft: true,
  hopLength: true,
  bpmMin: true,
  bpmMax: true,
  startBpm: true,
  useTriadsOnly: true,
  useHpss: true,
  chromaHighpassHz: true,
  useBassWeighted: true,
  chromaHopMultiplier: true,
  useChordHmm: true,
  useChordKeyContext: true,
  chordHmmBeamWidth: true,
  detectChordInversions: true,
  adaptiveTempo: true,
  tempoUpdateIntervalBeats: true,
  computeTempoCurve: true,
  meterCandidateNumerators: true,
  meterDenominator: true,
  tuning: true,
};
const MUSIC_ANALYZE_OPTION_KEY_LIST = Object.keys(
  MUSIC_ANALYZE_OPTION_KEYS,
) as (keyof MusicAnalyzeOptions)[];

/**
 * Pulls MusicAnalyzeOptions fields spread directly onto a request object
 * (`analyze`/`analyzeAsync`'s flattened `MusicAnalyzeRequest` shape) rather
 * than nested under `options` (`analyzeWithProgress`'s own documented shape).
 * A request built for one is structurally assignable to the other -- every
 * extra field either carries is optional on the other's type -- so a caller
 * reusing one request object across both must not have either shape's fields
 * silently dropped.
 */
function flattenedMusicAnalyzeOptions(request: MusicAnalyzeOptions): MusicAnalyzeOptions {
  const out: MusicAnalyzeOptions = {};
  for (const key of MUSIC_ANALYZE_OPTION_KEY_LIST) {
    if (request[key] !== undefined) {
      (out as Record<string, unknown>)[key] = request[key];
    }
  }
  return out;
}

/** Request for {@link estimateMeter}. */
export interface EstimateMeterRequest {
  /** Beat positions in seconds, non-decreasing. */
  beatTimes: ArrayLike<number>;
  /**
   * Per-beat accent value, the same length as `beatTimes`. `AnalysisResult`'s
   * `beatObservations.onsetStrength` is the intended source; `beats[].strength`
   * also works but is a single unwindowed envelope frame. Neither needs
   * pre-scaling: the series is divided by its own maximum before scoring, so
   * only the accent contrast within it is read.
   *
   * A series assembled by hand from `onsetEnvelope` — one frame read at each
   * beat time — is neither of those, and it carries a sample-rate dependence
   * neither of them has: a hop counted in samples frames a different amount of
   * time at each rate, so one waveform sampled at 32000, 44100 and 48000 Hz has
   * produced three different winning numerators off beat times identical to the
   * sample. Widening the read to a window around the beat does not remove it.
   */
  beatStrengths: ArrayLike<number>;
  /**
   * Meter numerators to score. At most 16 entries, each in `[2, 32]`; an empty
   * list is rejected rather than restoring the default. Widening the set does
   * not force a wider meter. Default `[3, 4, 6]`.
   */
  candidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter; a power of two in `[1, 32]`.
   * Reported as requested: whether a beat divides into three is measured from
   * energy *between* the beats, which per-beat accents do not carry, so a
   * compound meter is not resolvable here — a six accented 3+3 comes back with
   * this denominator and `grouping === [3, 3]`. Default 4.
   */
  denominator?: number;
  /** Weight on the accent at each measure's first beat. Default 1. */
  downbeatWeight?: number;
  /** Weight on measure-to-measure accent agreement. Default 0.5. */
  measureWeight?: number;
  /** Weight on the subdivision accent pattern. Default 0.15. */
  subdivisionWeight?: number;
  /**
   * Subdivision score at which a 6 candidate is reported as compound (x/8).
   * Only consulted when there is a subdivision to measure, so it has no effect
   * on {@link estimateMeter}, which scores per-beat accents alone. Default 0.85.
   */
  compoundSubdivisionThreshold?: number;
}

export interface AnalyzeSectionsRequest extends AnalyzeSectionsOptions, SamplesRequest {}
export interface DetectBoundariesRequest extends BoundaryOptions, SamplesRequest {}
export interface AnalyzeMelodyRequest extends MelodyOptions, SamplesRequest {}
export interface AnalyzeBpmRequest extends AnalyzeBpmOptions, SamplesRequest {}
export interface AnalyzeRhythmRequest extends AnalyzeRhythmOptions, SamplesRequest {}
export interface AnalyzeDynamicsRequest extends AnalyzeDynamicsOptions, SamplesRequest {}
export interface AnalyzeImpulseResponseRequest extends SamplesRequest {
  nOctaveBands?: number;
  minDecayDb?: number;
}
export interface DetectAcousticRequest extends AcousticOptions, SamplesRequest {}
export interface AnalyzeTimbreRequest extends AnalyzeTimbreOptions, SamplesRequest {}
export interface DetectChordsRequest extends ChordDetectionOptions, SamplesRequest {}
export interface ChordFunctionalAnalysisRequest extends ChordDetectionOptions, SamplesRequest {
  keyRoot: ChordFunctionsKey['root'];
  keyMode?: ChordFunctionsKey['mode'];
}

export function detectBpm(request: SamplesRequest): number;
export function detectBpm(samples: Float32Array, sampleRate?: number): number;
export function detectBpm(samples: Float32Array | SamplesRequest, sampleRate = 22050): number {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate } : requestObject('detectBpm', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectBpm', request.samples, resolvedSampleRate, request);
  return addon.detectBpm(request.samples, resolvedSampleRate);
}

/**
 * Detect the musical key. The chroma is read at concert A440 unless `tuning`
 * says otherwise: a semitone fraction in `[-0.5, 0.5)`, or `'auto'` to measure
 * it from the audio. The result reports the tuning used.
 */
export function detectKey(request: DetectKeyRequest): KeyDetection;
export function detectKey(
  samples: Float32Array,
  sampleRate?: number,
  options?: DetectKeyOptions,
): KeyDetection;
export function detectKey(
  samples: Float32Array | DetectKeyRequest,
  sampleRate?: number,
  options?: DetectKeyOptions,
): KeyDetection {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('detectKey', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectKey', request.samples, resolvedSampleRate, request);
  return addon.detectKey(request.samples, resolvedSampleRate, request);
}

export function detectKeyCandidates(request: DetectKeyCandidatesRequest): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array | DetectKeyCandidatesRequest,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): KeyCandidate[] {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('detectKeyCandidates', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectKeyCandidates', request.samples, resolvedSampleRate, request);
  return addon.detectKeyCandidates(request.samples, resolvedSampleRate, request);
}

export function detectBeats(request: SamplesRequest): Float32Array;
export function detectBeats(samples: Float32Array, sampleRate?: number): Float32Array;
export function detectBeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate }
      : requestObject('detectBeats', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectBeats', request.samples, resolvedSampleRate, request);
  return addon.detectBeats(request.samples, resolvedSampleRate);
}

export function detectDownbeats(request: SamplesRequest): Float32Array;
export function detectDownbeats(samples: Float32Array, sampleRate?: number): Float32Array;
export function detectDownbeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate }
      : requestObject('detectDownbeats', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectDownbeats', request.samples, resolvedSampleRate, request);
  return addon.detectDownbeats(request.samples, resolvedSampleRate);
}

export function detectOnsets(request: DetectOnsetsRequest): Float32Array;
export function detectOnsets(
  samples: Float32Array,
  sampleRate?: number,
  options?: OnsetDetectOptions,
): Float32Array;
export function detectOnsets(
  samples: Float32Array | DetectOnsetsRequest,
  sampleRate = 22050,
  options: OnsetDetectOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('detectOnsets', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectOnsets', request.samples, resolvedSampleRate, request);
  return addon.detectOnsets(request.samples, resolvedSampleRate, request);
}

export function analyze(request: MusicAnalyzeRequest): AnalysisResult;
export function analyze(
  samples: Float32Array,
  sampleRate?: number,
  options?: MusicAnalyzeOptions,
): AnalysisResult;
export function analyze(
  samples: Float32Array | MusicAnalyzeRequest,
  sampleRate = 22050,
  options: MusicAnalyzeOptions = {},
): AnalysisResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyze', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyze', request.samples, resolvedSampleRate, request);
  return addon.analyze(request.samples, resolvedSampleRate, request);
}

const asFloat32Array = (values: ArrayLike<number>): Float32Array =>
  values instanceof Float32Array ? values : Float32Array.from(values);

/**
 * Estimate meter over a caller-supplied beat series, without audio and without
 * re-running analysis, so an arbitrary span of an existing result — or a beat
 * series from anywhere else — can be scored on its own.
 *
 * `beatStrengths` is the accent evidence the scoring reads; feed it
 * `AnalysisResult`'s `beatObservations.onsetStrength` rather than
 * `beats[].strength`, which is a single unwindowed envelope frame. Either
 * arrives in whatever units the envelope produced; the series is divided by its
 * own maximum before scoring, so it needs no pre-scaling.
 *
 * Option values are validated by the core, so an out-of-range weight, a
 * denominator that is not a power of two, an empty `candidateNumerators`, or a
 * `beatTimes` that decreases surfaces as a `SonareError`.
 *
 * The result carries `grouping` alongside the numerator: how the bar divides
 * into accent groups of two and three beats, so a seven comes back as
 * `[3, 2, 2]` or `[2, 2, 3]` rather than as a bare seven.
 */
export function estimateMeter(request: EstimateMeterRequest): MeterEstimate {
  requestObject('estimateMeter', request, 'request', true);
  return addon.estimateMeter(
    asFloat32Array(request.beatTimes),
    asFloat32Array(request.beatStrengths),
    request,
  );
}

/**
 * Synthesize a room impulse response from shoebox geometry.
 *
 * @throws SonareError when the geometry, placement or timing is unusable (for
 *   example a source outside the room); the message leads with the diagnostic
 *   code, such as `acoustic.source_outside_room`.
 */
export function synthesizeRir(options: RirSynthOptions = {}): RirResult {
  requestObject('synthesizeRir', options, 'request', true);
  assertRoomOptions('synthesizeRir', options);
  return addon.synthesizeRir(options);
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response. The volume scale is anchored by
 * `referenceAbsorption`; `confidence` reports how well the data support it.
 */
export function estimateRoom(request: RoomEstimateRequest): RoomEstimateResult;
export function estimateRoom(
  samples: Float32Array,
  sampleRate?: number,
  options?: RoomEstimateOptions,
): RoomEstimateResult;
export function estimateRoom(
  samples: Float32Array | RoomEstimateRequest,
  sampleRate = 48000,
  options: RoomEstimateOptions = {},
): RoomEstimateResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('estimateRoom', samples);
  assertRoomOptions('estimateRoom', request);
  const resolvedSampleRate = request.sampleRate ?? 48000;
  assertSampleRate('estimateRoom', resolvedSampleRate);
  return addon.estimateRoom(request.samples, resolvedSampleRate, request);
}

/**
 * Turn a room estimate into the geometry {@link synthesizeRir} takes.
 *
 * The pair to {@link estimateRoom}: the estimate's `lengthM`, `widthM`,
 * `heightM` and `bandAbsorption` are already `synthesizeRir`'s names, so this
 * merges the placement in and drops what the estimate does not converge on. The
 * estimate carries no placement, so `source` and `listener` are set only when
 * given; an omitted one is left to `synthesizeRir`'s own default, which may fall
 * outside a small estimated room. Absorption bands that did not converge are
 * left out, so the scalar `absorption` applies.
 *
 * @throws SonareError when the estimate has no measurable dimensions (NaN).
 */
export function roomGeometryFromEstimate(
  request: RoomGeometryFromEstimateRequest,
): RoomGeometryOptions;
export function roomGeometryFromEstimate(
  estimate: RoomGeometryEstimate,
  options?: RoomGeometryFromEstimateOptions,
): RoomGeometryOptions;
export function roomGeometryFromEstimate(
  estimate: RoomGeometryEstimate | RoomGeometryFromEstimateRequest,
  options: RoomGeometryFromEstimateOptions = {},
): RoomGeometryOptions {
  if (typeof estimate !== 'object' || estimate === null) {
    throw new TypeError('roomGeometryFromEstimate: estimate must be an object');
  }
  const request: RoomGeometryFromEstimateRequest =
    'estimate' in estimate ? estimate : { estimate, ...options };
  if (typeof request.estimate !== 'object' || request.estimate === null) {
    throw new TypeError('roomGeometryFromEstimate: estimate must be an object');
  }
  const geometry: RoomGeometryOptions = addon.roomGeometryFromEstimate(request.estimate);
  const { source, listener } = request;
  if (source !== undefined) {
    geometry.sourceX = source.x;
    geometry.sourceY = source.y;
    geometry.sourceZ = source.z;
  }
  if (listener !== undefined) {
    geometry.listenerX = listener.x;
    geometry.listenerY = listener.y;
    geometry.listenerZ = listener.z;
  }
  return geometry;
}

/**
 * Morph a recording's reverberation toward a target room (creative FX, not
 * dereverberation).
 *
 * Returns the morphed samples in `audio` (input length plus the target room's
 * reverb tail) alongside the target-room synthesis's own `diagnostics`, which
 * report a room other than the one requested — see {@link RoomMorphResult}.
 */
export function roomMorph(request: RoomMorphRequest): RoomMorphResult;
export function roomMorph(
  samples: Float32Array,
  sampleRate: number,
  options?: RoomMorphOptions,
): RoomMorphResult;
export function roomMorph(
  samples: Float32Array | RoomMorphRequest,
  sampleRate = 48000,
  options: RoomMorphOptions = {},
): RoomMorphResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('roomMorph', samples);
  assertRoomOptions('roomMorph', request);
  const resolvedSampleRate = request.sampleRate ?? 48000;
  assertSampleRate('roomMorph', resolvedSampleRate);
  return addon.roomMorph(request.samples, resolvedSampleRate, request);
}

/**
 * Asynchronous variant of {@link analyze}. Runs the DSP pipeline on a libuv
 * worker thread so the JS event loop is never blocked. The returned promise
 * resolves with the same shape as the synchronous version.
 */
export function analyzeAsync(request: MusicAnalyzeRequest): Promise<AnalysisResult>;
export function analyzeAsync(
  samples: Float32Array,
  sampleRate?: number,
  options?: MusicAnalyzeOptions,
): Promise<AnalysisResult>;
export function analyzeAsync(
  samples: Float32Array | MusicAnalyzeRequest,
  sampleRate = 22050,
  options: MusicAnalyzeOptions = {},
): Promise<AnalysisResult> {
  // Preserve the legacy async validation contract: invalid positional input is
  // handed to the addon so it becomes a rejected Promise, not a synchronous
  // property-access error while normalizing the new request form.
  if (!(samples instanceof Float32Array) && (!samples || typeof samples !== 'object')) {
    return addon.analyzeAsync(samples as Float32Array, sampleRate);
  }
  // Normalizing and validating can throw (a bad sample rate, or an invalid
  // option value the addon reports as a synchronous C++ throw rather than a
  // pending-exception-to-reject conversion); route that through the same
  // rejected-Promise contract so `analyzeAsync(...).catch(h)` sees every
  // validation failure, matching masterAudioAsync/masterAudioStereoAsync.
  try {
    const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
    const resolvedSampleRate = request.sampleRate ?? 22050;
    assertAudioInput('analyzeAsync', request.samples, resolvedSampleRate, request);
    return addon.analyzeAsync(request.samples, resolvedSampleRate, request);
  } catch (error) {
    return Promise.reject(error);
  }
}

/**
 * Run the full music analysis, reporting per-stage progress.
 *
 * The progress callback is invoked synchronously during analysis with a
 * normalized progress value in `[0, 1]` and the current stage name. The result
 * shape matches {@link analyze}.
 */
export function analyzeWithProgress(request: AnalyzeWithProgressRequest): AnalysisResult;
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number | undefined,
  onProgress: AnalysisProgressCallback,
  options?: MusicAnalyzeOptions,
): AnalysisResult;
export function analyzeWithProgress(
  samples: Float32Array | AnalyzeWithProgressRequest,
  sampleRate?: number,
  onProgress?: AnalysisProgressCallback,
  options?: MusicAnalyzeOptions,
): AnalysisResult {
  const request: AnalyzeWithProgressRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, onProgress, options }
      : requestObject('analyzeWithProgress', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeWithProgress', request.samples, resolvedSampleRate, request);
  // A request object built from analyze/analyzeAsync's flattened
  // MusicAnalyzeRequest shape carries its option fields directly on the
  // request rather than nested under `options`; read both and let the
  // nested `options` -- this function's own documented shape -- win a field
  // present in both, rather than silently dropping the flattened ones.
  const mergedOptions: MusicAnalyzeOptions = {
    ...flattenedMusicAnalyzeOptions(request),
    ...request.options,
  };
  // The addon reads options with the same reader analyze uses.
  return addon.analyzeWithProgress(
    request.samples,
    resolvedSampleRate,
    request.onProgress ?? (() => {}),
    request.cancel ?? (() => false),
    mergedOptions,
  );
}

/** Detect song-structure sections (intro/verse/chorus/...). */
export function analyzeSections(request: AnalyzeSectionsRequest): Section[];
export function analyzeSections(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeSectionsOptions,
): Section[];
export function analyzeSections(
  samples: Float32Array | AnalyzeSectionsRequest,
  sampleRate = 22050,
  options: AnalyzeSectionsOptions = {},
): Section[] {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeSections', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeSections', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('analyzeSections', request.nFft, request.hopLength);
  return addon.analyzeSections(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.minSectionSec ?? 4.0,
  );
}

/**
 * Detect structural boundaries and return the novelty curve they came from.
 *
 * This is the layer {@link analyzeSections} is built on, not a coarser view of
 * it: sections are labelled spans, these are the unlabelled transitions plus
 * the continuous curve they were picked from, so a caller that wants its own
 * threshold needs this and cannot derive it from the section list.
 *
 * Two thresholds decide what is returned. `absoluteThreshold` is compared
 * against the raw response and asks whether the features changed at all;
 * `threshold` is relative and is applied to the curve after it has been scaled
 * by its own maximum, so it cannot answer that question on its own.
 *
 * The analysis resamples anything above 22.05 kHz before computing a feature,
 * so the result's `sampleRate`, `hopLength` and `frameStride` describe the
 * analysis grid `frame` indexes rather than the input. `time` is the
 * authoritative output.
 *
 * @example
 * ```ts
 * const { boundaries, noveltyCurve, noveltyPeak } = detectBoundaries({
 *   samples,
 *   sampleRate: 44100,
 * });
 * for (const boundary of boundaries) {
 *   console.log(boundary.time, boundary.strength);
 * }
 * ```
 */
export function detectBoundaries(request: DetectBoundariesRequest): BoundaryResult {
  requestObject('detectBoundaries', request, 'request', true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('detectBoundaries', request.samples, resolvedSampleRate, request);
  // Validation only: the addon seeds every default from the C ABI, so the
  // resolved pair is deliberately not passed on.
  resolveFftOptions('detectBoundaries', request.nFft, request.hopLength);
  return addon.detectBoundaries(request.samples, resolvedSampleRate, request);
}

/**
 * Extract the melody contour from monophonic audio.
 *
 * By default this uses plain per-frame YIN. Pass `{ usePyin: true }` for the
 * Viterbi-smoothed pYIN tracker (less prone to octave jumps), or supply
 * `usePyin` / `center` positionally. When pYIN is active, `center` (default
 * `true`) zero-pads by `frameLength / 2` so frame `i` is centered at
 * `i * hopLength` (matching `librosa.pyin(center=True)`); `center` is ignored
 * for plain YIN.
 */
export function analyzeMelody(request: AnalyzeMelodyRequest): MelodyResult;
export function analyzeMelody(
  samples: Float32Array,
  sampleRate?: number,
  options?: MelodyOptions,
): MelodyResult;
export function analyzeMelody(
  samples: Float32Array | AnalyzeMelodyRequest,
  sampleRate = 22050,
  options: MelodyOptions = {},
): MelodyResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeMelody', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeMelody', request.samples, resolvedSampleRate, request);
  // frameLength is a framing window, not a transform size, so it carries no evenness rule.
  assertPositiveInteger('analyzeMelody', request.frameLength ?? 2048, 'frameLength');
  assertPositiveInteger('analyzeMelody', request.hopLength ?? 256, 'hopLength');
  return addon.analyzeMelody(
    request.samples,
    resolvedSampleRate,
    request.fmin ?? 65.0,
    request.fmax ?? 2093.0,
    request.frameLength ?? 2048,
    request.hopLength ?? 256,
    request.threshold ?? 0.1,
    request.usePyin ?? false,
    request.center ?? true,
  );
}

export function analyzeBpm(request: AnalyzeBpmRequest): BpmAnalysisResult;
export function analyzeBpm(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeBpmOptions,
): BpmAnalysisResult;
export function analyzeBpm(
  samples: Float32Array | AnalyzeBpmRequest,
  sampleRate = 22050,
  options: AnalyzeBpmOptions = {},
): BpmAnalysisResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeBpm', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeBpm', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('analyzeBpm', request.nFft, request.hopLength);
  assertPositiveInteger('analyzeBpm', request.maxCandidates ?? 5, 'maxCandidates');
  return addon.analyzeBpm(
    request.samples,
    resolvedSampleRate,
    request.bpmMin ?? 30.0,
    request.bpmMax ?? 300.0,
    request.startBpm ?? 120.0,
    fft.nFft,
    fft.hopLength,
    request.maxCandidates ?? 5,
  );
}

export function analyzeRhythm(request: AnalyzeRhythmRequest): RhythmResult;
export function analyzeRhythm(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeRhythmOptions,
): RhythmResult;
export function analyzeRhythm(
  samples: Float32Array | AnalyzeRhythmRequest,
  sampleRate = 22050,
  options: AnalyzeRhythmOptions = {},
): RhythmResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeRhythm', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeRhythm', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('analyzeRhythm', request.nFft, request.hopLength);
  return addon.analyzeRhythm(
    request.samples,
    resolvedSampleRate,
    request.bpmMin ?? 60.0,
    request.bpmMax ?? 200.0,
    request.startBpm ?? 120.0,
    fft.nFft,
    fft.hopLength,
  );
}

export function analyzeDynamics(request: AnalyzeDynamicsRequest): DynamicsResult;
export function analyzeDynamics(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeDynamicsOptions,
): DynamicsResult;
export function analyzeDynamics(
  samples: Float32Array | AnalyzeDynamicsRequest,
  sampleRate = 22050,
  options: AnalyzeDynamicsOptions = {},
): DynamicsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeDynamics', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeDynamics', request.samples, resolvedSampleRate, request);
  assertPositiveInteger('analyzeDynamics', request.hopLength ?? 512, 'hopLength');
  return addon.analyzeDynamics(
    request.samples,
    resolvedSampleRate,
    request.windowSec ?? 0.4,
    request.hopLength ?? 512,
    request.compressionThreshold ?? 6.0,
  );
}

export function analyzeImpulseResponse(request: AnalyzeImpulseResponseRequest): AcousticResult;
export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate?: number,
  nOctaveBands?: number,
  minDecayDb?: number,
): AcousticResult;
export function analyzeImpulseResponse(
  samples: Float32Array | AnalyzeImpulseResponseRequest,
  sampleRate = 48000,
  nOctaveBands = 6,
  minDecayDb?: number,
): AcousticResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nOctaveBands, minDecayDb }
      : requestObject('analyzeImpulseResponse', samples);
  if (request.minDecayDb === null) {
    throw new TypeError('analyzeImpulseResponse: minDecayDb must be a finite number');
  }
  const resolvedMinDecayDb = request.minDecayDb === undefined ? 30.0 : request.minDecayDb;
  assertFiniteScalar('analyzeImpulseResponse', resolvedMinDecayDb, 'minDecayDb');
  if (resolvedMinDecayDb <= 0) {
    throw new RangeError('analyzeImpulseResponse: minDecayDb must be greater than zero');
  }
  const resolvedSampleRate = request.sampleRate ?? 48000;
  assertAudioInput('analyzeImpulseResponse', request.samples, resolvedSampleRate, request);
  // Zero is degenerate but legal: the C ABI refuses only a negative count, so
  // refusing it here would narrow the domain the other surfaces accept.
  assertNonNegativeSafeInteger('analyzeImpulseResponse', request.nOctaveBands ?? 6, 'nOctaveBands');
  return addon.analyzeImpulseResponse(
    request.samples,
    resolvedSampleRate,
    request.nOctaveBands ?? 6,
    resolvedMinDecayDb,
  );
}

export function detectAcoustic(request: DetectAcousticRequest): AcousticResult;
export function detectAcoustic(
  samples: Float32Array,
  sampleRate?: number,
  options?: AcousticOptions,
): AcousticResult;
export function detectAcoustic(
  samples: Float32Array | DetectAcousticRequest,
  sampleRate = 48000,
  options: AcousticOptions = {},
): AcousticResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('detectAcoustic', samples);
  const resolvedSampleRate = request.sampleRate ?? 48000;
  assertAudioInput('detectAcoustic', request.samples, resolvedSampleRate, request);
  assertNonNegativeSafeInteger('detectAcoustic', request.nOctaveBands ?? 6, 'nOctaveBands');
  assertNonNegativeSafeInteger(
    'detectAcoustic',
    request.nThirdOctaveSubbands ?? 24,
    'nThirdOctaveSubbands',
  );
  return addon.detectAcoustic(
    request.samples,
    resolvedSampleRate,
    request.nOctaveBands ?? 6,
    request.nThirdOctaveSubbands ?? 24,
    request.minDecayDb ?? 30.0,
    request.noiseFloorMarginDb ?? 10.0,
  );
}

export function analyzeTimbre(request: AnalyzeTimbreRequest): TimbreResult;
export function analyzeTimbre(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeTimbreOptions,
): TimbreResult;
export function analyzeTimbre(
  samples: Float32Array | AnalyzeTimbreRequest,
  sampleRate = 22050,
  options: AnalyzeTimbreOptions = {},
): TimbreResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('analyzeTimbre', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('analyzeTimbre', request.samples, resolvedSampleRate, request);
  const fft = resolveFftOptions('analyzeTimbre', request.nFft, request.hopLength);
  assertPositiveInteger('analyzeTimbre', request.nMels ?? 128, 'nMels');
  assertPositiveInteger('analyzeTimbre', request.nMfcc ?? 13, 'nMfcc');
  return addon.analyzeTimbre(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nMels ?? 128,
    request.nMfcc ?? 13,
    request.windowSec ?? 0.5,
  );
}

/**
 * Resolved chord-detection parameters with all defaults applied. Used to feed
 * the positional native call from either the positional or options-object
 * public forms.
 */
interface ResolvedChordParams {
  minDuration: number;
  smoothingWindow: number;
  threshold: number;
  useTriadsOnly: boolean;
  nFft: number;
  hopLength: number;
  useBeatSync: boolean;
  useHmm: boolean;
  hmmBeamWidth: number;
  useKeyContext: boolean;
  keyRoot: ChordFunctionsKey['root'];
  keyMode: ChordFunctionsKey['mode'];
  detectInversions: boolean;
  chromaMethod: ChordChromaMethod;
  tuning: number | 'auto';
}

function resolveChordOptions(options: ChordDetectionOptions): ResolvedChordParams {
  return {
    minDuration: options.minDuration ?? 0.3,
    smoothingWindow: options.smoothingWindow ?? 2.0,
    threshold: options.threshold ?? 0.5,
    useTriadsOnly: options.useTriadsOnly ?? false,
    nFft: options.nFft ?? 2048,
    hopLength: options.hopLength ?? 512,
    useBeatSync: options.useBeatSync ?? true,
    useHmm: options.useHmm ?? false,
    hmmBeamWidth: options.hmmBeamWidth ?? 24,
    useKeyContext: options.useKeyContext ?? false,
    keyRoot: options.keyRoot ?? 0,
    keyMode: options.keyMode ?? 0,
    detectInversions: options.detectInversions ?? false,
    chromaMethod: options.chromaMethod ?? 'stft',
    tuning: options.tuning ?? 0,
  };
}

/** Validate the resolved chord params the addon narrows into C ints, before they are sent. */
/**
 * The chord parameters both entry points resolve the same way. The key is not
 * among them: `chordFunctionalAnalysis` takes it as its own argument and sends
 * that rather than the resolved one, so each caller checks the key it sends.
 */
function assertChordParams(
  fnName: string,
  p: ResolvedChordParams,
): { nFft: number; hopLength: number } {
  const fft = resolveFftOptions(fnName, p.nFft, p.hopLength);
  assertPositiveInteger(fnName, p.hmmBeamWidth, 'hmmBeamWidth');
  return fft;
}

/**
 * Detect chords from mono samples.
 *
 * Accepts either an options object (`detectChords(samples, sampleRate, options)`,
 * matching the WASM binding) or the legacy positional argument list. The form
 * is selected by the type of the third argument: an object selects the
 * options form, otherwise the positional form is used.
 */
export function detectChords(request: DetectChordsRequest): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate?: number,
  minDuration?: number,
  smoothingWindow?: number,
  threshold?: number,
  useTriadsOnly?: boolean,
  nFft?: number,
  hopLength?: number,
  useBeatSync?: boolean,
  useHmm?: boolean,
  hmmBeamWidth?: number,
  useKeyContext?: boolean,
  keyRoot?: ChordFunctionsKey['root'],
  keyMode?: ChordFunctionsKey['mode'],
  detectInversions?: boolean,
  chromaMethod?: ChordChromaMethod,
): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array | DetectChordsRequest,
  sampleRate = 22050,
  minDurationOrOptions: number | ChordDetectionOptions = 0.3,
  smoothingWindow = 2.0,
  threshold = 0.5,
  useTriadsOnly = false,
  nFft = 2048,
  hopLength = 512,
  useBeatSync = true,
  useHmm = false,
  hmmBeamWidth = 24,
  useKeyContext = false,
  keyRoot: ChordFunctionsKey['root'] = 0,
  keyMode: ChordFunctionsKey['mode'] = 0,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): ChordAnalysisResult {
  if (!(samples instanceof Float32Array)) {
    requestObject('detectChords', samples);
  }
  const p: ResolvedChordParams =
    samples instanceof Float32Array && typeof minDurationOrOptions === 'object'
      ? resolveChordOptions(minDurationOrOptions)
      : samples instanceof Float32Array
        ? {
            minDuration: minDurationOrOptions as number,
            smoothingWindow,
            threshold,
            useTriadsOnly,
            nFft,
            hopLength,
            useBeatSync,
            useHmm,
            hmmBeamWidth,
            useKeyContext,
            keyRoot,
            keyMode,
            detectInversions,
            chromaMethod,
            tuning: 0,
          }
        : resolveChordOptions(samples);
  const resolvedSampleRate =
    samples instanceof Float32Array ? sampleRate : (samples.sampleRate ?? 22050);
  assertAudioInput(
    'detectChords',
    samples instanceof Float32Array ? samples : samples.samples,
    resolvedSampleRate,
    samples instanceof Float32Array ? {} : samples,
  );
  const fft = assertChordParams('detectChords', p);
  return addon.detectChords(
    samples instanceof Float32Array ? samples : samples.samples,
    resolvedSampleRate,
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    fft.nFft,
    fft.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.keyRoot,
    p.keyMode,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
    p.tuning === 'auto' ? 0 : p.tuning,
    p.tuning === 'auto',
  );
}

/**
 * Functional (Roman-numeral) chord analysis from mono samples: {@link detectChords}
 * followed by {@link chordFunctions} in the given key, returning the timed
 * entries with `roman` and `function` added.
 *
 * Accepts either an options object
 * (`chordFunctionalAnalysis(samples, keyRoot, keyMode, sampleRate, options)`,
 * matching the WASM binding) or the legacy positional argument list. The form
 * is selected by the type of the fifth argument.
 */
export function chordFunctionalAnalysis(request: ChordFunctionalAnalysisRequest): FunctionalChord[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: ChordFunctionsKey['root'],
  keyMode?: ChordFunctionsKey['mode'],
  sampleRate?: number,
  options?: ChordDetectionOptions,
): FunctionalChord[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: ChordFunctionsKey['root'],
  keyMode?: ChordFunctionsKey['mode'],
  sampleRate?: number,
  minDuration?: number,
  smoothingWindow?: number,
  threshold?: number,
  useTriadsOnly?: boolean,
  nFft?: number,
  hopLength?: number,
  useBeatSync?: boolean,
  useHmm?: boolean,
  hmmBeamWidth?: number,
  useKeyContext?: boolean,
  detectInversions?: boolean,
  chromaMethod?: ChordChromaMethod,
): FunctionalChord[];
export function chordFunctionalAnalysis(
  samples: Float32Array | ChordFunctionalAnalysisRequest,
  keyRoot?: ChordFunctionsKey['root'],
  keyMode: ChordFunctionsKey['mode'] = 0,
  sampleRate = 22050,
  minDurationOrOptions: number | ChordDetectionOptions = 0.3,
  smoothingWindow = 2.0,
  threshold = 0.5,
  useTriadsOnly = false,
  nFft = 2048,
  hopLength = 512,
  useBeatSync = true,
  useHmm = false,
  hmmBeamWidth = 24,
  useKeyContext = false,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): FunctionalChord[] {
  if (!(samples instanceof Float32Array)) {
    requestObject('chordFunctionalAnalysis', samples);
  }
  const p: ResolvedChordParams =
    samples instanceof Float32Array && typeof minDurationOrOptions === 'object'
      ? resolveChordOptions(minDurationOrOptions)
      : samples instanceof Float32Array
        ? {
            minDuration: minDurationOrOptions as number,
            smoothingWindow,
            threshold,
            useTriadsOnly,
            nFft,
            hopLength,
            useBeatSync,
            useHmm,
            hmmBeamWidth,
            useKeyContext,
            keyRoot: keyRoot ?? 0,
            keyMode,
            detectInversions,
            chromaMethod,
            tuning: 0,
          }
        : resolveChordOptions(samples);
  const resolvedSampleRate =
    samples instanceof Float32Array ? sampleRate : (samples.sampleRate ?? 22050);
  assertAudioInput(
    'chordFunctionalAnalysis',
    samples instanceof Float32Array ? samples : samples.samples,
    resolvedSampleRate,
    samples instanceof Float32Array ? {} : samples,
  );
  const fft = assertChordParams('chordFunctionalAnalysis', p);
  // Ahead of the key, because a first argument that is neither form reads as a
  // request whose every field is absent, and the key would be blamed for it.
  if (!(samples instanceof Float32Array) && !(samples.samples instanceof Float32Array)) {
    throw new TypeError('chordFunctionalAnalysis: samples must be a Float32Array');
  }
  // The key travels beside the resolved parameters rather than inside them, so
  // it is read once here and both checked and sent from the same const.
  const resolvedKeyRoot = samples instanceof Float32Array ? keyRoot : samples.keyRoot;
  const resolvedKeyMode = samples instanceof Float32Array ? keyMode : (samples.keyMode ?? 0);
  if (resolvedKeyRoot === undefined || resolvedKeyRoot === null) {
    throw new TypeError('chordFunctionalAnalysis: keyRoot is required');
  }
  const analysed: {
    chords: Chord[];
    roman: string[];
    functions: ChordFunction[];
  } = addon.chordFunctionalAnalysis(
    samples instanceof Float32Array ? samples : samples.samples,
    resolvedKeyRoot,
    resolvedKeyMode,
    resolvedSampleRate,
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    fft.nFft,
    fft.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
    p.tuning === 'auto' ? 0 : p.tuning,
    p.tuning === 'auto',
  );
  return analysed.chords.map((chord, i) => ({
    ...chord,
    roman: analysed.roman[i],
    function: analysed.functions[i],
  }));
}

/**
 * Label chords that are already known with their Roman numeral and harmonic
 * function in a key. Nothing is re-detected.
 *
 * `chords` is {@link detectChords}' result or its `chords` array, or the same
 * shape built by hand (only `root` and `quality` are read). `key` is
 * `{ root, mode }`, e.g. {@link detectKey}'s result. Returns the same timed
 * entries with `roman` (`'I'`, `'V7'`, `'vi'`; `'N.C.'` for an unknown chord)
 * and `function` (`'tonic'`, `'subdominant'`, `'dominant'`, `'chromatic'` for a
 * root outside the key's scale, `'none'` for an unknown chord) added. Every mode
 * other than minor reads the major scale.
 */
export function chordFunctions(request: ChordFunctionsRequest): FunctionalChord[];
export function chordFunctions<T extends ChordFunctionsInput>(
  request: ChordFunctionsRequest<T>,
): FunctionalChord<T>[];
export function chordFunctions(
  request: ChordFunctionsRequest<ChordFunctionsInput>,
): FunctionalChord<ChordFunctionsInput>[] {
  requestObject('chordFunctions', request, 'request', true);
  const entries = 'chords' in request.chords ? request.chords.chords : request.chords;
  const labels: { roman: string[]; functions: ChordFunction[] } = addon.chordFunctions(
    entries,
    request.key.root,
    request.key.mode,
  );
  return entries.map((chord, i) => ({
    ...chord,
    roman: labels.roman[i],
    function: labels.functions[i],
  }));
}

function chordChromaMethodValue(method: ChordChromaMethod): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new RangeError(`Invalid chord chroma method: ${method}`);
}
