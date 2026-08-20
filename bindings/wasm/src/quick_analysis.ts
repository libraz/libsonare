import {
  chordChromaMethodValue,
  convertAnalysisResult,
  convertChordAnalysisResult,
  convertKeyCandidate,
  keyModeValues,
  keyProfileValue,
} from './analysis_helpers';
import { getSonareModule } from './module_state';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeTimbreOptions,
  ChordAnalysisResult,
  ChordDetectionOptions,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  MeterEstimate,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomMorphOptions,
} from './public_types';
import { Mode, PitchClass } from './public_types';
import type { ProgressCallback, WasmAcousticResult } from './sonare.js';
import type { ValidateOptions } from './validation';
import {
  assertFiniteScalar,
  assertNonNegativeInteger,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

type GuardedOptions = ValidateOptions;

/** Canonical request form for one-shot analysis functions. */
export interface SamplesRequest extends GuardedOptions {
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

/** Canonical request form for key detection functions. */
export interface DetectKeyRequest extends KeyDetectionOptions, SamplesRequest {}

/** Canonical request form for analysis with synchronous progress reporting. */
export interface AnalyzeWithProgressRequest extends SamplesRequest {
  onProgress?: ProgressCallback;
  cancel?: () => boolean;
}

/** Canonical request form for chord detection. */
export interface DetectChordsRequest extends ChordDetectionOptions, SamplesRequest {}

/** Canonical request form for functional chord analysis. */
export interface ChordFunctionalAnalysisRequest extends DetectChordsRequest {
  keyRoot: PitchClass;
  /** Musical mode; defaults to {@link Mode.Major}. */
  keyMode?: Mode;
}

/** Canonical request form for impulse-response analysis. */
export interface AnalyzeImpulseResponseRequest extends SamplesRequest {
  nOctaveBands?: number;
  minDecayDb?: number;
}

/** Canonical request form for acoustic analysis. */
export interface DetectAcousticRequest extends AcousticOptions, SamplesRequest {}

/** Canonical request form for equivalent-room estimation. */
export interface EstimateRoomRequest extends RoomEstimateOptions, SamplesRequest {}

/** Canonical request form for room-reverb morphing. */
export interface RoomMorphRequest extends RoomMorphOptions, GuardedOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Canonical request forms for detailed music-analysis APIs. */
export interface AnalyzeBpmRequest extends AnalyzeBpmOptions, SamplesRequest {}
export interface AnalyzeRhythmRequest extends AnalyzeRhythmOptions, SamplesRequest {}
export interface AnalyzeDynamicsRequest extends AnalyzeDynamicsOptions, SamplesRequest {}
export interface AnalyzeTimbreRequest extends AnalyzeTimbreOptions, SamplesRequest {}

function validateAnalysisInput(
  fnName: string,
  samples: Float32Array,
  sampleRate: number,
  options: GuardedOptions = {},
): void {
  assertSampleRate(fnName, sampleRate);
  assertSamples(fnName, samples, options.validate !== false);
}

// ============================================================================
// Quick API (High-level Analysis)
// ============================================================================

/**
 * Detect BPM from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Detected BPM
 */
export function detectBpm(request: SamplesRequest): number;
export function detectBpm(
  samples: Float32Array,
  sampleRate?: number,
  options?: GuardedOptions,
): number;
export function detectBpm(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
  options: GuardedOptions = {},
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectBpm', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().detectBpm(request.samples, request.sampleRate ?? 22050);
}

/**
 * Detect musical key from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Detected key
 */
export function detectKey(request: DetectKeyRequest): Key;
export function detectKey(
  samples: Float32Array,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): Key;
export function detectKey(
  samples: Float32Array | DetectKeyRequest,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): Key {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectKey', request.samples, request.sampleRate ?? 22050, request);
  const result = requireModule()._detectKeyWithOptions(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 4096,
    request.hopLength ?? 512,
    request.useHpss ?? false,
    request.loudnessWeighted ?? false,
    request.highPassHz ?? 0,
    keyModeValues(request.modes),
    keyProfileValue(request.profile),
    request.genreHint ?? '',
  );
  return {
    root: result.root as PitchClass,
    mode: result.mode as Mode,
    confidence: result.confidence,
    name: result.name,
    shortName: result.shortName,
  };
}

export function detectKeyCandidates(request: DetectKeyRequest): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate?: number,
  options?: KeyDetectionOptions,
): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array | DetectKeyRequest,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): KeyCandidate[] {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput(
    'detectKeyCandidates',
    request.samples,
    request.sampleRate ?? 22050,
    request,
  );
  // The embind value marshalling returns an array whose constructor is not this
  // realm's Array; chaining .map() onto it propagates that constructor via
  // Symbol.species, leaving a result that structuredClone (and so postMessage to
  // a Worker) rejects with "could not be cloned". Array.from() re-roots it as a
  // plain Array before mapping.
  const candidates = requireModule()._detectKeyCandidates(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 4096,
    request.hopLength ?? 512,
    request.useHpss ?? false,
    request.loudnessWeighted ?? false,
    request.highPassHz ?? 0,
    keyModeValues(request.modes),
    keyProfileValue(request.profile),
    request.genreHint ?? '',
  );
  return Array.from(candidates, convertKeyCandidate);
}

