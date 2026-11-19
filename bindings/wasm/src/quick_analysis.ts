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
import { codedError, refuse, refuseType, withEntry } from './internal/entry.js';
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
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometry,
  RoomMorphOptions,
  RoomMorphResult,
  RoomMorphStereoResult,
  RoomPlacement,
  SynthesizeRirRequest,
  TimbreResult,
} from './public_types.js';
import type { ProgressCallback, WasmAcousticResult } from './sonare.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertFloatFields,
  assertFloatValue,
  assertFrequencyBounds,
  assertNonNegativeFloatValue,
  assertNonNegativeInteger,
  assertNumber,
  assertSampleRate,
  assertU32,
  optionalBoolean,
  requestObject,
  requestOnly,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** The tuning argument the module reads: cents, or `'auto'` (absent or `'auto'`). */
function nativeTuning(cents: number | 'auto' | undefined | null): number | 'auto' {
  if (cents === undefined || cents === null || cents === 'auto') {
    return 'auto';
  }
  if (typeof cents === 'string') {
    throw refuse(`tuningCents must be a number of cents or 'auto', got '${cents}'`);
  }
  if (typeof cents !== 'number') {
    throw refuseType("tuningCents must be a number of cents or 'auto'");
  }
  return cents;
}

type NamedSelectors<T> = Omit<T, 'materialPreset' | 'mode'> & {
  materialPreset?: number;
  mode?: number;
};

/** Fields whose 0 the core reads as "use the library default": absent is the default, 0 is refused. */
const ZERO_IS_DEFAULT_FIELDS = [
  'mixingTimeMs',
  'maxSec',
  'aspectHintLw',
  'aspectHintLh',
  'referenceAbsorption',
  'nOctaveBands',
  'minDecayDb',
  'noiseFloorMarginDb',
] as const;

