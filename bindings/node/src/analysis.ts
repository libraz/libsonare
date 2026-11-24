import { libraryToken, ordinalToken, skewError } from './internal/core_tokens.js';
import { assertDeclaredKeys, refuse, refuseType, withEntry } from './internal/entry.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { KEY_MODES } from './internal/key_modes.js';
import { requireListBudget } from './internal/list_budget.js';
import { restoreNullMeans } from './internal/record_json.js';
import { withRespelled } from './internal/respell.js';
import { tempoRange } from './internal/tempo_range.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type { RequestFn } from './request_keys.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisChord,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  BeatObservations,
  BoundaryOptions,
  BoundaryResult,
  BpmAnalysisResult,
  BpmCandidateRelation,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  ChordFunction,
  ChordFunctionsInput,
  ChordFunctionsKey,
  DynamicsResult,
  FunctionalChord,
  Key,
  KeyCandidate,
  KeyDetection,
  KeyDetectionOptions,
  KeyMode,
  KeyProfile,
  MelodyOptions,
  MelodyPoint,
  MelodyResult,
  MeterEstimate,
  PitchClass,
  ProgressCallback,
  RhythmResult,
  RirResult,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometry,
  RoomMorphOptions,
  RoomMorphResult,
  RoomMorphStereoResult,
  RoomPlacement,
  Section,
  SectionAnalysis,
  SectionType,
  SynthesizeRirRequest,
  TimbreResult,
} from './types.js';
import { CHORD_QUALITIES } from './types_analysis.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertFloatFields,
  assertFloatValue,
  assertInt32,
  assertPitchClass,
  assertU32,
  requestOnly,
  sampleOneShot,
} from './validation.js';

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

const ACOUSTIC_MODES = ['auto', 'blind', 'impulseResponse'] as const;

/** The coordinates of a room placement. */
const PLACEMENT_KEYS = ['x', 'y', 'z'];

/** A placement the caller gave, checked; absent (`undefined` / `null`) stays `undefined`. */
function checkedPlacement(
  name: 'source' | 'listener',
  placement: unknown,
): RoomPlacement | undefined {
  if (isAbsent(placement)) {
    return undefined;
  }
  if (typeof placement !== 'object') {
    throw refuseType(`${name} must be an object with x, y and z`);
  }
  assertDeclaredKeys(placement, PLACEMENT_KEYS, name);
  const { x, y, z } = placement as Record<string, unknown>;
  assertFloatValue(x, `${name}.x`);
  assertFloatValue(y, `${name}.y`);
  assertFloatValue(z, `${name}.z`);
  return { x, y, z } as RoomPlacement;
}

/**
 * Checks the public room options before the addon reads them: `source` /
 * `listener` and `mode` are validated, and an explicit 0 on a zero-means-default
 * field is refused. Fractional `seed` / `nOctaveBands` are refused before the addon
 * narrows them onto that zero.
 */
function nativeRoomOptions(options: object): Record<string, unknown> {
  const given = options as Record<string, unknown>;
  const resolved: Record<string, unknown> = { ...given };
  const read = (field: string): number | undefined => {
    const value = given[field];
    if (isAbsent(value)) {
      return undefined;
    }
    if (typeof value !== 'number') {
      throw refuseType(`${field} must be a number`);
    }
    return value;
  };
  for (const field of ZERO_IS_DEFAULT_FIELDS) {
    if (read(field) === 0) {
      throw refuse(`${field} must not be 0; omit it for the default`);
    }
  }
  const seed = read('seed');
  if (seed !== undefined) {
    assertU32(seed, 'seed');
  }
  const nOctaveBands = read('nOctaveBands');
  if (nOctaveBands !== undefined) {
    assertInt32(nOctaveBands, 'nOctaveBands');
  }
  for (const name of ['source', 'listener'] as const) {
    const placement = checkedPlacement(name, given[name]);
    delete resolved[name];
    if (placement !== undefined) {
      resolved[name] = placement;
    }
  }
  if (given.mode !== undefined) {
    assertClosedToken(given.mode, ACOUSTIC_MODES, 'mode');
  }
  return resolved;
}

// The native layer answers in its own field names and ordinals; the facade
// reshapes every result here, so the raw shape is deliberately loose.
// biome-ignore lint/suspicious/noExplicitAny: raw addon objects
type Native = any;

/** Drops absent (`undefined` / `null`) entries so the addon sees an omitted key. */
function compact(object: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(object)) {
    if (!isAbsent(value)) {
      out[key] = value;
    }
  }
  return out;
}

function without(object: object, ...keys: string[]): Record<string, unknown> {
  const out: Record<string, unknown> = { ...object };
  for (const key of keys) {
    delete out[key];
  }
  return out;
}

const KEY_PROFILE_NATIVE: Readonly<Record<KeyProfile, string>> = {
  krumhanslSchmuckler: 'krumhansl-schmuckler',
  temperley: 'temperley',
  shaath: 'shaath',
  faraldoEdmt: 'faraldo-edmt',
  faraldoEdma: 'faraldo-edma',
  faraldoEdmm: 'faraldo-edmm',
  bellmanBudge: 'bellman-budge',
};

const GENRE_HINTS = ['auto', 'edm', 'electronic', 'dance', 'pop', 'classical', 'jazz'] as const;
const CHORD_CHROMA_METHODS: readonly ChordChromaMethod[] = ['stft', 'nnls', 'cqt'];
const BPM_CANDIDATE_RELATIONS: readonly BpmCandidateRelation[] = [
  'primary',
  'half',
  'double',
  'other',
];
const SECTION_TYPES: readonly SectionType[] = [
  'intro',
  'verse',
  'preChorus',
  'chorus',
  'bridge',
  'instrumental',
  'outro',
  'unknown',
];

