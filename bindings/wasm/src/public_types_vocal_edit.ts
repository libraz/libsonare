/** Public value types for the monophonic vocal edit surface. */

/** A half-open sample range `[startSample, endSample)` in destination time. */
export interface VocalRange {
  /** First sample of the range. */
  startSample: number;
  /** One past the last sample of the range. */
  endSample: number;
}

/**
 * Identifies the exact session state a result was produced from.
 *
 * The counters (`sessionEpoch`, `revision`, `draftId`, `generation`, `requestId`) are
 * unsigned 64-bit values that can exceed 2^53, so they travel as canonical decimal
 * strings. They are compared for equality or order and never used in arithmetic.
 */
export interface VocalStateToken {
  /** Changes whenever a session is created or restored. Decimal string (64-bit counter). */
  sessionEpoch: string;
  /** Committed revision; every commit, undo and redo advances it. Decimal string (64-bit counter). */
  revision: string;
  /** Identifies the open draft, or `'0'` when the token describes committed state. */
  draftId: string;
  /** Advances with every applied operation batch of a draft. Decimal string (64-bit counter). */
  generation: string;
  /** The `requestId` of the render this token accompanies, or `'0'`. Decimal string (64-bit counter). */
  requestId: string;
  /** Identifier of the rendering profile the session uses. */
  profileId: number;
}

/** A point of a pitch curve target. */
export interface VocalPitchPoint {
  /** Position in source samples. */
  sourceSample: number;
  /** Target pitch as a MIDI note number (fractional values allowed). */
  midi: number;
}

/** What a note's pitch is pulled toward. */
export type VocalPitchTarget =
  | {
      /** No pitch target; the note keeps its own pitch. */
      mode: 'none';
    }
  | {
      /** Steer the note toward one centre pitch. */
      mode: 'center';
      /** Centre pitch as a MIDI note number (fractional). */
      midi: number;
    }
  | {
      /** Steer the note along a pitch curve over the source. */
      mode: 'curve';
      /** Curve points, each a source position in samples and a target MIDI note. */
      points: readonly VocalPitchPoint[];
    };

/** Pitch correction settings of one note. */
export interface VocalPitchEdit {
  /** Pitch the note is moved toward. */
  target: VocalPitchTarget;
  /** Correction strength, 0 (none) to 1 (full). */
  amount: number;
  /** Correction time constant in milliseconds. */
  speedMs: number;
  /** Largest correction applied to any frame, in semitones. */
  maxCorrectionSemitones: number;
  /** Constant shift added after correction, in semitones. */
  transposeSemitones: number;
  /** Scale applied to slow pitch drift around the target. */
  driftScale: number;
  /** Scale applied to vibrato depth. */
  vibratoScale: number;
}

/** How the spectral envelope follows a pitch change. */
export type VocalFormantMode = 'preserve' | 'shift';

/** Formant handling of one note. */
export interface VocalFormantEdit {
  /** `'preserve'` keeps the original envelope; `'shift'` moves it by `shiftSemitones`. */
  mode: VocalFormantMode;
  /** Envelope shift in semitones, used when `mode` is `'shift'`. */
  shiftSemitones: number;
}

/** The complete set of edits applied to one note. */
export interface VocalNoteEdit {
  /** Pitch correction. */
  pitch: VocalPitchEdit;
  /** Where the note starts in the output, in samples. */
  destinationStartSample: number;
  /** How long the note is in the output, in samples. */
  destinationLengthSamples: number;
  /** Gain in decibels. */
  gainDb: number;
  /** Whether the note is silenced. */
  muted: boolean;
  /** Per-note amplitude envelope multipliers; empty for none. */
  amplitudeEnvelope: readonly number[];
  /** Formant handling. */
  formant: VocalFormantEdit;
}