/** Reads the room fields of one call: `undefined` and `null` are absent, a non-number is a `TypeError`. */
function optionalRoomNumber(options: object, field: string): number | undefined {
  const value = (options as Record<string, unknown>)[field];
  if (value === undefined || value === null) {
    return undefined;
  }
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be a number`);
  }
  return value;
}

/** A placement the caller gave, checked; absent (`undefined` / `null`) stays `undefined`. */
function checkedPlacement(
  name: 'source' | 'listener',
  placement: unknown,
): RoomPlacement | undefined {
  if (placement === undefined || placement === null) {
    return undefined;
  }
  if (typeof placement !== 'object') {
    throw refuseType(`${name} must be an object with x, y and z`);
  }
  const { x, y, z } = placement as Record<string, unknown>;
  assertFloatValue(x, `${name}.x`);
  assertFloatValue(y, `${name}.y`);
  assertFloatValue(z, `${name}.z`);
  return { x, y, z } as RoomPlacement;
}

/**
 * Turns the public room options into the module's: the `materialPreset` and
 * `mode` names become their integer values (a number is refused), a `source` /
 * `listener` is checked, and an explicit 0 on a zero-means-default field is refused. The names are the core's,
 * read from the module so no second table exists here.
 */
function nativeRoomOptions<T extends object>(options: T): NamedSelectors<T> {
  const resolved: Record<string, unknown> = { ...(options as Record<string, unknown>) };
  for (const field of ZERO_IS_DEFAULT_FIELDS) {
    const value = optionalRoomNumber(options, field);
    if (value === 0) {
      throw refuse(`${field} must not be 0; omit it for the default`);
    }
  }
  const seed = optionalRoomNumber(options, 'seed');
  if (seed !== undefined) {
    assertU32(seed, 'seed');
  }
  for (const name of ['source', 'listener'] as const) {
    const placement = checkedPlacement(name, (options as Record<string, unknown>)[name]);
    delete resolved[name];
    if (placement !== undefined) {
      resolved[name] = placement;
    }
  }
  const { materialPreset, mode } = options as { materialPreset?: unknown; mode?: unknown };
  if (materialPreset !== undefined || mode !== undefined) {
    const tables = requireModule().acousticSelectorNames();
    const resolve = (key: 'materialPreset' | 'mode', value: unknown) => {
      const names = tables[key];
      const valid = names.map((name: string) => `'${name}'`).join(', ');
      if (typeof value !== 'string') {
        throw refuseType(`${key} must be one of ${valid} (a name, not a number)`);
      }
      const index = names.indexOf(value);
      if (index < 0) {
        throw refuse(`${key} must be one of ${valid}, got '${value}'`);
      }
      return index;
    };
    if (materialPreset !== undefined) {
      resolved.materialPreset = resolve('materialPreset', materialPreset);
    }
    if (mode !== undefined) {
      resolved.mode = resolve('mode', mode);
    }
  }
  return resolved as NamedSelectors<T>;
}

/** Runs a room call whose refusal of its arguments is a `RangeError`, whatever layer refused. */
function refusingInvalidGeometry<T>(call: () => T): T {
  try {
    return call();
  } catch (error) {
    if (error instanceof SonareError && error.code === ErrorCode.InvalidParameter) {
      throw refuse(`${error.message}`);
    }
    throw error;
  }
}

/** The audio and its sample rate, which every one-shot over samples takes. */
interface SampleInput {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Tempo search range shared by {@link detectBpm} and {@link detectBeats}. */
export interface TempoRangeOptions {
  /** Lowest tempo searched, in BPM; default 60. Finite and positive, below `bpmMax`; a `RangeError` otherwise. */
  bpmMin?: number;
  /** Highest tempo searched, in BPM; default 200. Finite and positive, above `bpmMin`; a `RangeError` otherwise. */
  bpmMax?: number;
}

/** The search range as given, checked as one pair; an omitted bound is left for the module to fill with the library default. */
function tempoRange(request: TempoRangeOptions): [number | undefined, number | undefined] {
  assertFrequencyBounds(request.bpmMin, request.bpmMax, 'bpmMin', 'bpmMax');
  return [request.bpmMin, request.bpmMax];
}

/** Canonical request form for {@link detectBpm}. */
export interface DetectBpmRequest extends SampleInput, TempoRangeOptions {}

/** Canonical request form for {@link detectBeats}. */
export interface DetectBeatsRequest extends SampleInput, TempoRangeOptions {}

/** Canonical request form for {@link detectDownbeats}. */
export interface DetectDownbeatsRequest extends SampleInput {}

/** Peak-picking configuration for {@link detectOnsets}. */
export interface OnsetDetectOptions {
  /** FFT window in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Peak-picking threshold on the normalised onset envelope. */
  threshold?: number;
  /** Frames before a peak over which it must be the maximum. Default `1`. */
  preMax?: number;
  /** Frames after a peak over which it must be the maximum. Default `1`. */
  postMax?: number;
  /** Frames before a peak averaged for the local mean. Default `3`. */
  preAvg?: number;
  /** Frames after a peak averaged for the local mean. Default `4`. */
  postAvg?: number;
  /** Offset added to the local mean, in onset-envelope units. Default `0.06`. */
  delta?: number;
  /** Minimum frames between consecutive onsets. Default `1`. */
  wait?: number;
  /** Move each onset back to the preceding envelope minimum. Default `false`. */
  backtrack?: boolean;
  /** Frames searched backwards when `backtrack` is set. Default `10`. */
  backtrackRange?: number;
}
/** Request for {@link detectOnsets}. */
export interface DetectOnsetsRequest extends SampleInput, OnsetDetectOptions {}

/** Canonical request form for key detection functions. */
export interface DetectKeyRequest extends SampleInput, KeyDetectionOptions {}
/** Request for {@link detectKeyCandidates}. */
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
  chords:
    | readonly T[]
    | {
        /** The chords, in time order. */
        chords: readonly T[];
      };
  /** Key the chords are labelled against. */
  key: ChordFunctionsKey;
}

/** Canonical request form for impulse-response analysis. */
export interface AnalyzeImpulseResponseRequest extends SampleInput {
  /** Octave bands; default 6. */
  nOctaveBands?: number;
  /** Minimum decay range in dB for a valid RT60 fit; default 30. */
  minDecayDb?: number;
}

/** Canonical request form for acoustic analysis. */
export interface DetectAcousticRequest extends SampleInput, AcousticOptions {}

/** Canonical request form for equivalent-room estimation. */
export interface EstimateRoomRequest extends SampleInput, RoomEstimateOptions {}

/** The room fields of an estimate that {@link roomGeometryFromEstimate} reads. */
export type RoomGeometryEstimate = Pick<RoomEstimateResult, 'lengthM' | 'widthM' | 'heightM'> & {
  /** Per-band absorption coefficients of the estimated room. */
  bandAbsorption?: Float32Array | number[];
};

/** Canonical request form for {@link roomGeometryFromEstimate}. */
export interface RoomGeometryFromEstimateRequest {
  /** The room estimate to read the geometry from. */
  estimate: RoomGeometryEstimate;
  /** Replaces the source placement scaled into the estimated room. */
  source?: RoomPlacement;
  /** Replaces the listener placement scaled into the estimated room. */
  listener?: RoomPlacement;
}

/** Canonical request form for room-reverb morphing. */
export interface RoomMorphRequest extends SampleInput, RoomMorphOptions {}

/** Canonical request form for stereo room-reverb morphing. */
export interface RoomMorphStereoRequest extends RoomMorphOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples; same length as `left`. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Receiver spacing in metres, in (0, 4]; omitted = 0.5. */
  receiverSpacingM?: number;
}

/** Canonical request forms for detailed music-analysis APIs. */
export interface AnalyzeBpmRequest extends SampleInput, AnalyzeBpmOptions {}
/** Request for {@link analyzeRhythm}. */
export interface AnalyzeRhythmRequest extends SampleInput, AnalyzeRhythmOptions {}
/** Request for {@link analyzeDynamics}. */
export interface AnalyzeDynamicsRequest extends SampleInput, AnalyzeDynamicsOptions {}
/** Request for {@link analyzeTimbre}. */
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
  return withEntry('detectBpm', () => {
    const request = sampleOneShot<DetectBpmRequest>('detectBpm', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const [bpmMin, bpmMax] = tempoRange(request);
    return requireModule().detectBpm(request.samples, request.sampleRate, bpmMin, bpmMax);
  });
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
  return withEntry('detectKey', () => {
    const request = sampleOneShot<DetectKeyRequest>('detectKey', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const chromaHighPassHz = request.chromaHighPassHz ?? 80;
    assertFloatValue(chromaHighPassHz, 'chromaHighPassHz');
    const result = requireModule()._detectKeyWithOptions(
      request.samples,
      request.sampleRate,
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      optionalBoolean(request.useHpss, 'useHpss', false),
      optionalBoolean(request.loudnessWeighted, 'loudnessWeighted', false),
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
      tuningCents: result.tuningCents,
    };
  });
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
  return withEntry('detectKeyCandidates', () => {
    const request = sampleOneShot<DetectKeyCandidatesRequest>('detectKeyCandidates', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const chromaHighPassHz = request.chromaHighPassHz ?? 80;
    assertFloatValue(chromaHighPassHz, 'chromaHighPassHz');
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
      optionalBoolean(request.useHpss, 'useHpss', false),
      optionalBoolean(request.loudnessWeighted, 'loudnessWeighted', false),
      chromaHighPassHz,
      keyModeValues(request.modes),
      keyProfileValue(request.profile),
      genreHintValue(request.genreHint),
      nativeTuning(request.tuningCents),
    );
    return Array.from(candidates, convertKeyCandidate);
  });
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
  return withEntry('detectOnsets', () => {
    const request = sampleOneShot<DetectOnsetsRequest>('detectOnsets', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return requireModule().detectOnsets(request.samples, request.sampleRate, request);
  });
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
  return withEntry('detectBeats', () => {
    const request = sampleOneShot<DetectBeatsRequest>('detectBeats', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const [bpmMin, bpmMax] = tempoRange(request);
    return requireModule().detectBeats(request.samples, request.sampleRate, bpmMin, bpmMax);
  });
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
  return withEntry('detectDownbeats', () => {
    const request = sampleOneShot<DetectDownbeatsRequest>('detectDownbeats', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return requireModule().detectDownbeats(request.samples, request.sampleRate);
  });
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
  return withEntry('detectChords', () => {
    const request = sampleOneShot<DetectChordsRequest>('detectChords', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const minDurationSec = request.minDurationSec ?? 0.3;
    const smoothingWindowSec = request.smoothingWindowSec ?? 2.0;
    assertFloatFields({ minDurationSec, smoothingWindowSec });
    const result = requireModule().detectChords(
      request.samples,
      request.sampleRate,
      minDurationSec,
      smoothingWindowSec,
      request.threshold ?? 0.5,
      optionalBoolean(request.useTriadsOnly, 'useTriadsOnly', true),
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      optionalBoolean(request.useBeatSync, 'useBeatSync', true),
      optionalBoolean(request.useHmm, 'useHmm', true),
      request.hmmBeamWidth ?? 24,
      optionalBoolean(request.useKeyContext, 'useKeyContext', false),
      pitchClassOrdinal(request.keyRoot ?? 0, 'keyRoot'),
      keyModeOrdinal(request.keyMode ?? 'major', 'keyMode'),
      optionalBoolean(request.detectInversions, 'detectInversions', false),
      chordChromaMethodValue(request.chromaMethod ?? 'cqt'),
      nativeTuning(request.tuningCents),
    );
    return convertChordAnalysisResult(result);
  });
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
  return withEntry('chordFunctions', () => {
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
  });
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
  return withEntry('chordFunctionalAnalysis', () => {
    const request = sampleOneShot<ChordFunctionalAnalysisRequest>('chordFunctionalAnalysis', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const keyRoot = pitchClassOrdinal(request.keyRoot, 'keyRoot');
    const minDurationSec = request.minDurationSec ?? 0.3;
    const smoothingWindowSec = request.smoothingWindowSec ?? 2.0;
    assertFloatFields({ minDurationSec, smoothingWindowSec });
    const analysed = requireModule().chordFunctionalAnalysis(
      request.samples,
      keyRoot,
      keyModeOrdinal(request.keyMode ?? 'major', 'keyMode'),
      request.sampleRate,
      minDurationSec,
      smoothingWindowSec,
      request.threshold ?? 0.5,
      optionalBoolean(request.useTriadsOnly, 'useTriadsOnly', true),
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      optionalBoolean(request.useBeatSync, 'useBeatSync', true),
      optionalBoolean(request.useHmm, 'useHmm', true),
      request.hmmBeamWidth ?? 24,
      optionalBoolean(request.useKeyContext, 'useKeyContext', false),
      optionalBoolean(request.detectInversions, 'detectInversions', false),
      chordChromaMethodValue(request.chromaMethod ?? 'cqt'),
      nativeTuning(request.tuningCents),
    );
    return convertChordAnalysisResult(analysed).chords.map((chord, i) => ({
      ...chord,
      romanNumeral: analysed.roman[i],
      function: analysed.functions[i] as ChordFunction,
    }));
  });
}

/**
 * Options for {@link analyze}. Every field is optional and falls back to the
 * core default when omitted.
 */
export interface MusicAnalyzeOptions {
  /** FFT window in samples at 22050 Hz, rescaled to the input rate. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest tempo searched in BPM; default 60. */
  bpmMin?: number;
  /** Highest tempo searched in BPM; default 200. */
  bpmMax?: number;
  /** Prior tempo estimate in BPM. Default `120`. */
  startBpm?: number;
  /** Detect triads only, with no seventh chords. Default `true`. */
  useTriadsOnly?: boolean;
  /** Apply harmonic-percussive separation before the chord chroma. Default `false`. */
  useHpss?: boolean;
  /** Chroma high-pass cutoff in Hz; absent = 80, 0 = no filter. */
  chromaHighPassHz?: number;
  /** Combine the chroma with a bass weighting. Default `true`. */
  useBassWeighted?: boolean;
  /** Chroma hop as a multiple of `hopLength`; larger is faster. Default `4`. */
  chromaHopMultiplier?: number;
  /** Smooth the chord sequence with a Viterbi HMM. Default `true`. */
  useChordHmm?: boolean;
  /** Bias the chord HMM transitions with the detected key. Default `false`. */
  useChordKeyContext?: boolean;
  /** Candidate beam width of the chord HMM. Default `24`. */
  chordHmmBeamWidth?: number;
  /** Estimate slash-chord bass notes. Default `false`. */
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
  return withEntry('analyze', () => {
    const request = sampleOneShot<AnalyzeRequest>('analyze', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const { onProgress, cancel, tuningCents, ...rest } = request;
    for (const [name, callback] of [
      ['onProgress', onProgress],
      ['cancel', cancel],
    ] as const) {
      if (callback !== undefined && callback !== null && typeof callback !== 'function') {
        throw refuseType(`${name} must be a function`);
      }
    }
    if (rest.chromaHighPassHz !== undefined && rest.chromaHighPassHz !== null) {
      assertFloatValue(rest.chromaHighPassHz, 'chromaHighPassHz');
    }
    const nativeOptions: Record<string, unknown> = { ...rest };
    if (tuningCents !== undefined) {
      nativeOptions.tuningCents = nativeTuning(tuningCents);
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
  });
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
  return withEntry('estimateMeter', () => {
    requestObject('estimateMeter', request, 'request', true);
    const { beats, ...options } = request;
    if (!Array.isArray(beats)) {
      throw refuseType('beats must be an array of { timeSec, strength }');
    }
    const times: number[] = [];
    const strengths: number[] = [];
    beats.forEach((beat, i) => {
      if (typeof beat !== 'object' || beat === null) {
        throw refuseType(`beats[${i}] must be an object`);
      }
      assertNumber(beat.timeSec, `beats[${i}].timeSec`);
      assertNumber(beat.strength, `beats[${i}].strength`);
      times.push(beat.timeSec);
      strengths.push(beat.strength);
    });
    return requireModule().estimateMeter(times, strengths, options);
  });
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
  return withEntry('analyzeImpulseResponse', () => {
    const request = sampleOneShot<AnalyzeImpulseResponseRequest>('analyzeImpulseResponse', args);
    // Only `undefined` takes the default. `null` falls through to
    // assertFiniteScalar, which refuses it with the same wording as any other
    // non-finite value.
    const resolvedMinDecayDb = request.minDecayDb === undefined ? 30.0 : request.minDecayDb;
    assertFiniteScalar(resolvedMinDecayDb, 'minDecayDb');
    assertAudioInput(request.samples, request.sampleRate, request);
    const result: WasmAcousticResult = requireModule().analyzeImpulseResponseEx(
      request.samples,
      request.sampleRate,
      request.nOctaveBands ?? 6,
      resolvedMinDecayDb,
    );
    return result;
  });
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
  return withEntry('detectAcoustic', () => {
    const request = sampleOneShot<DetectAcousticRequest>('detectAcoustic', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const result: WasmAcousticResult = requireModule().detectAcoustic(
      request.samples,
      request.sampleRate,
      request.nOctaveBands ?? 6,
      request.nThirdOctaveSubbands ?? 24,
      request.minDecayDb ?? 30.0,
      request.noiseFloorMarginDb ?? 10.0,
    );
    return result;
  });
}

/**
 * Synthesize a room impulse response from shoebox geometry.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} when the geometry, placement or timing is unusable (for
 *   example a source outside the room: the message leads with the diagnostic code,
 *   such as `acoustic.source_outside_room`), on an out-of-range `sampleRate`, or
 *   on an explicit 0 where omission selects the default
 */
export function synthesizeRir(request: SynthesizeRirRequest): RirResult;
export function synthesizeRir(...args: unknown[]): RirResult {
  return withEntry('synthesizeRir', () => {
    const r = requestOnly<SynthesizeRirRequest>('synthesizeRir', args);
    assertSampleRate(r.sampleRate);
    const module = requireModule();
    if (typeof module.synthesizeRir !== 'function') {
      throw codedError(
        ErrorCode.NotSupported,
        'libsonare was built without acoustic-simulation support',
      );
    }
    const native = withEntry('synthesizeRir', () => nativeRoomOptions(r));
    const synthesize = module.synthesizeRir;
    return refusingInvalidGeometry(() => synthesize.call(module, native));
  });
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, an
 *   unknown `mode`, or an explicit 0 where omission selects the default
 */
export function estimateRoom(request: EstimateRoomRequest): RoomEstimateResult;
export function estimateRoom(
  samples: Float32Array,
  sampleRate: number,
  options?: RoomEstimateOptions,
): RoomEstimateResult;
export function estimateRoom(...args: unknown[]): RoomEstimateResult {
  return withEntry('estimateRoom', () => {
    const module = requireModule();
    if (typeof module.estimateRoom !== 'function') {
      throw codedError(
        ErrorCode.NotSupported,
        'libsonare was built without acoustic-simulation support',
      );
    }
    const r = sampleOneShot<EstimateRoomRequest>('estimateRoom', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return module.estimateRoom(
      r.samples,
      r.sampleRate,
      withEntry('estimateRoom', () => nativeRoomOptions(r)),
    );
  });
}

/**
 * Turn a room estimate into the geometry {@link synthesizeRir} takes.
 *
 * The pair to {@link estimateRoom}: the estimate's `lengthM`, `widthM`,
 * `heightM` and `bandAbsorption` are already `synthesizeRir`'s names. The
 * result also carries a placement, the default `source` and `listener`
 * positions scaled per axis into the estimated room (the default 7 x 5 x 3 room
 * maps to the default placement exactly), and `absorption` as the mean of the
 * finite bands. `source` and `listener` of the request override the scaled
 * placement when given. Absorption bands that did not converge are left out of
 * `bandAbsorption`.
 *
 * @throws SonareError when the estimate has no measurable dimensions (NaN).
 */
export function roomGeometryFromEstimate(request: RoomGeometryFromEstimateRequest): RoomGeometry;
export function roomGeometryFromEstimate(...args: unknown[]): RoomGeometry {
  return withEntry('roomGeometryFromEstimate', () => {
    const request = requestOnly<RoomGeometryFromEstimateRequest>('roomGeometryFromEstimate', args);
    const module = requireModule();
    if (typeof module.roomGeometryFromEstimate !== 'function') {
      throw codedError(
        ErrorCode.NotSupported,
        'libsonare was built without acoustic-simulation support',
      );
    }
    if (typeof request.estimate !== 'object' || request.estimate === null) {
      throw refuseType('estimate must be an object');
    }
    const { bandAbsorption, lengthM, widthM, heightM } = request.estimate;
    const geometry = module.roomGeometryFromEstimate({
      lengthM,
      widthM,
      heightM,
      ...(bandAbsorption === undefined
        ? {}
        : { bandAbsorption: Float32Array.from(bandAbsorption) }),
    }) as RoomGeometry;
    for (const name of ['source', 'listener'] as const) {
      const placement = checkedPlacement(name, request[name]);
      if (placement !== undefined) {
        geometry[name] = placement;
      }
    }
    return geometry;
  });
}

/**
 * Morph a recording's reverberation toward a target room (creative FX, not
 * dereverberation).
 *
 * Returns the morphed samples in `audio` (input length plus the target room's
 * reverb tail) alongside the target-room synthesis's own `diagnostics`, which
 * report a room other than the one requested — see {@link RoomMorphResult}.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, an
 *   unusable room geometry, or an explicit 0 where omission selects the default
 */
export function roomMorph(request: RoomMorphRequest): RoomMorphResult;
export function roomMorph(
  samples: Float32Array,
  sampleRate: number,
  options?: RoomMorphOptions,
): RoomMorphResult;
export function roomMorph(...args: unknown[]): RoomMorphResult {
  return withEntry('roomMorph', () => {
    const module = requireModule();
    if (typeof module.roomMorph !== 'function') {
      throw codedError(
        ErrorCode.NotSupported,
        'libsonare was built without acoustic-simulation support',
      );
    }
    const r = sampleOneShot<RoomMorphRequest>('roomMorph', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const native = withEntry('roomMorph', () => nativeRoomOptions(r));
    const morph = module.roomMorph;
    return refusingInvalidGeometry(() => morph.call(module, r.samples, r.sampleRate, native));
  });
}

/**
 * Morph a stereo recording's reverberation toward a target room, as heard by two
 * omnidirectional receivers `receiverSpacingM` apart (receiver spacing in
 * metres, in (0, 4]; omitted = 0.5).
 *
 * Returns both channels (input length plus the reverb tail) and the target-room
 * synthesis `diagnostics`, with the same codes as {@link RoomMorphResult}.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} if the channel lengths differ, `receiverSpacingM` is
 *   non-finite or outside (0, 4], or the room geometry is unusable.
 */
export function roomMorphStereo(request: RoomMorphStereoRequest): RoomMorphStereoResult;
export function roomMorphStereo(...args: unknown[]): RoomMorphStereoResult {
  return withEntry('roomMorphStereo', () => {
    const module = requireModule();
    if (typeof module.roomMorphStereo !== 'function') {
      throw codedError(
        ErrorCode.NotSupported,
        'libsonare was built without acoustic-simulation support',
      );
    }
    const request = requestOnly<RoomMorphStereoRequest>('roomMorphStereo', args);
    assertAudioInput(request.left, request.sampleRate, request, 'left');
    assertAudioInput(request.right, request.sampleRate, request, 'right');
    if (request.left.length !== request.right.length) {
      throw refuse('Stereo channel lengths must match.');
    }
    const spacing = request.receiverSpacingM;
    if (spacing !== undefined && !(Number.isFinite(spacing) && spacing > 0 && spacing <= 4)) {
      throw refuse(
        `receiverSpacingM must be a finite number in (0, 4] metres; omitted = 0.5 (got ${spacing})`,
      );
    }
    const native = withEntry('roomMorphStereo', () => nativeRoomOptions(request));
    const morphStereo = module.roomMorphStereo;
    return refusingInvalidGeometry(() =>
      morphStereo.call(module, request.left, request.right, request.sampleRate, native),
    );
  });
}

/** Result of {@link analyzeBpm}: the chosen tempo with its alternatives and tempo curves. */
export interface BpmAnalysisResult {
  /** Detected tempo in BPM. */
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
  /** Autocorrelation of the onset envelope over tempo lags. */
  autocorrelation: Float32Array;
  /** Tempogram of the onset envelope. */
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
  return withEntry('analyzeBpm', () => {
    const request = sampleOneShot<AnalyzeBpmRequest>('analyzeBpm', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    assertNonNegativeInteger(request.maxCandidates ?? 5, 'maxCandidates');
    return requireModule().analyzeBpm(
      request.samples,
      request.sampleRate,
      request.bpmMin,
      request.bpmMax,
      request.startBpm,
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      request.maxCandidates ?? 5,
    );
  });
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
  return withEntry('analyzeRhythm', () => {
    const request = sampleOneShot<AnalyzeRhythmRequest>('analyzeRhythm', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const result = requireModule().analyzeRhythm(
      request.samples,
      request.sampleRate,
      request.bpmMin,
      request.bpmMax,
      request.startBpm,
      request.nFft ?? 2048,
      request.hopLength ?? 512,
    );
    return { ...result, grooveType: result.grooveType as GrooveType };
  });
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
  return withEntry('analyzeDynamics', () => {
    const request = sampleOneShot<AnalyzeDynamicsRequest>('analyzeDynamics', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const compressionThresholdDb = request.compressionThresholdDb ?? 6.0;
    assertNonNegativeFloatValue(compressionThresholdDb, 'compressionThresholdDb');
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
      loudnessRangeLu: result.loudnessRangeLu,
      isCompressed: result.isCompressed,
      loudnessTimesSec: result.loudnessTimesSec,
      loudnessRmsDb: result.loudnessRmsDb,
    };
  });
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
  return withEntry('analyzeTimbre', () => {
    const request = sampleOneShot<AnalyzeTimbreRequest>('analyzeTimbre', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return requireModule().analyzeTimbre(
      request.samples,
      request.sampleRate,
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      request.nMels ?? 128,
      request.nMfcc ?? 13,
      request.windowSec ?? 0.5,
    );
  });
}
