import {
  chordChromaMethodValue,
  chordQualityOrdinal,
  convertAnalysisResult,
  convertChordAnalysisResult,
  convertKeyCandidate,
  genreHintValue,
  keyModeName,
  keyModeOrdinal,
  keyModeValues,
  keyProfileValue,
  pitchClassOrdinal,
} from './analysis_helpers.js';
import { ErrorCode, SonareError } from './errors.js';
import { getSonareModule } from './module_state.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeTimbreOptions,
  Beat,
  BpmCandidate,
  Chord,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChordFunction,
  ChordFunctionsInput,
  ChordFunctionsKey,
  DynamicsResult,
  FunctionalChord,
  GrooveType,
  KeyCandidate,
  KeyDetection,
  KeyDetectionOptions,
  KeyMode,
  MeterEstimate,
  PitchClass,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryFromEstimateOptions,
  RoomGeometryOptions,
  RoomMorphOptions,
  RoomMorphResult,
  RoomMorphStereoResult,
  TimbreResult,
} from './public_types.js';
import type { ProgressCallback, WasmAcousticResult } from './sonare.js';
import type { ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertFloatFields,
  assertFloatValue,
  assertNonNegativeFloatValue,
  assertNonNegativeInteger,
  assertNumber,
  assertU32,
  optionalBoolean,
  optionsBag,
  requestObject,
  sampleOneShot,
  tuningCentsFromNative,
  tuningCentsToNative,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** The tuning argument the module reads: a semitone fraction, or `'auto'` (absent or `'auto'`). */
function nativeTuning(cents: number | 'auto' | undefined | null): number | 'auto' {
  if (cents === undefined || cents === null || cents === 'auto') {
    return 'auto';
  }
  if (typeof cents === 'string') {
    throw new RangeError(`tuningCents must be a number of cents or 'auto', got '${cents}'`);
  }
  if (typeof cents !== 'number') {
    throw new TypeError("tuningCents must be a number of cents or 'auto'");
  }
  return tuningCentsToNative(cents);
}

type NamedSelectors<T> = Omit<T, 'materialPreset' | 'mode'> & {
  materialPreset?: number;
  mode?: number;
};

/**
 * Replaces a `materialPreset` / `mode` name with its integer value for the
 * module; a number is refused. The names are the core's, read from the module
 * so no second table exists here. A present `seed` that is not a uint32 is
 * refused here too, since the module would otherwise fall back to its default.
 */
function resolveAcousticSelectors<
  T extends { materialPreset?: unknown; mode?: unknown; seed?: unknown },
>(options: T): NamedSelectors<T> {
  if (options.seed !== undefined && options.seed !== null) {
    if (typeof options.seed !== 'number') {
      throw new TypeError('seed must be a number');
    }
    assertU32('acoustic', options.seed, 'seed');
  }
  if (options.materialPreset === undefined && options.mode === undefined) {
    return options as NamedSelectors<T>;
  }
  const tables = requireModule().acousticSelectorNames();
  const resolve = (key: 'materialPreset' | 'mode') => {
    const value = options[key];
    const names = tables[key];
    const valid = names.map((name) => `'${name}'`).join(', ');
    if (typeof value !== 'string') {
      throw new TypeError(`${key} must be one of ${valid} (a name, not a number)`);
    }
    const index = names.indexOf(value);
    if (index < 0) {
      throw new RangeError(`${key} must be one of ${valid}, got '${value}'`);
    }
    return index;
  };
  const resolved = { ...options };
  if (options.materialPreset !== undefined) {
    resolved.materialPreset = resolve('materialPreset');
  }
  if (options.mode !== undefined) {
    resolved.mode = resolve('mode');
  }
  return resolved as NamedSelectors<T>;
}

type GuardedOptions = ValidateOptions;

/** The audio and its sample rate, which every one-shot over samples takes. */
interface SampleInput {
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** The settings of a request, as the positional form takes them. */
type OptionsOf<R> = Omit<R, 'samples' | 'sampleRate'>;

/** Canonical request form for {@link detectBpm}. */
export interface DetectBpmRequest extends SampleInput, GuardedOptions {}

/** Canonical request form for {@link detectBeats}. */
export interface DetectBeatsRequest extends SampleInput, GuardedOptions {}

/** Canonical request form for {@link detectDownbeats}. */
export interface DetectDownbeatsRequest extends SampleInput, GuardedOptions {}

/** Peak-picking configuration for {@link detectOnsets}. */
export interface OnsetDetectOptions extends GuardedOptions {
  nFft?: number;
  hopLength?: number;
  /** Peak-picking threshold on the normalised onset envelope. */
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
export interface DetectOnsetsRequest extends SampleInput, OnsetDetectOptions {}

/** Canonical request form for key detection functions. */
export interface DetectKeyRequest extends SampleInput, KeyDetectionOptions {}
export interface DetectKeyCandidatesRequest extends SampleInput, KeyDetectionOptions {}

/** Canonical request form for chord detection. */
export interface DetectChordsRequest extends SampleInput, ChordDetectionOptions {}

/** Canonical request form for functional chord analysis. */
export interface ChordFunctionalAnalysisRequest extends DetectChordsRequest {
  /** Pitch class 0-11, e.g. {@link detectKey}'s `root`. */
  keyRoot: PitchClass;
  /** Mode name (`'major'`, `'minor'`, ...); defaults to `'major'`. */
  keyMode?: KeyMode;
}

/** Canonical request form for labelling known chords with their function in a key. */
export interface ChordFunctionsRequest<T extends ChordFunctionsInput = Chord> {
  /** `detectChords`' timed result or its `chords` array, or the same shape built by hand. */
  chords: readonly T[] | { chords: readonly T[] };
  key: ChordFunctionsKey;
}

/** Canonical request form for impulse-response analysis. */
export interface AnalyzeImpulseResponseRequest extends SampleInput, GuardedOptions {
  /** Octave bands; default 6. */
  nOctaveBands?: number;
  /** Minimum decay range in dB for a valid RT60 fit; default 30. */
  minDecayDb?: number;
}

/** Canonical request form for acoustic analysis. */
export interface DetectAcousticRequest extends SampleInput, AcousticOptions {}

/** Canonical request form for equivalent-room estimation. */
export interface EstimateRoomRequest extends RoomEstimateOptions, GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** The room fields of an estimate that {@link roomGeometryFromEstimate} reads. */
export type RoomGeometryEstimate = Pick<RoomEstimateResult, 'lengthM' | 'widthM' | 'heightM'> & {
  bandAbsorption?: Float32Array | number[];
};

/** Canonical request form for {@link roomGeometryFromEstimate}. */
export interface RoomGeometryFromEstimateRequest extends RoomGeometryFromEstimateOptions {
  estimate: RoomGeometryEstimate;
}

/** Canonical request form for room-reverb morphing. */
export interface RoomMorphRequest extends RoomMorphOptions, GuardedOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Canonical request form for stereo room-reverb morphing. */
export interface RoomMorphStereoRequest extends RoomMorphOptions, GuardedOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  /** Receiver spacing in metres, in (0, 4]; omitted = 0.5. */
  receiverSpacingM?: number;
}

/** Canonical request forms for detailed music-analysis APIs. */
export interface AnalyzeBpmRequest extends SampleInput, AnalyzeBpmOptions {}
export interface AnalyzeRhythmRequest extends SampleInput, AnalyzeRhythmOptions {}
export interface AnalyzeDynamicsRequest extends SampleInput, AnalyzeDynamicsOptions {}
export interface AnalyzeTimbreRequest extends SampleInput, AnalyzeTimbreOptions {}

// ============================================================================
// Quick API (High-level Analysis)
// ============================================================================

/**
 * Detect BPM from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Detected BPM
 * @throws {TypeError} when `sampleRate` is missing or not a number
 * @throws {RangeError} on empty or non-finite audio, or an out-of-range rate
 */
export function detectBpm(request: DetectBpmRequest): number;
export function detectBpm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBpmRequest>,
): number;
export function detectBpm(...args: unknown[]): number {
  const request = sampleOneShot<DetectBpmRequest>('detectBpm', args);
  assertAudioInput('detectBpm', request.samples, request.sampleRate, request);
  return requireModule().detectBpm(request.samples, request.sampleRate);
}

/**
 * Detect musical key from audio samples.
 *
 * The tuning is measured from the audio unless `tuningCents` says otherwise: a
 * value in cents in `[-50, 50)` (0 reads concert A440), or `'auto'`. The result
 * reports the tuning used.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Detected key and the tuning its chroma was built with
 * @throws {TypeError} on a wrong-typed field or a missing `sampleRate`
 * @throws {RangeError} on an unknown `profile`, `modes` or `genreHint` token, or audio out of domain
 */
export function detectKey(request: DetectKeyRequest): KeyDetection;
export function detectKey(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectKeyRequest>,
): KeyDetection;
export function detectKey(...args: unknown[]): KeyDetection {
  const request = sampleOneShot<DetectKeyRequest>('detectKey', args);
  assertAudioInput('detectKey', request.samples, request.sampleRate, request);
  const chromaHighPassHz = request.chromaHighPassHz ?? 80;
  assertFloatValue('detectKey', chromaHighPassHz, 'chromaHighPassHz');
  const result = requireModule()._detectKeyWithOptions(
    request.samples,
    request.sampleRate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    optionalBoolean('detectKey', request.useHpss, 'useHpss', false),
    optionalBoolean('detectKey', request.loudnessWeighted, 'loudnessWeighted', false),
    chromaHighPassHz,
    keyModeValues(request.modes),
    keyProfileValue(request.profile),
    genreHintValue(request.genreHint),
    nativeTuning(request.tuningCents),
  );
  return {
    root: result.root as PitchClass,
    mode: keyModeName(result.mode),
    confidence: result.confidence,
    name: result.name,
    shortName: result.shortName,
    tuningCents: tuningCentsFromNative(result.tuning),
  };
}

/**
 * Detect the ranked key candidates of an audio buffer.
 *
 * @throws {TypeError} on a wrong-typed field or a missing `sampleRate`
 * @throws {RangeError} on an unknown `profile`, `modes` or `genreHint` token, or audio out of domain
 */
export function detectKeyCandidates(request: DetectKeyCandidatesRequest): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectKeyCandidatesRequest>,
): KeyCandidate[];
export function detectKeyCandidates(...args: unknown[]): KeyCandidate[] {
  const request = sampleOneShot<DetectKeyCandidatesRequest>('detectKeyCandidates', args);
  assertAudioInput('detectKeyCandidates', request.samples, request.sampleRate, request);
  const chromaHighPassHz = request.chromaHighPassHz ?? 80;
  assertFloatValue('detectKeyCandidates', chromaHighPassHz, 'chromaHighPassHz');
  // The embind value marshalling returns an array whose constructor is not this
  // realm's Array; chaining .map() onto it propagates that constructor via
  // Symbol.species, leaving a result that structuredClone (and so postMessage to
  // a Worker) rejects with "could not be cloned". Array.from() re-roots it as a
  // plain Array before mapping.
  const candidates = requireModule()._detectKeyCandidates(
    request.samples,
    request.sampleRate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    optionalBoolean('detectKeyCandidates', request.useHpss, 'useHpss', false),
    optionalBoolean('detectKeyCandidates', request.loudnessWeighted, 'loudnessWeighted', false),
    chromaHighPassHz,
    keyModeValues(request.modes),
    keyProfileValue(request.profile),
    genreHintValue(request.genreHint),
    nativeTuning(request.tuningCents),
  );
  return Array.from(candidates, convertKeyCandidate);
}