/** One segmented note of the analysed vocal. */
export interface VocalNote {
  /** Stable note identifier; split and merge retire ids and mint new ones. */
  id: number;
  /** First source sample of the note. */
  sourceStartSample: number;
  /** One past the last source sample of the note. */
  sourceEndSample: number;
  /** First analysis frame of the note. */
  analysisFrameStart: number;
  /** One past the last analysis frame of the note. */
  analysisFrameEnd: number;
  /** Whether the note has a measured pitch. */
  hasPitch: boolean;
  /** Median fundamental frequency in hertz. */
  medianHz: number;
  /** Median pitch as a MIDI note number. */
  centerMidi: number;
  /** Pitch stability of the note, 0 (unstable) to 1 (steady). */
  f0Stability: number;
  /** Per-frame amplitude of the note. */
  amplitude: Float32Array;
  /** The edits currently applied to the note. */
  edit: VocalNoteEdit;
}

/** A crossfade between two adjacent notes. */
export interface VocalTransition {
  /** Note on the left side of the join. */
  leftNoteId: number;
  /** Note on the right side of the join. */
  rightNoteId: number;
  /** Crossfade length on the left note, in samples. */
  leftWindowSamples: number;
  /** Crossfade length on the right note, in samples. */
  rightWindowSamples: number;
  /** Crossfade strength, 0 to 1. */
  strength: number;
  /** Crossfade shape. */
  curve: 'smoothstep';
}

/** Notes and transitions of a session or draft. */
export interface VocalNotesResult {
  /** Notes in source order. */
  notes: VocalNote[];
  /** Transitions between adjacent notes. */
  transitions: VocalTransition[];
}

/** The pitch analysis a session was built from. */
export interface VocalAnalysis {
  /** Source sample position of frame 0 (may be fractional). */
  frameOriginSample: number;
  /** Source samples between consecutive frames (may be fractional). */
  samplesPerFrame: number;
  /** Length of one analysis frame in samples. */
  frameLengthSamples: number;
  /** Fundamental frequency per frame in hertz. */
  f0Hz: Float32Array;
  /** Per-frame voicing flag, 1 where voiced. */
  voiced: Uint8Array;
  /** Name of the algorithm that produced the analysis. */
  algorithmId: string;
  /** Version of that algorithm. */
  algorithmVersion: number;
  /** Source length in samples. */
  sourceLengthSamples: number;
  /** Source sample rate in hertz. */
  sampleRate: number;
  /** SHA-256 of the source PCM as 64 lowercase hex characters. */
  sourceSha256: string;
  /** SHA-256 of the analysis data as 64 lowercase hex characters. */
  analysisSha256: string;
  /** Lower bound of the pitch search in hertz. */
  fminHz: number;
  /** Upper bound of the pitch search in hertz. */
  fmaxHz: number;
  /** Cutoff on the normalized YIN function, in (0, 1]; 0 is refused. */
  yinThreshold: number;
  /** Probability in [0, 1] above which a frame counts as voiced. */
  voicedThreshold: number;
  /** Whether frames are centred on their origin sample. */
  centered: boolean;
  /** Pitch jump that starts a new note, in cents. */
  segmentationThresholdCents: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs: number;
  /** Reference tuning pitch (A4) in hertz. */
  referenceHz: number;
}

/** What the loaded vocal-edit engine supports. */
export interface VocalCapabilities {
  /** Vocal-edit ABI version of the loaded library, as {@link vocalEditAbiVersion} reports it. */
  abiVersion: number;
  /** Identifier of the rendering profile. */
  profileId: number;
  /** Always `true`: the engine edits one voice at a time. */
  monophonicOnly: true;
  /** Whether analysis can be cancelled part way. */
  analysisCancellable: boolean;
  /** Smallest formant shift accepted, in semitones. */
  minimumFormantShiftSemitones: number;
  /** Largest formant shift accepted, in semitones. */
  maximumFormantShiftSemitones: number;
}

