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
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  DynamicsResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  MelodyOptions,
  MelodyResult,
  MeterEstimate,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomMorphOptions,
  Section,
  TimbreResult,
} from './types.js';
import { assertFiniteScalar } from './validation.js';

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

export interface DetectKeyRequest extends KeyDetectionOptions, SamplesRequest {}

export interface RoomEstimateRequest extends RoomEstimateOptions, SamplesRequest {}
export interface RoomMorphRequest extends RoomMorphOptions, SamplesRequest {}

export interface AnalyzeWithProgressRequest extends SamplesRequest {
  onProgress?: AnalysisProgressCallback;
  cancel?: () => boolean;
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
}
export interface MusicAnalyzeRequest extends SamplesRequest, MusicAnalyzeOptions {}

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
   */
  beatStrengths: ArrayLike<number>;
  /**
   * Meter numerators to score. At most 16 entries, each in `[2, 32]`; an empty
   * list is rejected rather than restoring the default. Widening the set does
   * not force a wider meter. Default `[3, 4, 6]`.
   */
  candidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter; a power of two in `[1, 32]`. The
   * estimator still reports 8 on its own when it resolves a compound meter.
   * Default 4.
   */
  denominator?: number;
  /** Weight on the accent at each measure's first beat. Default 1. */
  downbeatWeight?: number;
  /** Weight on measure-to-measure accent agreement. Default 0.5. */
  measureWeight?: number;
  /** Weight on the subdivision accent pattern. Default 0.15. */
  subdivisionWeight?: number;
  /** Subdivision score at which a 6 candidate is reported as compound (x/8). Default 0.85. */
  compoundSubdivisionThreshold?: number;
}

export interface AnalyzeSectionsRequest extends AnalyzeSectionsOptions, SamplesRequest {}
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
  keyRoot: number;
  keyMode?: number;
}

export function detectBpm(request: SamplesRequest): number;
export function detectBpm(samples: Float32Array, sampleRate?: number): number;
export function detectBpm(samples: Float32Array | SamplesRequest, sampleRate = 22050): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.detectBpm(request.samples, request.sampleRate ?? 22050);
}

export function detectKey(request: DetectKeyRequest): Key;
export function detectKey(
  samples: Float32Array,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): Key;
export function detectKey(
  samples: Float32Array | DetectKeyRequest,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): Key {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.detectKey(request.samples, request.sampleRate ?? 22050, request);
}

export function detectKeyCandidates(request: DetectKeyRequest): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array | DetectKeyRequest,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): KeyCandidate[] {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.detectKeyCandidates(request.samples, request.sampleRate ?? 22050, request);
}

export function detectBeats(request: SamplesRequest): Float32Array;
export function detectBeats(samples: Float32Array, sampleRate?: number): Float32Array;
export function detectBeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.detectBeats(request.samples, request.sampleRate ?? 22050);
}

export function detectDownbeats(request: SamplesRequest): Float32Array;
export function detectDownbeats(samples: Float32Array, sampleRate?: number): Float32Array;
export function detectDownbeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.detectDownbeats(request.samples, request.sampleRate ?? 22050);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.detectOnsets(request.samples, request.sampleRate ?? 22050, request);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyze(request.samples, request.sampleRate ?? 22050, request);
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
  return addon.estimateMeter(
    asFloat32Array(request.beatTimes),
    asFloat32Array(request.beatStrengths),
    request,
  );
}

/**
 * Synthesize a room impulse response from shoebox geometry. `hasError` is true
 * when the source/listener falls outside the room (the RIR is then empty).
 */
export function synthesizeRir(options: RirSynthOptions = {}): RirResult {
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.estimateRoom(request.samples, request.sampleRate ?? 48000, request);
}

/**
 * Morph a recording's reverberation toward a target room (creative FX, not
 * dereverberation). Returns the morphed samples (input length plus the target
 * room's reverb tail).
 */