/**
 * Detect onset times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Array of onset times in seconds
 */
export function detectOnsets(request: DetectOnsetsRequest): Float32Array;
export function detectOnsets(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectOnsetsRequest>,
): Float32Array;
export function detectOnsets(...args: unknown[]): Float32Array {
  const request = sampleOneShot<DetectOnsetsRequest>('detectOnsets', args);
  assertAudioInput('detectOnsets', request.samples, request.sampleRate, request);
  return requireModule().detectOnsets(request.samples, request.sampleRate, request);
}

/**
 * Detect beat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Array of beat times in seconds
 */
export function detectBeats(request: DetectBeatsRequest): Float32Array;
export function detectBeats(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBeatsRequest>,
): Float32Array;
export function detectBeats(...args: unknown[]): Float32Array {
  const request = sampleOneShot<DetectBeatsRequest>('detectBeats', args);
  assertAudioInput('detectBeats', request.samples, request.sampleRate, request);
  return requireModule().detectBeats(request.samples, request.sampleRate);
}

/**
 * Detect downbeat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Array of downbeat times in seconds
 */
export function detectDownbeats(request: DetectDownbeatsRequest): Float32Array;
export function detectDownbeats(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectDownbeatsRequest>,
): Float32Array;
export function detectDownbeats(...args: unknown[]): Float32Array {
  const request = sampleOneShot<DetectDownbeatsRequest>('detectDownbeats', args);
  assertAudioInput('detectDownbeats', request.samples, request.sampleRate, request);
  return requireModule().detectDownbeats(request.samples, request.sampleRate);
}