/** The tuning argument the addon reads: cents, `'auto'`, or absent. */
function nativeTuning(tuningCents: unknown): number | 'auto' | undefined {
  if (isAbsent(tuningCents)) {
    return undefined;
  }
  if (tuningCents === 'auto') {
    return 'auto';
  }
  if (typeof tuningCents !== 'number') {
    throw refuseType(`tuningCents must be a number or 'auto'`);
  }
  return tuningCents;
}

function pitchClassOf(value: number): PitchClass {
  return value as PitchClass;
}

function chordFromNative(raw: Native): Chord {
  return {
    root: pitchClassOf(raw.root),
    bass: pitchClassOf(raw.bass),
    rootName: raw.rootName,
    bassName: raw.bassName,
    quality: ordinalToken(CHORD_QUALITIES, raw.quality, 'chord quality'),
    name: raw.name,
    startSec: raw.startSec,
    endSec: raw.endSec,
    durationSec: raw.durationSec,
    confidence: raw.confidence,
  };
}

function keyFromNative(raw: Native): Key {
  return { ...raw, root: pitchClassOf(raw.root) };
}

function sectionTypeFromNative(ordinal: unknown): SectionType {
  if (ordinal === undefined || ordinal === null) {
    throw skewError('analysis result section is missing its type field');
  }
  return ordinalToken(SECTION_TYPES, ordinal, 'section type');
}

function sectionFromNative(raw: Native): Section {
  return {
    type: sectionTypeFromNative(raw.type),
    name: raw.name,
    startSec: raw.startSec,
    endSec: raw.endSec,
    energyLevel: raw.energyLevel,
    confidence: raw.confidence,
    group: raw.group,
  };
}

// The standalone melody natives still use the 1.x keys.
function melodyPointFromNative(raw: Native): MelodyPoint {
  const point: MelodyPoint = { timeSec: raw.time, confidence: raw.confidence };
  if (raw.frequency > 0) {
    point.frequencyHz = raw.frequency;
  }
  return point;
}

// The analysis document already carries the public key names; only the coded
// enums (pitch class, chord quality, section type) are decoded here.
function analysisFromNative(raw: Native): AnalysisResult {
  const { chords, sections, ...rest } = restoreNullMeans(raw, 'analysis_result') as Native;
  return {
    ...rest,
    bpmCandidates: rest.bpmCandidates.map((candidate: Native) => ({
      ...candidate,
      relation: libraryToken(BPM_CANDIDATE_RELATIONS, candidate.relation, 'BPM candidate relation'),
    })),
    key: keyFromNative(raw.key),
    chords: chords.map(
      (c: Native): AnalysisChord => ({ ...chordFromNative(c), romanNumeral: c.romanNumeral }),
    ),
    sections: sections.map(sectionFromNative),
  };
}

function keyDetectionFromNative(raw: Native): KeyDetection {
  return keyFromNative(raw) as KeyDetection;
}

function chordAnalysisFromNative(raw: Native): ChordAnalysisResult {
  return { chords: raw.chords.map(chordFromNative), tuningCents: raw.tuningCents };
}

/** Peak-picking configuration for {@link detectOnsets}. */
export interface OnsetDetectOptions {
  /** FFT window in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Positive peak-picking threshold on the onset envelope. Omit for adaptive detection. */
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
export interface DetectOnsetsRequest extends OnsetDetectOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Tempo search range shared by {@link detectBpm} and {@link detectBeats}. */
export interface TempoRangeOptions {
  /** Lowest tempo searched, in BPM; default 60. Finite and positive, below `bpmMax`; a `RangeError` otherwise. */
  bpmMin?: number;
  /**
   * Highest tempo searched, in BPM; default 200. Finite and positive, above `bpmMin`; a
   * `RangeError` otherwise. A range other than the default is searched as given, and the
   * unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
}

/** Request for {@link detectBpm}. */
export interface DetectBpmRequest extends TempoRangeOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link detectBeats}. */
export interface DetectBeatsRequest extends TempoRangeOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link detectDownbeats}. */
export interface DetectDownbeatsRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request for {@link detectKey}. */
export interface DetectKeyRequest extends KeyDetectionOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link detectKeyCandidates}. */
export interface DetectKeyCandidatesRequest extends KeyDetectionOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request for {@link estimateRoom}. */
export interface EstimateRoomRequest extends RoomEstimateOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link roomMorph}. */
export interface RoomMorphRequest extends RoomMorphOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link roomMorphStereo}. */
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
/** The room fields of an estimate that {@link roomGeometryFromEstimate} reads. */
export type RoomGeometryEstimate = Pick<RoomEstimateResult, 'lengthM' | 'widthM' | 'heightM'> & {
  /** Per-band absorption coefficients of the estimated room. */
  bandAbsorption?: Float32Array | number[];
};
/** Request for {@link roomGeometryFromEstimate}. */
export interface RoomGeometryFromEstimateRequest {
  /** The room estimate to read the geometry from. */
  estimate: RoomGeometryEstimate;
  /** Replaces the source placement scaled into the estimated room. */
  source?: RoomPlacement;
  /** Replaces the listener placement scaled into the estimated room. */
  listener?: RoomPlacement;
}
/** Request for {@link chordFunctions}. */
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