/**
 * Detect onset times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of onset times in seconds
 */
export function detectOnsets(request: DetectOnsetsRequest): Float32Array;
export function detectOnsets(
  samples: Float32Array,
  sampleRate?: number,
  options?: OnsetDetectOptions & GuardedOptions,
): Float32Array;
export function detectOnsets(
  samples: Float32Array | DetectOnsetsRequest,
  sampleRate = 22050,
  options: OnsetDetectOptions & GuardedOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectOnsets', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().detectOnsets(request.samples, request.sampleRate ?? 22050, request);
}

/**
 * Detect beat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of beat times in seconds
 */
export function detectBeats(request: SamplesRequest): Float32Array;
export function detectBeats(
  samples: Float32Array,
  sampleRate?: number,
  options?: GuardedOptions,
): Float32Array;
export function detectBeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
  options: GuardedOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectBeats', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().detectBeats(request.samples, request.sampleRate ?? 22050);
}

/**
 * Detect downbeat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Array of downbeat times in seconds
 */
export function detectDownbeats(request: SamplesRequest): Float32Array;
export function detectDownbeats(
  samples: Float32Array,
  sampleRate?: number,
  options?: GuardedOptions,
): Float32Array;
export function detectDownbeats(
  samples: Float32Array | SamplesRequest,
  sampleRate = 22050,
  options: GuardedOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectDownbeats', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().detectDownbeats(request.samples, request.sampleRate ?? 22050);
}

/**
 * Detect chords from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param options - Optional chord detection settings
 * @returns Detected chord segments
 */
export function detectChords(request: DetectChordsRequest): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array | DetectChordsRequest,
  sampleRate = 22050,
  options: ChordDetectionOptions = {},
): ChordAnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('detectChords', request.samples, request.sampleRate ?? 22050, request);
  const result = requireModule().detectChords(
    request.samples,
    request.sampleRate ?? 22050,
    request.minDuration ?? 0.3,
    request.smoothingWindow ?? 2.0,
    request.threshold ?? 0.5,
    request.useTriadsOnly ?? false,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.useBeatSync ?? true,
    request.useHmm ?? false,
    request.hmmBeamWidth ?? 24,
    request.useKeyContext ?? false,
    request.keyRoot ?? PitchClass.C,
    request.keyMode ?? Mode.Major,
    request.detectInversions ?? false,
    chordChromaMethodValue(request.chromaMethod ?? 'stft'),
  );
  return convertChordAnalysisResult(result);
}

/**
 * Functional (Roman-numeral) harmonic analysis of the detected chord
 * progression, relative to the given key. Mirrors the C-ABI
 * `sonare_chord_functional_analysis` and the Node/Python `chordFunctionalAnalysis`.
 *
 * @returns One Roman-numeral label (e.g. "I", "IV", "V", "vi") per detected chord
 */
export function chordFunctionalAnalysis(request: ChordFunctionalAnalysisRequest): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: PitchClass,
  keyMode?: Mode,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array | ChordFunctionalAnalysisRequest,
  keyRoot?: PitchClass,
  keyMode?: Mode,
  sampleRate = 22050,
  options: ChordDetectionOptions = {},
): string[] {
  const request =
    samples instanceof Float32Array
      ? { samples, keyRoot, keyMode, sampleRate, ...options }
      : samples;
  validateAnalysisInput(
    'chordFunctionalAnalysis',
    request.samples,
    request.sampleRate ?? 22050,
    request,
  );
  return requireModule().chordFunctionalAnalysis(
    request.samples,
    request.keyRoot as PitchClass,
    request.keyMode ?? Mode.Major,
    request.sampleRate ?? 22050,
    request.minDuration ?? 0.3,
    request.smoothingWindow ?? 2.0,
    request.threshold ?? 0.5,
    request.useTriadsOnly ?? false,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.useBeatSync ?? true,
    request.useHmm ?? false,
    request.hmmBeamWidth ?? 24,
    request.useKeyContext ?? false,
    request.detectInversions ?? false,
    chordChromaMethodValue(request.chromaMethod ?? 'stft'),
  );
}