/**
 * Detect chords from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - Optional chord detection settings
 * @returns Detected chord segments
 * @throws {TypeError} on a wrong-typed field (`keyRoot` is a number, `keyMode` a name)
 * @throws {RangeError} on an out-of-domain `keyRoot`, `keyMode` or `chromaMethod`
 */
export function detectChords(request: DetectChordsRequest): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectChordsRequest>,
): ChordAnalysisResult;
export function detectChords(...args: unknown[]): ChordAnalysisResult {
  const request = sampleOneShot<DetectChordsRequest>('detectChords', args);
  assertAudioInput('detectChords', request.samples, request.sampleRate, request);
  const minDurationSec = request.minDurationSec ?? 0.3;
  const smoothingWindowSec = request.smoothingWindowSec ?? 2.0;
  assertFloatFields('detectChords', { minDurationSec, smoothingWindowSec });
  const result = requireModule().detectChords(
    request.samples,
    request.sampleRate,
    minDurationSec,
    smoothingWindowSec,
    request.threshold ?? 0.5,
    optionalBoolean('detectChords', request.useTriadsOnly, 'useTriadsOnly', true),
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    optionalBoolean('detectChords', request.useBeatSync, 'useBeatSync', true),
    optionalBoolean('detectChords', request.useHmm, 'useHmm', true),
    request.hmmBeamWidth ?? 24,
    optionalBoolean('detectChords', request.useKeyContext, 'useKeyContext', false),
    pitchClassOrdinal(request.keyRoot ?? 0, 'keyRoot'),
    keyModeOrdinal(request.keyMode ?? 'major', 'keyMode'),
    optionalBoolean('detectChords', request.detectInversions, 'detectInversions', false),
    chordChromaMethodValue(request.chromaMethod ?? 'cqt'),
    nativeTuning(request.tuningCents),
  );
  return convertChordAnalysisResult(result);
}