/** Settings for {@link analyze}. Every field is optional and falls back to the core default when omitted. */
export interface MusicAnalyzeOptions {
  /** FFT window in samples at 22050 Hz, rescaled to the input rate. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest tempo searched, in BPM. Default `60`. */
  bpmMin?: number;
  /**
   * Highest tempo searched, in BPM. Default `200`. A range other than the default is searched as
   * given, and the unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
  /**
   * Finite, positive BPM prior that seeds beat tracking if the tempo search finds none.
   * `bpm` is absent when no tempo is measured. Default 120.
   */
  startBpm?: number;
  /** Detect triads only, with no seventh chords. Default `true`. */
  useTriadsOnly?: boolean;
  /** Apply harmonic-percussive separation before the chord chroma. Default `false`. */
  useHpss?: boolean;
  /** Chroma high-pass corner in Hz. Absent = 80; 0 = no filter. */
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
   * Tuning offset of the recording in cents; must be in `[-50, 50)`. Every chroma
   * the analysis builds (key, chords, sections) is shifted by it, so a recording
   * that is not at A440 reads its key and chords on its own pitch grid. Omitted or
   * `'auto'` measures it from the audio; pass 0 for concert A440. The value used,
   * given or measured, is the result's `tuningCents`; {@link referenceHzToTuning}
   * converts a reference pitch to this unit.
   */
  tuningCents?: number | 'auto';
}

/** Request for {@link analyze}: the settings plus audio and optional progress reporting. */
export interface AnalyzeRequest extends MusicAnalyzeOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Called with a progress value in `[0, 1]` and the current stage name; its return value is ignored. */
  onProgress?: ProgressCallback;
  /** Polled during analysis; return `true` to cancel cooperatively. */
  cancel?: () => boolean;
}

/** Request for {@link analyzeAsync}: {@link AnalyzeRequest} without the synchronous callbacks. */
export type AnalyzeAsyncRequest = Omit<AnalyzeRequest, 'onProgress' | 'cancel'>;

/** Request for {@link estimateMeter}. */
export interface EstimateMeterRequest {
  /** Beat-level accent evidence, usually from {@link AnalysisResult.beatObservations}. */
  observations: BeatObservations;
  /**
   * Meter numerators to score. At most 16 entries, each in `[2, 32]`; an empty
   * list is rejected rather than restoring the default. Widening the set does
   * not force a wider meter. Default `[3, 4, 6]`.
   */
  candidateNumerators?: number[];
  /**
   * Beat unit reported for the detected meter; a power of two in `[1, 32]`.
   * A six with a compound 3+3 grouping can be reported with denominator 8 when
   * the subdivision observations support it. Default 4.
   */
  denominator?: number;
  /** Weight on the accent at each measure's first beat. Default 1. */
  downbeatWeight?: number;
  /** Weight on measure-to-measure accent agreement. Default 0.5. */
  measureWeight?: number;
  /** Weight on the subdivision accent pattern. Default 0.15. */
  subdivisionWeight?: number;
  /**
   * Mean inter-beat accent as a fraction of the mean beat accent, at and above
   * which a six (or a three) is read as compound. Default 0.5.
   */
  compoundSubdivisionThreshold?: number;
}

/** Request for {@link analyzeSections}. */
export interface AnalyzeSectionsRequest extends AnalyzeSectionsOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link detectBoundaries}. */
export interface DetectBoundariesRequest extends BoundaryOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeMelody}. */
export interface AnalyzeMelodyRequest extends MelodyOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeBpm}. */
export interface AnalyzeBpmRequest extends AnalyzeBpmOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeRhythm}. */
export interface AnalyzeRhythmRequest extends AnalyzeRhythmOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeDynamics}. */
export interface AnalyzeDynamicsRequest extends AnalyzeDynamicsOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeImpulseResponse}. */
export interface AnalyzeImpulseResponseRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Octave bands. Default `6`. */
  nOctaveBands?: number;
  /** Minimum decay range in dB for a valid RT60 fit. Default `30`. */
  minDecayDb?: number;
}
/** Request for {@link detectAcoustic}. */
export interface DetectAcousticRequest extends AcousticOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link analyzeTimbre}. */
export interface AnalyzeTimbreRequest extends AnalyzeTimbreOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link detectChords}. */
export interface DetectChordsRequest extends ChordDetectionOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}
/** Request for {@link chordFunctionalAnalysis}. */
export interface ChordFunctionalAnalysisRequest
  extends Omit<ChordDetectionOptions, 'keyRoot' | 'keyMode'> {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Pitch class 0-11 of the key. */
  keyRoot: PitchClass;
  /** Mode of the key. Default `'major'`. */
  keyMode?: KeyMode;
}

/** Normalises either call form of a one-shot over audio and preflights the audio. */
function audioCall<R extends { samples: Float32Array; sampleRate: number }>(
  requestName: RequestFn,
  args: readonly unknown[],
): R {
  const request = sampleOneShot<R>(requestName, args);
  assertAudioInput(request.samples, request.sampleRate, request);
  return request;
}