/**
 * Options for {@link analyze}. Every field is optional and falls back to the
 * core default when omitted.
 */
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
  /**
   * Track a locally updated tempo prior during beat tracking (default: false).
   */
  adaptiveTempo?: boolean;
  /**
   * Length of the local tempo context in beats (default: 8). Must be positive.
   */
  tempoUpdateIntervalBeats?: number;
  /**
   * Decode a per-beat local tempo curve into
   * {@link AnalysisResult.beatLocalBpm} (default: false).
   *
   * @remarks
   * Off by default because it is an extra output rather than a better analysis:
   * nothing else in the result changes, and a caller that does not read the
   * curve would pay a decode over the beat grid for nothing.
   *
   * The curve describes the beat grid it was decoded from, and beat tracking
   * holds a fixed tempo prior unless {@link MusicAnalyzeOptions.adaptiveTempo}
   * is also set, so measuring a tempo that moves needs both.
   */
  computeTempoCurve?: boolean;
  /**
   * Meter numerators the estimator scores (default: `[3, 4, 6]`).
   *
   * @remarks
   * Adding a numerator widens the search; it does not force the result. The
   * list must hold between 1 and 16 entries, each in `[2, 32]`.
   */
  meterCandidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter (default: 4). Must be a power of
   * two in `[1, 32]`. The estimator still reports 8 on its own when it resolves
   * a compound meter, so this is the unit for everything else.
   */
  meterDenominator?: number;
}
export interface MusicAnalyzeRequest extends SamplesRequest, MusicAnalyzeOptions {}

/**
 * Perform complete music analysis.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Complete analysis result
 *
 * @remarks
 * This call is synchronous and blocks until analysis completes. Unlike the
 * Node binding (which offers `analyzeAsync` on a libuv worker thread), the
 * WASM build runs on a single thread, so there is no non-blocking variant —
 * the DSP pipeline always runs to completion on the calling thread. To keep
 * the UI responsive for long inputs, drive this from a Web Worker and use
 * {@link analyzeWithProgress} to report progress.
 */
export function analyze(request: MusicAnalyzeRequest): AnalysisResult;
export function analyze(
  samples: Float32Array,
  sampleRate?: number,
  options?: GuardedOptions & MusicAnalyzeOptions,
): AnalysisResult;
export function analyze(
  samples: Float32Array | MusicAnalyzeRequest,
  sampleRate = 22050,
  options: GuardedOptions & MusicAnalyzeOptions = {},
): AnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('analyze', request.samples, request.sampleRate ?? 22050, request);
  const result = requireModule().analyze(request.samples, request.sampleRate ?? 22050, request);
  return convertAnalysisResult(result);
}

/**
 * Canonical request form for {@link estimateMeter}.
 *
 * @remarks
 * Every scoring field is optional and falls back to the core default when
 * omitted; the core validates them and rejects a value it cannot answer rather
 * than substituting one.
 */
export interface EstimateMeterRequest {
  /** Beat positions in seconds, non-decreasing. */
  beatTimes: ArrayLike<number>;
  /**
   * Per-beat accent value, the same length as {@link beatTimes}.
   *
   * @remarks
   * `AnalysisResult.beatObservations.onsetStrength` is the intended source —
   * it is the windowed value the library's own downbeat pass scores.
   * `beats[].strength` also works but is a single unwindowed envelope frame.
   * Neither needs pre-scaling: the series is divided by its own maximum before
   * scoring, so only the accent contrast within it is read.
   */
  beatStrengths: ArrayLike<number>;
  /**
   * Meter numerators to score (default: `[3, 4, 6]`).
   *
   * @remarks
   * Adding a numerator widens the search; it does not force the result. The
   * list must hold between 1 and 16 entries, each in `[2, 32]`.
   */
  candidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter (default: 4). Must be a power of
   * two in `[1, 32]`. The estimator still reports 8 on its own when it resolves
   * a compound meter, so this is the unit for everything else.
   */
  denominator?: number;
  /** Weight of the downbeat accent term (default: 1). */
  downbeatWeight?: number;
  /** Weight of the measure-periodicity term (default: 0.5). */
  measureWeight?: number;
  /** Weight of the subdivision term (default: 0.15). */
  subdivisionWeight?: number;
  /** Score ratio above which a compound meter is preferred (default: 0.85). */
  compoundSubdivisionThreshold?: number;
}