/**
 * Label chords that are already known with their Roman numeral and harmonic
 * function in a key. Nothing is re-detected.
 *
 * `chords` is {@link detectChords}' result or its `chords` array, or the same
 * shape built by hand (only `root` and `quality` are read). `key` is
 * `{ root, mode }`, e.g. {@link detectKey}'s result. Returns the same timed
 * entries with `romanNumeral` (`'I'`, `'V7'`, `'vi'`; `'N.C.'` for an unknown chord)
 * and `function` (`'tonic'`, `'subdominant'`, `'dominant'`, `'chromatic'` for a
 * root outside the key's scale, `'none'` for an unknown chord) added. Every mode
 * other than minor reads the major scale.
 */
export function chordFunctions<T extends ChordFunctionsInput>(
  request: ChordFunctionsRequest<T>,
): FunctionalChord<T>[] {
  requestObject('chordFunctions', request, 'request', true);
  const entries = 'chords' in request.chords ? request.chords.chords : request.chords;
  // The module reads each chord's quality as its native ordinal.
  const nativeEntries = entries.map((chord) => ({
    root: chord.root,
    quality: chordQualityOrdinal(chord.quality, 'quality'),
  }));
  const labels = requireModule().chordFunctions(
    nativeEntries,
    pitchClassOrdinal(request.key.root, 'key.root'),
    keyModeOrdinal(request.key.mode, 'key.mode'),
  );
  return entries.map((chord, i) => ({
    ...chord,
    romanNumeral: labels.roman[i],
    function: labels.functions[i] as ChordFunction,
  }));
}

/**
 * Functional (Roman-numeral) harmonic analysis of the detected chord
 * progression, relative to the given key. Mirrors the C-ABI
 * `sonare_chord_functional_analysis` and the Node/Python `chordFunctionalAnalysis`.
 *
 * Runs {@link detectChords} followed by {@link chordFunctions} in the given key.
 * `options` carries the required `keyRoot` in the positional form.
 *
 * @returns The timed chord entries with `romanNumeral` (e.g. "I", "IV", "V", "vi")
 *   and `function` added
 */
export function chordFunctionalAnalysis(request: ChordFunctionalAnalysisRequest): FunctionalChord[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<ChordFunctionalAnalysisRequest>,
): FunctionalChord[];
export function chordFunctionalAnalysis(...args: unknown[]): FunctionalChord[] {
  const request = sampleOneShot<ChordFunctionalAnalysisRequest>('chordFunctionalAnalysis', args);
  assertAudioInput('chordFunctionalAnalysis', request.samples, request.sampleRate, request);
  const minDurationSec = request.minDurationSec ?? 0.3;
  const smoothingWindowSec = request.smoothingWindowSec ?? 2.0;
  assertFloatFields('chordFunctionalAnalysis', { minDurationSec, smoothingWindowSec });
  const analysed = requireModule().chordFunctionalAnalysis(
    request.samples,
    pitchClassOrdinal(request.keyRoot, 'keyRoot'),
    keyModeOrdinal(request.keyMode ?? 'major', 'keyMode'),
    request.sampleRate,
    minDurationSec,
    smoothingWindowSec,
    request.threshold ?? 0.5,
    optionalBoolean('chordFunctionalAnalysis', request.useTriadsOnly, 'useTriadsOnly', true),
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    optionalBoolean('chordFunctionalAnalysis', request.useBeatSync, 'useBeatSync', true),
    optionalBoolean('chordFunctionalAnalysis', request.useHmm, 'useHmm', true),
    request.hmmBeamWidth ?? 24,
    optionalBoolean('chordFunctionalAnalysis', request.useKeyContext, 'useKeyContext', false),
    optionalBoolean('chordFunctionalAnalysis', request.detectInversions, 'detectInversions', false),
    chordChromaMethodValue(request.chromaMethod ?? 'cqt'),
    nativeTuning(request.tuningCents),
  );
  return convertChordAnalysisResult(analysed).chords.map((chord, i) => ({
    ...chord,
    romanNumeral: analysed.roman[i],
    function: analysed.functions[i] as ChordFunction,
  }));
}