/** The key-detection fields in the addon's spelling. */
function nativeKeyOptions(request: KeyDetectionOptions): Record<string, unknown> {
  const { profile, genreHint, modes } = request;
  if (!isAbsent(profile)) {
    assertClosedToken(profile, Object.keys(KEY_PROFILE_NATIVE) as KeyProfile[], 'profile');
  }
  if (!isAbsent(genreHint)) {
    assertClosedToken(genreHint, GENRE_HINTS, 'genreHint');
  }
  let nativeModes: unknown = modes;
  if (modes === 'majorMinor') {
    nativeModes = 'major-minor';
  } else if (Array.isArray(modes)) {
    if (modes.length === 0) {
      throw refuse(`modes must not be empty; omit it for the default mode set`);
    }
    requireListBudget(modes, 'modes');
    modes.forEach((mode, i) => {
      assertClosedToken(mode, KEY_MODES, `modes[${i}]`);
    });
  } else if (typeof modes === 'string') {
    if (modes !== 'all' && modes !== 'modal') {
      throw refuse('modes must be majorMinor, all, modal or an array of mode names');
    }
  } else if (!isAbsent(modes)) {
    throw refuseType('modes must be an array or mode-set string');
  }
  if (!isAbsent(request.chromaHighPassHz)) {
    assertFloatValue(request.chromaHighPassHz, 'chromaHighPassHz');
  }
  return compact({
    nFft: request.nFft,
    hopLength: request.hopLength,
    useHpss: request.useHpss,
    loudnessWeighted: request.loudnessWeighted,
    chromaHighPassHz: request.chromaHighPassHz,
    modes: nativeModes,
    profile: isAbsent(profile) ? undefined : KEY_PROFILE_NATIVE[profile],
    genreHint,
    tuningCents: nativeTuning(request.tuningCents),
  });
}

/** The music-analysis settings in the addon's spelling. */
function nativeAnalyzeOptions(request: AnalyzeRequest): Record<string, unknown> {
  if (!isAbsent(request.chromaHighPassHz)) {
    assertFloatValue(request.chromaHighPassHz, 'chromaHighPassHz');
  }
  return compact({
    ...without(request, 'samples', 'sampleRate', 'onProgress', 'cancel', 'tuningCents'),
    tuningCents: nativeTuning(request.tuningCents),
  });
}

/**
 * Detect the tempo in BPM; `undefined` when no tempo is found (silence).
 *
 * @throws TypeError for a wrong-typed argument or an omitted `sampleRate`;
 *   RangeError for empty or non-finite audio or an out-of-range rate.
 */
export function detectBpm(request: DetectBpmRequest): number | undefined;
export function detectBpm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBpmRequest>,
): number | undefined;
export function detectBpm(...args: unknown[]): number | undefined {
  return withEntry('detectBpm', () => {
    const request = audioCall<DetectBpmRequest>('detectBpm', args);
    const [bpmMin, bpmMax] = tempoRange(request);
    return addon.detectBpm(request.samples, request.sampleRate, bpmMin, bpmMax);
  });
}

/**
 * Detect the musical key. The tuning is measured from the audio unless
 * `tuningCents` says otherwise: an offset in cents in `[-50, 50)` (0 reads
 * concert A440), or `'auto'`. The result reports the tuning used.
 */
export function detectKey(request: DetectKeyRequest): KeyDetection;
export function detectKey(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectKeyRequest>,
): KeyDetection;
export function detectKey(...args: unknown[]): KeyDetection {
  return withEntry('detectKey', () => {
    const request = audioCall<DetectKeyRequest>('detectKey', args);
    return keyDetectionFromNative(
      addon.detectKey(request.samples, request.sampleRate, nativeKeyOptions(request)),
    );
  });
}

/** The ranked key candidates; the options are {@link detectKey}'s. */
export function detectKeyCandidates(request: DetectKeyCandidatesRequest): KeyCandidate[];
export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectKeyCandidatesRequest>,
): KeyCandidate[];
export function detectKeyCandidates(...args: unknown[]): KeyCandidate[] {
  return withEntry('detectKeyCandidates', () => {
    const request = audioCall<DetectKeyCandidatesRequest>('detectKeyCandidates', args);
    const candidates: Native[] = addon.detectKeyCandidates(
      request.samples,
      request.sampleRate,
      nativeKeyOptions(request),
    );
    return candidates.map((c) => ({ key: keyFromNative(c.key), correlation: c.correlation }));
  });
}

/** Beat times in seconds. */
export function detectBeats(request: DetectBeatsRequest): Float32Array;
export function detectBeats(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBeatsRequest>,
): Float32Array;
export function detectBeats(...args: unknown[]): Float32Array {
  return withEntry('detectBeats', () => {
    const request = audioCall<DetectBeatsRequest>('detectBeats', args);
    const [bpmMin, bpmMax] = tempoRange(request);
    return addon.detectBeats(request.samples, request.sampleRate, bpmMin, bpmMax);
  });
}

/** Downbeat times in seconds. */
export function detectDownbeats(request: DetectDownbeatsRequest): Float32Array;
export function detectDownbeats(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectDownbeatsRequest>,
): Float32Array;
export function detectDownbeats(...args: unknown[]): Float32Array {
  return withEntry('detectDownbeats', () => {
    const request = audioCall<DetectDownbeatsRequest>('detectDownbeats', args);
    return addon.detectDownbeats(request.samples, request.sampleRate);
  });
}

/** Onset times in seconds. */
export function detectOnsets(request: DetectOnsetsRequest): Float32Array;
export function detectOnsets(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectOnsetsRequest>,
): Float32Array;
export function detectOnsets(...args: unknown[]): Float32Array {
  return withEntry('detectOnsets', () => {
    const request = audioCall<DetectOnsetsRequest>('detectOnsets', args);
    return addon.detectOnsets(request.samples, request.sampleRate, request);
  });
}

function assertProgressCallbacks(request: AnalyzeRequest): void {
  for (const field of ['onProgress', 'cancel'] as const) {
    const value = request[field];
    if (!isAbsent(value) && typeof value !== 'function') {
      throw refuseType(`${field} must be a function`);
    }
  }
}