/**
 * Estimate meter over a caller-supplied beat series.
 *
 * @param request - Beat series plus optional scoring configuration
 * @returns The selected signature, its downbeat phase and grouping, and the
 *   scored candidates
 *
 * @remarks
 * Scores only the per-beat strengths, so no audio and no frame-level onset
 * envelope is needed: an arbitrary span of an existing analysis can be scored
 * without re-running it. Pass `beatObservations.onsetStrength` rather than
 * `beats[].strength` — see {@link EstimateMeterRequest.beatStrengths}.
 *
 * The result carries `grouping` alongside the numerator: how the bar divides
 * into accent groups of two and three beats, so a seven comes back as `[3, 2,
 * 2]` or `[2, 2, 3]` rather than as a bare seven.
 */
export function estimateMeter(request: EstimateMeterRequest): MeterEstimate {
  return requireModule().estimateMeter(request.beatTimes, request.beatStrengths, request);
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
  validateAnalysisInput(
    'analyzeImpulseResponse',
    request.samples,
    request.sampleRate ?? 48000,
    request,
  );
  const result: WasmAcousticResult = requireModule().analyzeImpulseResponseEx(
    request.samples,
    request.sampleRate ?? 48000,
    request.nOctaveBands ?? 6,
    resolvedMinDecayDb,
  );
  return result;
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
  validateAnalysisInput('detectAcoustic', request.samples, request.sampleRate ?? 48000, request);
  const result: WasmAcousticResult = requireModule().detectAcoustic(
    request.samples,
    request.sampleRate ?? 48000,
    request.nOctaveBands ?? 6,
    request.nThirdOctaveSubbands ?? 24,
    request.minDecayDb ?? 30.0,
    request.noiseFloorMarginDb ?? 10.0,
  );
  return result;
}

/**
 * Synthesize a room impulse response from shoebox geometry. `hasError` is true
 * when the source/listener falls outside the room (the RIR is then empty).
 */