/**
 * Options for {@link analyze}. Every field is optional and falls back to the
 * core default when omitted.
 */
export interface MusicAnalyzeOptions extends GuardedOptions {
  nFft?: number;
  hopLength?: number;
  /** Lowest tempo searched in BPM; default 60. */
  bpmMin?: number;
  /** Highest tempo searched in BPM; default 200. */
  bpmMax?: number;
  startBpm?: number;
  useTriadsOnly?: boolean;
  useHpss?: boolean;
  /** Chroma high-pass cutoff in Hz; absent = 80, 0 = no filter. */
  chromaHighPassHz?: number;
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
  /**
   * Tuning offset of the recording in cents; must be in `[-50, 50)`. Every
   * chroma the analysis builds (key, chords, sections) is shifted by it, so a
   * recording that is not at A440 reads its key and chords on its own pitch
   * grid. Omitted or `'auto'` measures it from the audio; pass 0 for concert
   * A440. The value used, given or measured, is the result's `tuningCents`;
   * {@link referenceHzToTuning} converts a reference pitch to this unit.
   */
  tuningCents?: number | 'auto';
}

/**
 * Canonical request form for {@link analyze}.
 *
 * `onProgress` and `cancel` are polled synchronously while the analysis runs.
 */
export interface AnalyzeRequest extends SampleInput, MusicAnalyzeOptions {
  /** Progress callback: `progress` in `[0, 1]` and the name of the running stage. */
  onProgress?: ProgressCallback;
  /** Polled between stages; return `true` to stop the analysis. */
  cancel?: () => boolean;
}

/**
 * Perform complete music analysis.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @returns Complete analysis result
 * @throws {TypeError} when `onProgress` or `cancel` is not a function
 *
 * @remarks
 * This call is synchronous and blocks until analysis completes. Unlike the
 * Node binding (which offers `analyzeAsync` on a libuv worker thread), the
 * WASM build runs on a single thread, so there is no non-blocking variant —
 * the DSP pipeline always runs to completion on the calling thread. To keep
 * the UI responsive for long inputs, drive this from a Web Worker and pass
 * `onProgress` to report progress.
 */
export function analyze(request: AnalyzeRequest): AnalysisResult;
export function analyze(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeRequest>,
): AnalysisResult;
export function analyze(...args: unknown[]): AnalysisResult {
  const request = sampleOneShot<AnalyzeRequest>('analyze', args);
  assertAudioInput('analyze', request.samples, request.sampleRate, request);
  const { onProgress, cancel, chromaHighPassHz, tuningCents, ...rest } = request;
  for (const [name, callback] of [
    ['onProgress', onProgress],
    ['cancel', cancel],
  ] as const) {
    if (callback !== undefined && callback !== null && typeof callback !== 'function') {
      throw new TypeError(`analyze: ${name} must be a function`);
    }
  }
  // Native keeps its own spellings for the high-pass cutoff and the tuning.
  const nativeOptions: Record<string, unknown> = { ...rest };
  if (chromaHighPassHz !== undefined) {
    if (chromaHighPassHz !== null) {
      assertFloatValue('analyze', chromaHighPassHz, 'chromaHighPassHz');
    }
    nativeOptions.chromaHighpassHz = chromaHighPassHz;
  }
  if (tuningCents !== undefined) {
    nativeOptions.tuning = nativeTuning(tuningCents);
  }
  const module = requireModule();
  const result =
    onProgress == null && cancel == null
      ? module.analyze(request.samples, request.sampleRate, nativeOptions)
      : module.analyzeWithProgress(
          request.samples,
          request.sampleRate,
          nativeOptions,
          onProgress ?? (() => {}),
          cancel ?? (() => false),
        );
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
  /**
   * Beats in time order: `timeSec` in seconds, non-decreasing, and the per-beat
   * accent value `strength`.
   *
   * @remarks
   * `AnalysisResult.beatObservations.onsetStrength` is the windowed value the
   * library's own downbeat pass scores, and `beats[].strength` is a single
   * unwindowed envelope frame; build the entries from whichever you mean to
   * score. Neither needs pre-scaling: the series is divided by its own maximum
   * before scoring, so only the accent contrast within it is read.
   *
   * A series assembled by hand from {@link onsetStrength} — one frame read at
   * each beat time — carries a sample-rate dependence neither of those has: a
   * hop counted in samples frames a different amount of time at each rate, so
   * one waveform sampled at 32000, 44100 and 48000 Hz has produced three
   * different winning numerators off beat times identical to the sample.
   * Widening the read to a window around the beat does not remove it. A browser
   * decodes at the output device's rate, so that is a different answer per
   * visitor for the same clip.
   */
  beats: readonly Beat[];
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
   * two in `[1, 32]`.
   *
   * @remarks
   * Reported as requested: whether a beat divides into three is measured from
   * energy *between* the beats, which per-beat accents do not carry, so a
   * compound meter is not resolvable here. A six accented 3+3 comes back with
   * this denominator and `grouping === [3, 3]`.
   */
  denominator?: number;
  /** Weight of the downbeat accent term (default: 1). */
  downbeatWeight?: number;
  /** Weight of the measure-periodicity term (default: 0.5). */
  measureWeight?: number;
  /** Weight of the subdivision term (default: 0.15). */
  subdivisionWeight?: number;
  /**
   * Score ratio above which a compound meter is preferred (default: 0.85).
   *
   * @remarks
   * Only consulted when there is a subdivision to measure, so it has no effect
   * here — this entry point scores per-beat accents alone.
   */
  compoundSubdivisionThreshold?: number;
}