/**
 * Run the full music analysis. `onProgress` receives a progress value in `[0, 1]`
 * and the stage name, synchronously; `cancel` is polled and returns `true` to stop.
 */
export function analyze(request: AnalyzeRequest): AnalysisResult;
export function analyze(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeRequest>,
): AnalysisResult;
export function analyze(...args: unknown[]): AnalysisResult {
  return withEntry('analyze', () => {
    const request = audioCall<AnalyzeRequest>('analyze', args);
    assertProgressCallbacks(request);
    const options = nativeAnalyzeOptions(request);
    const { onProgress, cancel } = request;
    return analysisFromNative(
      addon.analyze(request.samples, request.sampleRate, options, onProgress, cancel),
    );
  });
}

/** The core names these fields under `beats`; the caller supplied `observations`. */
const METER_OBSERVATION_RESPELLED = {
  'beats.accent': { name: 'observations.accent', scale: 1 },
  'beats.hasAttack': { name: 'observations.hasAttack', scale: 1 },
  'beats.lowFrequencyEnergy': { name: 'observations.lowFrequencyEnergy', scale: 1 },
  'beats.subdivisionAccent': { name: 'observations.subdivisionAccent', scale: 1 },
};

const METER_OBSERVATION_KEYS = [
  'accent',
  'hasAttack',
  'lowFrequencyEnergy',
  'subdivisionAccent',
  'chordChange',
] as const satisfies readonly (keyof BeatObservations)[];

function meterFloatStream(value: unknown, name: string): Float32Array {
  if (!Array.isArray(value)) {
    throw refuseType(`${name} must be an array`);
  }
  requireListBudget(value, name);
  for (let i = 0; i < value.length; i += 1) {
    if (typeof value[i] !== 'number') {
      throw refuseType(`${name}[${i}] must be a number`);
    }
  }
  return Float32Array.from(value);
}

/**
 * Estimate meter from the provided beat-level observations. Pass a span of an
 * existing result's observations or observations collected elsewhere.
 *
 * Option values are validated by the core, so an out-of-range weight, a
 * denominator that is not a power of two, an empty `candidateNumerators`, or
 * observation streams with invalid lengths or values surfaces as a `RangeError`.
 *
 * The result carries `grouping` alongside the numerator: how the bar divides
 * into accent groups of two and three beats, so a seven comes back as
 * `[3, 2, 2]` or `[2, 2, 3]` rather than as a bare seven.
 */
export function estimateMeter(request: EstimateMeterRequest): MeterEstimate;
export function estimateMeter(...args: unknown[]): MeterEstimate {
  return withEntry('estimateMeter', () => {
    const request = requestOnly<EstimateMeterRequest>('estimateMeter', args);
    const { observations } = request;
    if (observations === null || typeof observations !== 'object' || Array.isArray(observations)) {
      throw refuseType('observations must be an object');
    }
    assertDeclaredKeys(observations, METER_OBSERVATION_KEYS, 'observations');
    if (!Array.isArray(observations.hasAttack)) {
      throw refuseType('observations.hasAttack must be an array of booleans');
    }
    const accent = meterFloatStream(observations.accent, 'observations.accent');
    requireListBudget(observations.hasAttack, 'observations.hasAttack');
    const lowFrequencyEnergy = meterFloatStream(
      observations.lowFrequencyEnergy,
      'observations.lowFrequencyEnergy',
    );
    const subdivisionAccent = meterFloatStream(
      observations.subdivisionAccent,
      'observations.subdivisionAccent',
    );
    return withRespelled(METER_OBSERVATION_RESPELLED, () =>
      addon.estimateMeter(
        accent,
        observations.hasAttack,
        lowFrequencyEnergy,
        subdivisionAccent,
        without(request, 'observations'),
      ),
    );
  });
}

/**
 * Synthesize a room impulse response from shoebox geometry.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} when the geometry, placement or timing is unusable (for
 *   example a source outside the room: the message names the refused value and
 *   the range it must lie in), on an out-of-range `sampleRate`, or
 *   on an explicit 0 where omission selects the default
 */
export function synthesizeRir(request: SynthesizeRirRequest): RirResult;
export function synthesizeRir(...args: unknown[]): RirResult {
  return withEntry('synthesizeRir', () => {
    const r = requestOnly<SynthesizeRirRequest>('synthesizeRir', args);
    assertInt32(r.sampleRate, 'sampleRate');
    return addon.synthesizeRir(nativeRoomOptions(r));
  });
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response. The volume scale is anchored by
 * `referenceAbsorption`; `confidence` reports how well the data support it.
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
    const r = sampleOneShot<EstimateRoomRequest>('estimateRoom', args);
    const native = nativeRoomOptions(r);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.estimateRoom(r.samples, r.sampleRate, native);
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
    if (typeof request.estimate !== 'object' || request.estimate === null) {
      throw refuseType('estimate must be an object');
    }
    const geometry = addon.roomGeometryFromEstimate(request.estimate) as RoomGeometry;
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
    const r = sampleOneShot<RoomMorphRequest>('roomMorph', args);
    const native = nativeRoomOptions(r);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.roomMorph(r.samples, r.sampleRate, native);
  });
}