/** Whether undo and redo currently have anything to act on. */
export interface VocalHistory {
  /** `true` when {@link VocalEditSession.undo} would change the state. */
  canUndo: boolean;
  /** `true` when {@link VocalEditSession.redo} would change the state. */
  canRedo: boolean;
}

/** Runtime limits of a session; an omitted field takes the library default. */
export interface VocalSessionLimits {
  /** Largest size of the undo history in bytes. */
  maxHistoryBytes?: number;
  /** Largest size of the render cache in bytes. */
  maxCacheBytes?: number;
  /** Most undo steps kept. */
  maxUndoDepth?: number;
  /** Most render jobs alive at once. */
  maxRenderJobs?: number;
}

/** A host-supplied pitch analysis that replaces the built-in one. */
export interface VocalAnalysisInput {
  /** Source sample position of frame 0. */
  frameOriginSample: number;
  /** Source samples between consecutive frames; must be positive. */
  samplesPerFrame: number;
  /** Length of one analysis frame in samples; must be positive. */
  frameLengthSamples: number;
  /** Fundamental frequency per frame in hertz. */
  f0Hz: Float32Array;
  /** Per-frame voicing flag, 1 where voiced; the same length as `f0Hz`. */
  voiced: Uint8Array;
  /** Name of the producing algorithm; must not contain NUL. */
  algorithmId?: string;
  /** Version of the producing algorithm. */
  algorithmVersion?: number;
  /** Lower bound of the pitch search in hertz. */
  fminHz?: number;
  /** Upper bound of the pitch search in hertz. */
  fmaxHz?: number;
  /** Cutoff on the normalized YIN function the analysis used, in (0, 1]; 0 is refused. */
  yinThreshold?: number;
  /** Voicing probability threshold in [0, 1] the analysis used. */
  voicedThreshold?: number;
  /** Whether frames are centred on their origin sample. */
  centered?: boolean;
  /** Pitch jump that starts a new note, in cents. */
  segmentationThresholdCents?: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs?: number;
  /** Reference tuning pitch (A4) in hertz. */
  referenceHz?: number;
}

/** Tuning of session creation. Every field is optional and defaults to the library value. */
export interface VocalCreateOptions {
  /** Length of the rendered output in samples; defaults to the source length. */
  outputLengthSamples?: number;
  /** Fade applied at the start and end of the output, in milliseconds. */
  edgeFadeMs?: number;
  /** Cutoff separating slow drift from vibrato, in hertz. */
  vibratoCutoffHz?: number;
  /** Pitch jump that starts a new note, in cents. */
  segmentationThresholdCents?: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs?: number;
  /** Analysis frame length in samples. */
  frameLengthSamples?: number;
  /** Analysis hop in samples. */
  hopLengthSamples?: number;
  /** Lower bound of the pitch search in hertz. */
  fminHz?: number;
  /** Upper bound of the pitch search in hertz. */
  fmaxHz?: number;
  /** Cutoff on the normalized YIN function, in (0, 1]; 0 is refused. */
  yinThreshold?: number;
  /** Voicing probability threshold, a probability in [0, 1]. */
  voicedThreshold?: number;
  /** Whether analysis frames are centred on their origin sample. */
  centered?: boolean;
  /** Reference tuning pitch (A4) in hertz. */
  referenceHz?: number;
  /** Runtime limits. */
  limits?: VocalSessionLimits;
  /** A host analysis to adopt instead of running pitch tracking. */
  analysis?: VocalAnalysisInput;
}

/** Input of {@link createVocalEditSession}. */
export interface VocalCreateRequest extends VocalCreateOptions {
  /** Mono source PCM. */
  samples: Float32Array;
  /** Sample rate of `samples` in hertz. */
  sampleRate: number;
}

/** Input of {@link restoreVocalEditSession}. */
export interface VocalRestoreRequest {
  /** The same mono source PCM the state was exported from. */
  samples: Float32Array;
  /** Sample rate of `samples` in hertz. */
  sampleRate: number;
  /** State blob from {@link VocalEditSession.exportState}. */
  state: Uint8Array;
  /** Runtime limits; omitted fields take the same defaults as session creation. */
  limits?: VocalSessionLimits;
}