/**
 * Estimate meter over a caller-supplied beat series.
 *
 * @param request - Beat series plus optional scoring configuration
 * @returns The selected signature, its downbeat phase and grouping, and the
 *   scored candidates
 * @throws {TypeError} when `beats` is not an array of `{ timeSec, strength }`
 *
 * @remarks
 * Scores only the per-beat strengths, so no audio and no frame-level onset
 * envelope is needed: an arbitrary span of an existing analysis can be scored
 * without re-running it. Build the entries from `beatObservations.onsetStrength`
 * rather than `beats[].strength` — see {@link EstimateMeterRequest.beats}.
 *
 * The result carries `grouping` alongside the numerator: how the bar divides
 * into accent groups of two and three beats, so a seven comes back as `[3, 2,
 * 2]` or `[2, 2, 3]` rather than as a bare seven.
 *
 * Check `searched` before reading anything as a detection: a series too short
 * to score any candidate reports a fixed fallback instead, and `candidateScores`
 * grows with the square root of how many beats were scored, so scores from
 * spans of different lengths are not directly comparable.
 */
export function estimateMeter(request: EstimateMeterRequest): MeterEstimate {
  requestObject('estimateMeter', request, 'request', true);
  const { beats, ...options } = request;
  if (!Array.isArray(beats)) {
    throw new TypeError('estimateMeter: beats must be an array of { timeSec, strength }');
  }
  const times: number[] = [];
  const strengths: number[] = [];
  beats.forEach((beat, i) => {
    if (typeof beat !== 'object' || beat === null) {
      throw new TypeError(`estimateMeter: beats[${i}] must be an object`);
    }
    assertNumber('estimateMeter', beat.timeSec, `beats[${i}].timeSec`);
    assertNumber('estimateMeter', beat.strength, `beats[${i}].strength`);
    times.push(beat.timeSec);
    strengths.push(beat.strength);
  });
  return requireModule().estimateMeter(times, strengths, options);
}

/**
 * Measure room acoustics from an impulse response.
 *
 * @throws {TypeError} when `sampleRate` is missing or `minDecayDb` is not a number
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 */
export function analyzeImpulseResponse(request: AnalyzeImpulseResponseRequest): AcousticResult;
export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeImpulseResponseRequest>,
): AcousticResult;
export function analyzeImpulseResponse(...args: unknown[]): AcousticResult {
  const request = sampleOneShot<AnalyzeImpulseResponseRequest>('analyzeImpulseResponse', args);
  // Only `undefined` takes the default. `null` falls through to
  // assertFiniteScalar, which refuses it with the same wording as any other
  // non-finite value.
  const resolvedMinDecayDb = request.minDecayDb === undefined ? 30.0 : request.minDecayDb;
  assertFiniteScalar('analyzeImpulseResponse', resolvedMinDecayDb, 'minDecayDb');
  assertAudioInput('analyzeImpulseResponse', request.samples, request.sampleRate, request);
  const result: WasmAcousticResult = requireModule().analyzeImpulseResponseEx(
    request.samples,
    request.sampleRate,
    request.nOctaveBands ?? 6,
    resolvedMinDecayDb,
  );
  return result;
}

/**
 * Detect room acoustic parameters from a recording.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 */