export function synthesizeRir(options: RirSynthOptions = {}): RirResult {
  const module = requireModule();
  if (typeof module.synthesizeRir !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  return module.synthesizeRir(options);
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response.
 */
export function estimateRoom(request: EstimateRoomRequest): RoomEstimateResult;
export function estimateRoom(
  samples: Float32Array,
  sampleRate?: number,
  options?: RoomEstimateOptions,
): RoomEstimateResult;
export function estimateRoom(
  samples: Float32Array | EstimateRoomRequest,
  sampleRate = 48000,
  options: RoomEstimateOptions = {},
): RoomEstimateResult {
  const module = requireModule();
  if (typeof module.estimateRoom !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('estimateRoom', request.samples, request.sampleRate ?? 48000, request);
  return module.estimateRoom(request.samples, request.sampleRate ?? 48000, request);
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
  sampleRate?: number,
  options: RoomMorphOptions = {},
): Float32Array {
  const module = requireModule();
  if (typeof module.roomMorph !== 'function') {
    throw new Error('libsonare was built without acoustic-simulation support');
  }
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  validateAnalysisInput('roomMorph', request.samples, request.sampleRate, request);
  return module.roomMorph(request.samples, request.sampleRate, request);
}

/**
 * Perform complete music analysis with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Complete analysis result
 */
export function analyzeWithProgress(request: AnalyzeWithProgressRequest): AnalysisResult;
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number | undefined,
  onProgress: ProgressCallback,
): AnalysisResult;
export function analyzeWithProgress(
  samples: Float32Array | AnalyzeWithProgressRequest,
  sampleRate = 22050,
  onProgress?: ProgressCallback,
): AnalysisResult {
  const request: AnalyzeWithProgressRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, onProgress: onProgress as ProgressCallback }
      : samples;
  validateAnalysisInput(
    'analyzeWithProgress',
    request.samples,
    request.sampleRate ?? 22050,
    request,
  );
  const result = requireModule().analyzeWithProgress(
    request.samples,
    request.sampleRate ?? 22050,
    request.onProgress ?? (() => {}),
    request.cancel ?? (() => false),
  );
  return convertAnalysisResult(result);
}

export interface BpmCandidate {
  bpm: number;
  confidence: number;
}

export interface BpmAnalysisResult {
  bpm: number;
  confidence: number;
  candidates: BpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

export interface RhythmAnalysisResult {
  timeSignature: { numerator: number; denominator: number; confidence: number };
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  bpm: number;
  beatIntervals: Float32Array;
}

export interface DynamicsAnalysisResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
  /** Loudness curve timestamps (seconds), parallel to {@link loudnessRmsDb}. */
  loudnessTimes: Float32Array;
  /** Loudness curve RMS values (dB), parallel to {@link loudnessTimes}. */
  loudnessRmsDb: Float32Array;
}

/** Timbre metrics for one analysis window. Entries are ordered by time in `timbreOverTime`. */
export interface TimbreFrame {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

export interface TimbreAnalysisResult extends TimbreFrame {
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
  /** Time-varying timbre metrics, one entry per analysis window. */
  timbreOverTime: TimbreFrame[];
}

/**
 * Detailed BPM analysis (BPM, confidence, alternate candidates, autocorrelation,
 * tempogram). Matches the Node `analyzeBpm` / Python `analyze_bpm` surface.
 */
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
  validateAnalysisInput('analyzeBpm', request.samples, request.sampleRate ?? 22050, request);
  assertNonNegativeInteger('analyzeBpm', request.maxCandidates ?? 5, 'maxCandidates');
  return requireModule().analyzeBpm(
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

/**
 * Detailed rhythm analysis (time signature, groove, syncopation, beat intervals).
 */
export function analyzeRhythm(request: AnalyzeRhythmRequest): RhythmAnalysisResult;
export function analyzeRhythm(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeRhythmOptions,
): RhythmAnalysisResult;
export function analyzeRhythm(
  samples: Float32Array | AnalyzeRhythmRequest,
  sampleRate = 22050,
  options: AnalyzeRhythmOptions = {},
): RhythmAnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('analyzeRhythm', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().analyzeRhythm(
    request.samples,
    request.sampleRate ?? 22050,
    request.bpmMin ?? 60.0,
    request.bpmMax ?? 200.0,
    request.startBpm ?? 120.0,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}

/**
 * Dynamics analysis (RMS, peak, crest factor, LRA, loudness curve).
 */
export function analyzeDynamics(request: AnalyzeDynamicsRequest): DynamicsAnalysisResult;
export function analyzeDynamics(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeDynamicsOptions,
): DynamicsAnalysisResult;
export function analyzeDynamics(
  samples: Float32Array | AnalyzeDynamicsRequest,
  sampleRate = 22050,
  options: AnalyzeDynamicsOptions = {},
): DynamicsAnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('analyzeDynamics', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().analyzeDynamics(
    request.samples,
    request.sampleRate ?? 22050,
    request.windowSec ?? 0.4,
    request.hopLength ?? 512,
    request.compressionThreshold ?? 6.0,
  );
}

/**
 * Timbre analysis (brightness/warmth/density/roughness/complexity plus spectral
 * features and per-window timbre frames).
 */
export function analyzeTimbre(request: AnalyzeTimbreRequest): TimbreAnalysisResult;
export function analyzeTimbre(
  samples: Float32Array,
  sampleRate?: number,
  options?: AnalyzeTimbreOptions,
): TimbreAnalysisResult;
export function analyzeTimbre(
  samples: Float32Array | AnalyzeTimbreRequest,
  sampleRate = 22050,
  options: AnalyzeTimbreOptions = {},
): TimbreAnalysisResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  validateAnalysisInput('analyzeTimbre', request.samples, request.sampleRate ?? 22050, request);
  return requireModule().analyzeTimbre(
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
 * Whether this WASM build was compiled with FFmpeg support. Mirrors Node /
 * Python `hasFfmpegSupport`. In the published WASM binding this currently
 * always returns `false` (FFmpeg is not bundled into the .wasm), but the API
 * exists so caller code can branch on capabilities portably.
 */
export function hasFfmpegSupport(): boolean {
  return requireModule().hasFfmpegSupport();
}