/**
 * Stereo {@link roomMorph}: `left` and `right` are each convolved with the target
 * room's impulse response for its own omnidirectional receiver, the pair spaced
 * `receiverSpacingM` apart and centred on the listener, so the added room is not
 * a single centred mono reverb.
 *
 * `left` and `right` must have the same length. The result carries both channels
 * (input length plus the reverb tail) and the synthesis `diagnostics`.
 *
 * @throws {TypeError} when `sampleRate` is missing or `receiverSpacingM` is not a number
 * @throws {RangeError} on a length mismatch, `receiverSpacingM` outside (0, 4], an
 *   unusable room geometry, or an explicit 0 where omission selects the default
 */
export function roomMorphStereo(request: RoomMorphStereoRequest): RoomMorphStereoResult;
export function roomMorphStereo(...args: unknown[]): RoomMorphStereoResult {
  return withEntry('roomMorphStereo', () => {
    const req = requestOnly<RoomMorphStereoRequest>('roomMorphStereo', args);
    const spacing = req.receiverSpacingM;
    if (spacing !== undefined && typeof spacing !== 'number') {
      throw refuseType('receiverSpacingM must be a number');
    }
    const native = nativeRoomOptions(req);
    assertAudioInput(req.left, req.sampleRate, req, 'left');
    assertAudioInput(req.right, req.sampleRate, req, 'right');
    if (req.left.length !== req.right.length) {
      throw refuse('left and right must have the same length');
    }
    return addon.roomMorphStereo(req.left, req.right, req.sampleRate, native);
  });
}

/**
 * Asynchronous variant of {@link analyze}. Runs the DSP pipeline on a libuv
 * worker thread so the JS event loop is never blocked. The returned promise
 * resolves with the same shape as the synchronous version, and every validation
 * failure rejects it rather than throwing.
 */
export function analyzeAsync(request: AnalyzeAsyncRequest): Promise<AnalysisResult>;
export function analyzeAsync(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeAsyncRequest>,
): Promise<AnalysisResult>;
export function analyzeAsync(...args: unknown[]): Promise<AnalysisResult> {
  return withEntry('analyzeAsync', () => {
    try {
      const request = audioCall<AnalyzeAsyncRequest>('analyzeAsync', args);
      const options = nativeAnalyzeOptions(request as AnalyzeRequest);
      return addon
        .analyzeAsync(request.samples, request.sampleRate, options)
        .then(analysisFromNative);
    } catch (error) {
      return Promise.reject(error);
    }
  });
}

/** Detect song-structure sections (intro/verse/chorus/...). */
export function analyzeSections(request: AnalyzeSectionsRequest): SectionAnalysis;
export function analyzeSections(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeSectionsRequest>,
): SectionAnalysis;
export function analyzeSections(...args: unknown[]): SectionAnalysis {
  return withEntry('analyzeSections', () => {
    const request = audioCall<AnalyzeSectionsRequest>('analyzeSections', args);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const native: { sections: Native[]; form: string } = addon.analyzeSections(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.minSectionSec ?? 4.0,
      request.threshold,
    );
    return { sections: native.sections.map(sectionFromNative), form: native.form };
  });
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
 * analysis grid `frame` indexes rather than the input. `timeSec` is the
 * authoritative output.
 *
 * @example
 * ```ts
 * const { boundaries, noveltyCurve, noveltyPeak } = detectBoundaries({
 *   samples,
 *   sampleRate: 44100,
 * });
 * for (const boundary of boundaries) {
 *   console.log(boundary.timeSec, boundary.strength);
 * }
 * ```
 */
export function detectBoundaries(request: DetectBoundariesRequest): BoundaryResult;
export function detectBoundaries(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBoundariesRequest>,
): BoundaryResult;
export function detectBoundaries(...args: unknown[]): BoundaryResult {
  return withEntry('detectBoundaries', () => {
    const request = audioCall<DetectBoundariesRequest>('detectBoundaries', args);
    const raw: Native = addon.detectBoundaries(
      request.samples,
      request.sampleRate,
      compact(without(request, 'samples', 'sampleRate')),
    );
    // The frame count is the novelty curve's length; the result does not repeat it.
    const { nFrames: _nFrames, ...rest } = raw;
    return {
      ...rest,
      boundaries: raw.boundaries.map((b: Native) => ({
        timeSec: b.time,
        frame: b.frame,
        strength: b.strength,
      })),
    };
  });
}

/**
 * Extract the melody contour from monophonic audio.
 *
 * By default this uses plain per-frame YIN. Pass `{ usePyin: true }` for the
 * Viterbi-smoothed pYIN tracker (less prone to octave jumps). When pYIN is
 * active, `center` (default `true`) zero-pads by `frameLength / 2` so frame `i`
 * is centered at `i * hopLength` (matching `librosa.pyin(center=True)`);
 * `center` is ignored for plain YIN.
 */
export function analyzeMelody(request: AnalyzeMelodyRequest): MelodyResult;
export function analyzeMelody(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeMelodyRequest>,
): MelodyResult;
export function analyzeMelody(...args: unknown[]): MelodyResult {
  return withEntry('analyzeMelody', () => {
    const request = audioCall<AnalyzeMelodyRequest>('analyzeMelody', args);
    const raw: Native = addon.analyzeMelody(
      request.samples,
      request.sampleRate,
      request.f0MinHz ?? 65.0,
      request.f0MaxHz ?? 2093.0,
      request.frameLength ?? 2048,
      request.hopLength ?? 256,
      request.threshold ?? 0.1,
      request.usePyin ?? false,
      request.center ?? true,
    );
    const result: MelodyResult = { points: raw.points.map(melodyPointFromNative) };
    if (raw.pitchRangeOctaves !== undefined) {
      result.pitchRangeOctaves = raw.pitchRangeOctaves;
    }
    if (raw.pitchStability !== undefined) {
      result.pitchStability = raw.pitchStability;
    }
    if (raw.meanFrequency !== undefined) {
      result.meanFrequencyHz = raw.meanFrequency;
    }
    if (raw.vibratoRate !== undefined) {
      result.vibratoRateHz = raw.vibratoRate;
    }
    return result;
  });
}

/** Tempo estimate with candidates; searches 60-200 BPM unless `bpmMin` / `bpmMax` say otherwise. */
export function analyzeBpm(request: AnalyzeBpmRequest): BpmAnalysisResult;
export function analyzeBpm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeBpmRequest>,
): BpmAnalysisResult;
export function analyzeBpm(...args: unknown[]): BpmAnalysisResult {
  return withEntry('analyzeBpm', () => {
    const request = audioCall<AnalyzeBpmRequest>('analyzeBpm', args);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    // The domain (>= 0) is the C ABI's; this only keeps the addon's int narrowing from wrapping.
    assertInt32(request.maxCandidates ?? 5, 'maxCandidates');
    return addon.analyzeBpm(
      request.samples,
      request.sampleRate,
      request.bpmMin,
      request.bpmMax,
      fft.nFft,
      fft.hopLength,
      request.maxCandidates ?? 5,
    );
  });
}