export function detectAcoustic(request: DetectAcousticRequest): AcousticResult;
export function detectAcoustic(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectAcousticRequest>,
): AcousticResult;
export function detectAcoustic(...args: unknown[]): AcousticResult {
  const request = sampleOneShot<DetectAcousticRequest>('detectAcoustic', args);
  assertAudioInput('detectAcoustic', request.samples, request.sampleRate, request);
  const result: WasmAcousticResult = requireModule().detectAcoustic(
    request.samples,
    request.sampleRate,
    request.nOctaveBands ?? 6,
    request.nThirdOctaveSubbands ?? 24,
    request.minDecayDb ?? 30.0,
    request.noiseFloorMarginDb ?? 10.0,
  );
  return result;
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
  const module = requireModule();
  if (typeof module.synthesizeRir !== 'function') {
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without acoustic-simulation support',
    );
  }
  return module.synthesizeRir(resolveAcousticSelectors(options));
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
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without acoustic-simulation support',
    );
  }
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...optionsBag('estimateRoom', options) }
      : requestObject('estimateRoom', samples);
  assertAudioInput('estimateRoom', request.samples, request.sampleRate ?? 48000, request);
  return module.estimateRoom(
    request.samples,
    request.sampleRate ?? 48000,
    resolveAcousticSelectors(request),
  );
}

/**
 * Turn a room estimate into the geometry {@link synthesizeRir} takes.
 *
 * The pair to {@link estimateRoom}: the estimate's `lengthM`, `widthM`,
 * `heightM` and `bandAbsorption` are already `synthesizeRir`'s names. The
 * result also carries a placement, the default source and listener positions
 * scaled per axis into the estimated room (the default 7 x 5 x 3 room maps to
 * the default placement exactly), and `absorption` as the mean of the finite
 * bands. `source` and `listener` override the scaled placement when given.
 * Absorption bands that did not converge are left out of `bandAbsorption`.
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
  const module = requireModule();
  if (typeof module.roomGeometryFromEstimate !== 'function') {
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without acoustic-simulation support',
    );
  }
  if (typeof estimate !== 'object' || estimate === null) {
    throw new TypeError('roomGeometryFromEstimate: estimate must be an object');
  }
  const request: RoomGeometryFromEstimateRequest =
    'estimate' in estimate
      ? requestObject('roomGeometryFromEstimate', estimate, 'request', true)
      : { estimate, ...optionsBag('roomGeometryFromEstimate', options) };
  if (typeof request.estimate !== 'object' || request.estimate === null) {
    throw new TypeError('roomGeometryFromEstimate: estimate must be an object');
  }
  const { bandAbsorption, lengthM, widthM, heightM } = request.estimate;
  const geometry: RoomGeometryOptions = module.roomGeometryFromEstimate({
    lengthM,
    widthM,
    heightM,
    ...(bandAbsorption === undefined ? {} : { bandAbsorption: Float32Array.from(bandAbsorption) }),
  });
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
  sampleRate?: number,
  options: RoomMorphOptions = {},
): RoomMorphResult {
  const module = requireModule();
  if (typeof module.roomMorph !== 'function') {
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without acoustic-simulation support',
    );
  }
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...optionsBag('roomMorph', options) }
      : requestObject('roomMorph', samples);
  assertAudioInput('roomMorph', request.samples, request.sampleRate, request);
  return module.roomMorph(request.samples, request.sampleRate, resolveAcousticSelectors(request));
}

/**
 * Morph a stereo recording's reverberation toward a target room, as heard by two
 * omnidirectional receivers `receiverSpacingM` apart (receiver spacing in
 * metres, in (0, 4]; omitted = 0.5).
 *
 * Returns both channels (input length plus the reverb tail) and the target-room
 * synthesis `diagnostics`, with the same codes as {@link RoomMorphResult}.
 * `sampleRate` defaults to 48000.
 *
 * @throws RangeError if the channel lengths differ or `receiverSpacingM` is
 *   non-finite or outside (0, 4].
 */
export function roomMorphStereo(request: RoomMorphStereoRequest): RoomMorphStereoResult {
  const module = requireModule();
  if (typeof module.roomMorphStereo !== 'function') {
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without acoustic-simulation support',
    );
  }
  requestObject('roomMorphStereo', request, 'request', true);
  const sampleRate = request.sampleRate ?? 48000;
  assertAudioInput('roomMorphStereo', request.left, sampleRate, request, 'left');
  assertAudioInput('roomMorphStereo', request.right, sampleRate, request, 'right');
  if (request.left.length !== request.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  const spacing = request.receiverSpacingM;
  if (spacing !== undefined && !(Number.isFinite(spacing) && spacing > 0 && spacing <= 4)) {
    throw new RangeError(
      `receiverSpacingM must be a finite number in (0, 4] metres; omitted = 0.5 (got ${spacing})`,
    );
  }
  return module.roomMorphStereo(
    request.left,
    request.right,
    sampleRate,
    resolveAcousticSelectors(request),
  );
}