export function roomMorph(request: RoomMorphRequest): Float32Array;
export function roomMorph(
  samples: Float32Array,
  sampleRate: number,
  options?: RoomMorphOptions,
): Float32Array;
export function roomMorph(
  samples: Float32Array | RoomMorphRequest,
  sampleRate = 48000,
  options: RoomMorphOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.roomMorph(request.samples, request.sampleRate ?? 48000, request);
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeAsync(request.samples, request.sampleRate ?? 22050, request);
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
): AnalysisResult;
export function analyzeWithProgress(
  samples: Float32Array | AnalyzeWithProgressRequest,
  sampleRate?: number,
  onProgress?: AnalysisProgressCallback,
): AnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, onProgress } : samples;
  return addon.analyzeWithProgress(
    request.samples,
    request.sampleRate ?? 22050,
    request.onProgress ?? (() => {}),
    request.cancel ?? (() => false),
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeSections(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.minSectionSec ?? 4.0,
  );
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeMelody(
    request.samples,
    request.sampleRate ?? 22050,
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeBpm(
    request.samples,
    request.sampleRate ?? 22050,
    request.bpmMin ?? 30.0,
    request.bpmMax ?? 300.0,
    request.startBpm ?? 120.0,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeRhythm(
    request.samples,
    request.sampleRate ?? 22050,
    request.bpmMin ?? 60.0,
    request.bpmMax ?? 200.0,
    request.startBpm ?? 120.0,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeDynamics(
    request.samples,
    request.sampleRate ?? 22050,
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
    samples instanceof Float32Array ? { samples, sampleRate, nOctaveBands, minDecayDb } : samples;
  if (request.minDecayDb === null) {
    throw new TypeError('analyzeImpulseResponse: minDecayDb must be a finite number');
  }
  const resolvedMinDecayDb = request.minDecayDb === undefined ? 30.0 : request.minDecayDb;
  assertFiniteScalar('analyzeImpulseResponse', resolvedMinDecayDb, 'minDecayDb');
  if (resolvedMinDecayDb <= 0) {
    throw new RangeError('analyzeImpulseResponse: minDecayDb must be greater than zero');
  }
  return addon.analyzeImpulseResponse(
    request.samples,
    request.sampleRate ?? 48000,
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.detectAcoustic(
    request.samples,
    request.sampleRate ?? 48000,
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
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.analyzeTimbre(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  keyRoot: number;
  keyMode: number;
  detectInversions: boolean;
  chromaMethod: ChordChromaMethod;
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
  };
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
  keyRoot?: number,
  keyMode?: number,
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
  keyRoot = 0,
  keyMode = 0,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): ChordAnalysisResult {
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
          }
        : resolveChordOptions(samples);
  return addon.detectChords(
    samples instanceof Float32Array ? samples : samples.samples,
    samples instanceof Float32Array ? sampleRate : (samples.sampleRate ?? 22050),
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    p.nFft,
    p.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.keyRoot,
    p.keyMode,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
  );
}

/**
 * Functional (Roman-numeral) chord analysis from mono samples.
 *
 * Accepts either an options object
 * (`chordFunctionalAnalysis(samples, keyRoot, keyMode, sampleRate, options)`,
 * matching the WASM binding) or the legacy positional argument list. The form
 * is selected by the type of the fifth argument.
 */
export function chordFunctionalAnalysis(request: ChordFunctionalAnalysisRequest): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: number,
  keyMode?: number,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: number,
  keyMode?: number,
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
): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array | ChordFunctionalAnalysisRequest,
  keyRoot = 0,
  keyMode = 0,
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
): string[] {
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
          }
        : resolveChordOptions(samples);
  return addon.chordFunctionalAnalysis(
    samples instanceof Float32Array ? samples : samples.samples,
    samples instanceof Float32Array ? keyRoot : samples.keyRoot,
    samples instanceof Float32Array ? keyMode : (samples.keyMode ?? 0),
    samples instanceof Float32Array ? sampleRate : (samples.sampleRate ?? 22050),
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    p.nFft,
    p.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
  );
}

function chordChromaMethodValue(method: ChordChromaMethod): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}