/** Detailed rhythm analysis (time signature, groove, syncopation, beat intervals). */
export function analyzeRhythm(request: AnalyzeRhythmRequest): RhythmResult;
export function analyzeRhythm(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeRhythmRequest>,
): RhythmResult;
export function analyzeRhythm(...args: unknown[]): RhythmResult {
  return withEntry('analyzeRhythm', () => {
    const request = audioCall<AnalyzeRhythmRequest>('analyzeRhythm', args);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.analyzeRhythm(
      request.samples,
      request.sampleRate,
      request.bpmMin,
      request.bpmMax,
      request.startBpm,
      fft.nFft,
      fft.hopLength,
    );
  });
}

/** Dynamics analysis (RMS, peak, crest factor, LRA, loudness curve). */
export function analyzeDynamics(request: AnalyzeDynamicsRequest): DynamicsResult;
export function analyzeDynamics(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeDynamicsRequest>,
): DynamicsResult;
export function analyzeDynamics(...args: unknown[]): DynamicsResult {
  return withEntry('analyzeDynamics', () => {
    const request = audioCall<AnalyzeDynamicsRequest>('analyzeDynamics', args);
    return addon.analyzeDynamics(
      request.samples,
      request.sampleRate,
      request.windowSec ?? 0.4,
      request.hopLength ?? 512,
      request.compressionThresholdDb ?? 6.0,
    );
  });
}

/** Measure room acoustics from an impulse response. */
export function analyzeImpulseResponse(request: AnalyzeImpulseResponseRequest): AcousticResult;
export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeImpulseResponseRequest>,
): AcousticResult;
export function analyzeImpulseResponse(...args: unknown[]): AcousticResult {
  return withEntry('analyzeImpulseResponse', () => {
    const request = audioCall<AnalyzeImpulseResponseRequest>('analyzeImpulseResponse', args);
    return addon.analyzeImpulseResponse(
      request.samples,
      request.sampleRate,
      request.nOctaveBands ?? 6,
      request.minDecayDb ?? 30.0,
    );
  });
}

/** Detect room acoustic parameters from a recording. */
export function detectAcoustic(request: DetectAcousticRequest): AcousticResult;
export function detectAcoustic(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectAcousticRequest>,
): AcousticResult;
export function detectAcoustic(...args: unknown[]): AcousticResult {
  return withEntry('detectAcoustic', () => {
    const request = audioCall<DetectAcousticRequest>('detectAcoustic', args);
    return addon.detectAcoustic(
      request.samples,
      request.sampleRate,
      request.nOctaveBands ?? 6,
      request.nThirdOctaveSubbands ?? 24,
      request.minDecayDb ?? 30.0,
      request.noiseFloorMarginDb ?? 10.0,
    );
  });
}

/** Timbre analysis (brightness, warmth, density, roughness and related descriptors). */
export function analyzeTimbre(request: AnalyzeTimbreRequest): TimbreResult;
export function analyzeTimbre(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeTimbreRequest>,
): TimbreResult;
export function analyzeTimbre(...args: unknown[]): TimbreResult {
  return withEntry('analyzeTimbre', () => {
    const request = audioCall<AnalyzeTimbreRequest>('analyzeTimbre', args);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.analyzeTimbre(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nMels ?? 128,
      request.nMfcc ?? 13,
      request.windowSec ?? 0.5,
    );
  });
}