export interface BpmAnalysisResult {
  bpm: number;
  /**
   * Winning tempo cluster's vote share within `bpmMin`–`bpmMax` (60–200 by default), searched at
   * the input rate. `analyze()` searches 60–200 BPM at 22050 Hz, so its `bpmConfidence` can differ.
   */
  confidence: number;
  /**
   * Chosen tempo first (`candidates[0].bpm === bpm`), then the other histogram
   * bins by descending vote share.
   */
  candidates: BpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

/**
 * Detailed BPM analysis (BPM, confidence, alternate candidates, autocorrelation,
 * tempogram). Matches the Node `analyzeBpm` / Python `analyze_bpm` surface.
 * Searches 60–200 BPM unless `bpmMin` / `bpmMax` say otherwise.
 */
export function analyzeBpm(request: AnalyzeBpmRequest): BpmAnalysisResult;
export function analyzeBpm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeBpmRequest>,
): BpmAnalysisResult;
export function analyzeBpm(...args: unknown[]): BpmAnalysisResult {
  const request = sampleOneShot<AnalyzeBpmRequest>('analyzeBpm', args);
  assertAudioInput('analyzeBpm', request.samples, request.sampleRate, request);
  assertNonNegativeInteger('analyzeBpm', request.maxCandidates ?? 5, 'maxCandidates');
  return requireModule().analyzeBpm(
    request.samples,
    request.sampleRate,
    request.bpmMin ?? 60.0,
    request.bpmMax ?? 200.0,
    request.startBpm ?? 120.0,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.maxCandidates ?? 5,
  );
}

/**
 * Detailed rhythm analysis (time signature, groove, syncopation, beat intervals).
 */
export function analyzeRhythm(request: AnalyzeRhythmRequest): RhythmResult;
export function analyzeRhythm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeRhythmRequest>,
): RhythmResult;
export function analyzeRhythm(...args: unknown[]): RhythmResult {
  const request = sampleOneShot<AnalyzeRhythmRequest>('analyzeRhythm', args);
  assertAudioInput('analyzeRhythm', request.samples, request.sampleRate, request);
  const result = requireModule().analyzeRhythm(
    request.samples,
    request.sampleRate,
    request.bpmMin ?? 60.0,
    request.bpmMax ?? 200.0,
    request.startBpm ?? 120.0,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
  return { ...result, grooveType: result.grooveType as GrooveType };
}

/**
 * Dynamics analysis (RMS, peak, crest factor, LRA, loudness curve).
 */
export function analyzeDynamics(request: AnalyzeDynamicsRequest): DynamicsResult;
export function analyzeDynamics(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeDynamicsRequest>,
): DynamicsResult;
export function analyzeDynamics(...args: unknown[]): DynamicsResult {
  const request = sampleOneShot<AnalyzeDynamicsRequest>('analyzeDynamics', args);
  assertAudioInput('analyzeDynamics', request.samples, request.sampleRate, request);
  const compressionThresholdDb = request.compressionThresholdDb ?? 6.0;
  assertNonNegativeFloatValue('analyzeDynamics', compressionThresholdDb, 'compressionThresholdDb');
  const result = requireModule().analyzeDynamics(
    request.samples,
    request.sampleRate,
    request.windowSec ?? 0.4,
    request.hopLength ?? 512,
    compressionThresholdDb,
  );
  return {
    dynamicRangeDb: result.dynamicRangeDb,
    peakDb: result.peakDb,
    rmsDb: result.rmsDb,
    crestFactor: result.crestFactor,
    loudnessRangeLu: result.loudnessRangeDb,
    isCompressed: result.isCompressed,
    loudnessTimesSec: result.loudnessTimes,
    loudnessRmsDb: result.loudnessRmsDb,
  };
}

/**
 * Timbre analysis (brightness/warmth/density/roughness/complexity plus spectral
 * features and per-window timbre frames).
 */
export function analyzeTimbre(request: AnalyzeTimbreRequest): TimbreResult;
export function analyzeTimbre(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeTimbreRequest>,
): TimbreResult;
export function analyzeTimbre(...args: unknown[]): TimbreResult {
  const request = sampleOneShot<AnalyzeTimbreRequest>('analyzeTimbre', args);
  assertAudioInput('analyzeTimbre', request.samples, request.sampleRate, request);
  return requireModule().analyzeTimbre(
    request.samples,
    request.sampleRate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.nMels ?? 128,
    request.nMfcc ?? 13,
    request.windowSec ?? 0.5,
  );
}