/** Replaces the edit parameters of a note. */
export interface VocalSetEditOperation {
  /** Operation discriminator. */
  kind: 'setEdit';
  /** Note to edit. */
  noteId: number;
  /** The complete new edit. */
  edit: VocalNoteEdit;
}

/** A required position spelled in samples (`S`) or in seconds (`T`), exactly one of the two. */
export type VocalSampleOrSec<S extends string, T extends string> =
  | ({ [K in S]: number } & { [K in T]?: never })
  | ({ [K in T]: number } & { [K in S]?: never });

/**
 * Re-spans a note. Each of the four positions is required and given in samples or in
 * seconds (rounded to the nearest sample at the session's rate), one spelling each.
 */
export type VocalSetSourceSpanOperation = {
  /** Operation discriminator. */
  kind: 'setSourceSpan';
  /** Note to re-span. */
  noteId: number;
} & VocalSampleOrSec<'sourceStartSample', 'sourceStartSec'> &
  VocalSampleOrSec<'sourceEndSample', 'sourceEndSec'> &
  VocalSampleOrSec<'destinationStartSample', 'destinationStartSec'> &
  VocalSampleOrSec<'destinationLengthSamples', 'destinationLengthSec'>;

/** Splits a note in two. The cut position is required: `sourceSample` or `sourceSec`, not both. */
export type VocalSplitOperation = {
  /** Operation discriminator. */
  kind: 'split';
  /** Note to split. */
  noteId: number;
} & VocalSampleOrSec<'sourceSample', 'sourceSec'>;

/** Merges adjacent notes into one. */
export interface VocalMergeOperation {
  /** Operation discriminator. */
  kind: 'merge';
  /** Notes to merge, in order. */
  noteIds: readonly number[];
  /** Whether the merged note keeps the existing edits (`'preserve'`) or starts fresh (`'reset'`). */
  policy?: 'preserve' | 'reset';
}

/** Sets or replaces the transition between two notes. */
export interface VocalSetTransitionOperation {
  /** Operation discriminator. */
  kind: 'setTransition';
  /** The transition to store. */
  transition: VocalTransition;
}

/** Removes the transition between two notes. */
export interface VocalRemoveTransitionOperation {
  /** Operation discriminator. */
  kind: 'removeTransition';
  /** Note on the left side of the join. */
  leftNoteId: number;
  /** Note on the right side of the join. */
  rightNoteId: number;
}

/** Restores notes to their analysed state. */
export interface VocalResetOperation {
  /** Operation discriminator. */
  kind: 'reset';
  /** Notes to reset. */
  noteIds: readonly number[];
}

/** One edit step applied to a draft. */
export type VocalEditOperation =
  | VocalSetEditOperation
  | VocalSetSourceSpanOperation
  | VocalSplitOperation
  | VocalMergeOperation
  | VocalSetTransitionOperation
  | VocalRemoveTransitionOperation
  | VocalResetOperation;

/** Input of `draft.apply`. */
export interface VocalApplyRequest {
  /**
   * The draft generation the operations were authored against, as a decimal string
   * (64-bit counter). Omitted means the draft's current generation.
   */
  expectedGeneration?: string;
  /** Operations applied in order, all or none. */
  operations: readonly VocalEditOperation[];
}

/** A note id replaced by an operation. */
export interface VocalIdChange {
  /** Index of the operation in the request that caused the change. */
  operationIndex: number;
  /** The id that no longer exists. */
  retiredId: number;
  /** The ids that took its place. */
  newIds: readonly number[];
}

/** Outcome of an apply, commit, undo or redo. */
export interface VocalEditResult {
  /** State the edit produced. */
  token: VocalStateToken;
  /** Destination ranges whose audio changed. */
  dirtyRanges: readonly VocalRange[];
  /** Note ids retired and created by the edit. */
  idChanges: readonly VocalIdChange[];
}