/** The chord parameters in the addon's positional order, defaults applied and validated. */
function resolveChordParams(request: ChordDetectionOptions): {
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
  chromaMethod: number;
  tuningCents: number;
  tuningAuto: boolean;
} {
  const fft = resolveFftOptions(request.nFft ?? 2048, request.hopLength ?? 512);
  const hmmBeamWidth = request.hmmBeamWidth ?? 24;
  const keyRoot = request.keyRoot ?? 0;
  assertPitchClass(keyRoot, 'keyRoot');
  const keyMode = request.keyMode ?? 'major';
  assertClosedToken(keyMode, KEY_MODES, 'keyMode');
  const chromaMethod = request.chromaMethod ?? 'cqt';
  assertClosedToken(chromaMethod, CHORD_CHROMA_METHODS, 'chromaMethod');
  const tuning = nativeTuning(request.tuningCents);
  const minDurationSec = request.minDurationSec ?? 0.3;
  const smoothingWindowSec = request.smoothingWindowSec ?? 2.0;
  assertFloatFields({ minDurationSec, smoothingWindowSec });
  return {
    minDuration: minDurationSec,
    smoothingWindow: smoothingWindowSec,
    threshold: request.threshold ?? 0.5,
    useTriadsOnly: request.useTriadsOnly ?? true,
    nFft: fft.nFft,
    hopLength: fft.hopLength,
    useBeatSync: request.useBeatSync ?? true,
    useHmm: request.useHmm ?? true,
    hmmBeamWidth,
    useKeyContext: request.useKeyContext ?? false,
    keyRoot,
    keyMode: KEY_MODES.indexOf(keyMode),
    detectInversions: request.detectInversions ?? false,
    chromaMethod: chromaMethod === 'stft' ? 0 : chromaMethod === 'nnls' ? 1 : 2,
    tuningCents: typeof tuning === 'number' ? tuning : 0,
    tuningAuto: tuning === undefined || tuning === 'auto',
  };
}

/**
 * Detect chords from mono samples.
 *
 * Takes either one request or `(samples, sampleRate, options?)`; a request plus an
 * extra positional argument is a `TypeError`.
 */
export function detectChords(request: DetectChordsRequest): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectChordsRequest>,
): ChordAnalysisResult;
export function detectChords(...args: unknown[]): ChordAnalysisResult {
  return withEntry('detectChords', () => {
    const request = audioCall<DetectChordsRequest>('detectChords', args);
    const p = resolveChordParams(request);
    return chordAnalysisFromNative(
      addon.detectChords(
        request.samples,
        request.sampleRate,
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
        p.chromaMethod,
        p.tuningCents,
        p.tuningAuto,
      ),
    );
  });
}

/**
 * Functional (Roman-numeral) chord analysis from mono samples: {@link detectChords}
 * followed by {@link chordFunctions} in the given key, returning the timed
 * entries with `romanNumeral` and `function` added.
 *
 * `keyRoot` is required and travels with the other settings, so the positional
 * form's `options` is required as well.
 */
export function chordFunctionalAnalysis(request: ChordFunctionalAnalysisRequest): FunctionalChord[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<ChordFunctionalAnalysisRequest>,
): FunctionalChord[];
export function chordFunctionalAnalysis(...args: unknown[]): FunctionalChord[] {
  return withEntry('chordFunctionalAnalysis', () => {
    const request = audioCall<ChordFunctionalAnalysisRequest>('chordFunctionalAnalysis', args);
    assertPitchClass(request.keyRoot, 'keyRoot');
    const p = resolveChordParams(request);
    const analysed: { chords: Native[]; roman: string[]; functions: ChordFunction[] } =
      addon.chordFunctionalAnalysis(
        request.samples,
        p.keyRoot,
        p.keyMode,
        request.sampleRate,
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
        p.chromaMethod,
        p.tuningCents,
        p.tuningAuto,
      );
    return analysed.chords.map((chord, i) => ({
      ...chordFromNative(chord),
      romanNumeral: analysed.roman[i],
      function: analysed.functions[i],
    }));
  });
}

/**
 * Label chords that are already known with their Roman numeral and harmonic
 * function in a key. Nothing is re-detected.
 *
 * `chords` is {@link detectChords}' result or its `chords` array, or the same
 * shape built by hand (only `root` and `quality` are read). `key` is
 * `{ root, mode }`, e.g. {@link detectKey}'s result. Returns the same timed
 * entries with `romanNumeral` (`'I'`, `'V7'`, `'vi'`; `'N.C.'` for an unknown
 * chord) and `function` (`'tonic'`, `'subdominant'`, `'dominant'`, `'chromatic'`
 * for a root outside the key's scale, `'none'` for an unknown chord) added. Every
 * mode other than minor reads the major scale.
 *
 * @throws TypeError when a root is not a number or a quality or mode is not a
 *   string; RangeError for a pitch class outside 0-11 or an unknown token.
 */
export function chordFunctions<T extends ChordFunctionsInput = Chord>(
  request: ChordFunctionsRequest<T>,
): FunctionalChord<T>[] {
  return withEntry('chordFunctions', () => {
    const { chords, key } = requestOnly<ChordFunctionsRequest<T>>('chordFunctions', [request]);
    if (typeof key !== 'object' || key === null) {
      throw refuseType('key must be an object');
    }
    if (typeof chords !== 'object' || chords === null) {
      throw refuseType('chords must be an array or a chord analysis result');
    }
    const entries = 'chords' in chords ? chords.chords : chords;
    if (!Array.isArray(entries)) {
      throw refuseType('chords must be an array');
    }
    assertPitchClass(key.root, 'key.root');
    assertClosedToken(key.mode, KEY_MODES, 'key.mode');
    requireListBudget(entries, 'chords');
    entries.forEach((chord, i) => {
      if (typeof chord !== 'object' || chord === null) {
        throw refuseType(`chords[${i}] must be an object`);
      }
      assertPitchClass(chord.root, `chords[${i}].root`);
      assertClosedToken(chord.quality, CHORD_QUALITIES, `chords[${i}].quality`);
    });
    const labels: { roman: string[]; functions: ChordFunction[] } = addon.chordFunctions(
      entries,
      key.root,
      key.mode,
    );
    return entries.map((chord, i) => ({
      ...chord,
      romanNumeral: labels.roman[i],
      function: labels.functions[i],
    }));
  });
}