/** Per-frame pitch of one note: measured, target and what the render will use. */
export interface VocalPitchEvaluation {
  /** Source sample position of each frame. */
  sourceSamples: Float64Array;
  /** Measured pitch per frame as MIDI note numbers. */
  measuredMidi: Float64Array;
  /** Target pitch per frame as MIDI note numbers. */
  targetMidi: Float64Array;
  /** Pitch the render will produce per frame as MIDI note numbers. */
  effectiveMidi: Float64Array;
  /** Per-frame voicing flag, 1 where voiced. */
  voiced: Uint8Array;
  /** Per-frame flag, 1 where a target exists. */
  hasTarget: Uint8Array;
}

/**
 * A destination range to render. Each bound is samples or seconds (rounded to
 * the nearest sample at the session's rate), one spelling each; an omitted start
 * is 0 and an omitted end is the end of the output. Negative bounds are refused.
 */
export interface VocalRangeInput {
  /** Start in samples. */
  startSample?: number;
  /** Start in seconds. */
  startSec?: number;
  /** End in samples. */
  endSample?: number;
  /** End in seconds. */
  endSec?: number;
}

/** Range and tag of a render or preview; the request of `VocalRenderSnapshot.beginRenderJob`. */
export interface VocalRenderJobRequest {
  /** Omit to render the complete destination output. */
  range?: VocalRangeInput;
  /** Caller tag echoed in the result token, as a decimal string (64-bit counter); defaults to `'0'`. */
  requestId?: string;
}

/** Input of the synchronous `VocalRenderSnapshot.render`. */
export interface VocalRenderRequest extends VocalRenderJobRequest {
  /**
   * Polled between render work units; returning `true` stops the render with a
   * `Cancelled` error and no partial PCM.
   */
  cancel?: () => boolean;
}

/** Input of an asynchronous render or worker preview. */
export interface VocalRenderAsyncRequest extends VocalRenderJobRequest {
  /** Aborts the render; an aborted call rejects with a `Cancelled` error. */
  signal?: AbortSignal;
}

/** Rendered PCM. */
export interface VocalRenderResult {
  /** Rendered mono samples. */
  samples: Float32Array;
  /** Destination sample the first rendered sample belongs to. */
  startSample: number;
  /** State the render was produced from. */
  token: VocalStateToken;
  /** Destination ranges that were recomputed rather than served from cache. */
  processedRanges: readonly VocalRange[];
  /** Work units served from the cache. */
  cacheHitUnits: number;
  /** Frames passed through unprocessed. */
  dryPassedFrames: number;
  /** Frames whose pitch correction was clamped by `maxCorrectionSemitones`. */
  limitedCorrectionFrames: number;
}

/** Progress of a render job. */
export interface VocalJobProgress {
  /** `true` once every work unit has run and the job can be finalised. */
  complete: boolean;
}

/** Why the vocal-edit engine refused a call. */
export type VocalReason =
  | 'none'
  | 'invalidInput'
  | 'revisionConflict'
  | 'sourceMismatch'
  | 'unsupported'
  | 'cancelled'
  | 'counterExhausted'
  | 'invalidState';

/** Structured cause carried as `detail` on a coded `SonareError` raised by a vocal-edit call. */
export interface VocalErrorDetail {
  /** The refusal category. */
  readonly reason: VocalReason;
  /** The request field the refusal concerns; empty when none. */
  readonly field: string;
  /** The value the engine required, as text. */
  readonly expected: string;
  /** The value the caller supplied, as text. */
  readonly actual: string;
  /** Descriptive form of `expected` when the engine has one; empty otherwise. */
  readonly expectedText: string;
  /** Descriptive form of `actual` when the engine has one; empty otherwise. */
  readonly actualText: string;
}
