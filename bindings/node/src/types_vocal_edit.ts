/** Why a vocal-edit call was refused. `'none'` is the value of a record that carries no failure. */
export type VocalReason =
  | 'none'
  | 'invalidInput'
  | 'revisionConflict'
  | 'sourceMismatch'
  | 'unsupported'
  | 'cancelled'
  | 'counterExhausted'
  | 'invalidState';

/** A half-open destination or source range in samples. */
export interface VocalRange {
  /** First sample of the range. */
  startSample: number;
  /** One past the last sample of the range. */
  endSample: number;
}

/**
 * Identifies one state of a session or draft. The counters are decimal strings
 * because 64-bit values exceed 2^53; they are compared for equality and never
 * computed on.
 */
export interface VocalStateToken {
  /** Decimal counter naming the session instance. */
  readonly sessionEpoch: string;
  /** Decimal counter of committed edits. */
  readonly revision: string;
  /** Decimal id of the draft, `'0'` for a committed state. */
  readonly draftId: string;
  /** Decimal counter of operations applied to the draft, `'0'` for a committed state. */
  readonly generation: string;
  /** Decimal id of the render request that produced the state, `'0'` when none. */
  readonly requestId: string;
  /** Engine profile the state was produced under. */
  readonly profileId: number;
}

/** One point of a pitch curve target. */
export interface VocalPitchPoint {
  /** Position in the source, in samples. */
  sourceSample: number;
  /** Target pitch as a MIDI note number (fractional). */
  midi: number;
}

/** Where a note's pitch is steered: nowhere, one centre pitch, or a curve over the source. */
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

/** Pitch correction and shaping applied to one note. */
export interface VocalPitchEdit {
  /** Pitch target. */
  target: VocalPitchTarget;
  /** Correction strength, 0 (none) to 1 (full). */
  amount: number;
  /** Correction speed in milliseconds. */
  speedMs: number;
  /** Largest correction applied, in semitones. */
  maxCorrectionSemitones: number;
  /** Transposition applied on top of the correction, in semitones. */
  transposeSemitones: number;
  /** Scale of the natural pitch drift kept (1 keeps it, 0 removes it). */
  driftScale: number;
  /** Scale of the vibrato kept (1 keeps it, 0 removes it). */
  vibratoScale: number;
}

/** Whether a note's formants follow its pitch or are shifted independently. */
export type VocalFormantMode = 'preserve' | 'shift';

/** Formant handling for one note. */
export interface VocalFormantEdit {
  /** Formant mode. */
  mode: VocalFormantMode;
  /** Formant shift in semitones, used in `'shift'` mode. */
  shiftSemitones: number;
}

/** Every editable property of one note. */
export interface VocalNoteEdit {
  /** Pitch edit. */
  pitch: VocalPitchEdit;
  /** Where the note starts in the output, in samples. */
  destinationStartSample: number;
  /** Length of the note in the output, in samples. */
  destinationLengthSamples: number;
  /** Gain in decibels. */
  gainDb: number;
  /** Silences the note when true. */
  muted: boolean;
  /** Amplitude envelope multipliers over the note; empty leaves the amplitude untouched. */
  amplitudeEnvelope: readonly number[];
  /** Formant edit. */
  formant: VocalFormantEdit;
}

/** One analysed note and its current edit. */
export interface VocalNote {
  /** Note id, stable until an operation retires it. */
  id: number;
  /** First source sample of the note. */
  sourceStartSample: number;
  /** One past the last source sample of the note. */
  sourceEndSample: number;
  /** First analysis frame of the note. */
  analysisFrameStart: number;
  /** One past the last analysis frame of the note. */
  analysisFrameEnd: number;
  /** True when the note has a measurable pitch. */
  hasPitch: boolean;
  /** Median fundamental frequency in hertz. */
  medianHz: number;
  /** Centre pitch as a MIDI note number. */
  centerMidi: number;
  /** Stability of the fundamental over the note, 0 to 1. */
  f0Stability: number;
  /** Amplitude envelope of the note, one value per analysis frame. */
  amplitude: Float32Array;
  /** Current edit. */
  edit: VocalNoteEdit;
}

/** A crossfade joining two adjacent notes. */
export interface VocalTransition {
  /** Id of the earlier note. */
  leftNoteId: number;
  /** Id of the later note. */
  rightNoteId: number;
  /** Window length taken from the earlier note, in samples. */
  leftWindowSamples: number;
  /** Window length taken from the later note, in samples. */
  rightWindowSamples: number;
  /** Blend strength, 0 to 1. */
  strength: number;
  /** Crossfade curve. */
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
  /** Source sample of the first analysis frame. */
  frameOriginSample: number;
  /** Hop between analysis frames, in samples. */
  samplesPerFrame: number;
  /** Analysis window length, in samples. */
  frameLengthSamples: number;
  /** Fundamental frequency per frame, in hertz. */
  f0Hz: Float32Array;
  /** Voicing flag per frame (0 or 1). */
  voiced: Uint8Array;
  /** Identifier of the pitch algorithm. */
  algorithmId: string;
  /** Version of the pitch algorithm. */
  algorithmVersion: number;
  /** Lowest frequency searched, in hertz. */
  fminHz: number;
  /** Highest frequency searched, in hertz. */
  fmaxHz: number;
  /** Cutoff on the normalized YIN function, in (0, 1]; 0 is refused. */
  yinThreshold: number;
  /** Voicing decision threshold, a probability in [0, 1]. */
  voicedThreshold: number;
  /** True when frames are centred on their position. */
  centered: boolean;
  /** Pitch change in cents that starts a new note. */
  segmentationThresholdCents: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs: number;
  /** Reference tuning (A4) in hertz. */
  referenceHz: number;
  /** Length of the analysed source, in samples. */
  sourceLengthSamples: number;
  /** Sample rate of the analysed source. */
  sampleRate: number;
  /** SHA-256 of the source, hexadecimal. */
  sourceSha256: string;
  /** SHA-256 of the analysis, hexadecimal. */
  analysisSha256: string;
}

/** What the loaded vocal-edit engine supports. */
export interface VocalCapabilities {
  /** Vocal-edit ABI version of the loaded library, as {@link vocalEditAbiVersion} reports it. */
  abiVersion: number;
  /** Engine profile id. */
  profileId: number;
  /** The engine edits one voice at a time. */
  monophonicOnly: true;
  /** True when analysis can be cancelled. */
  analysisCancellable: boolean;
  /** Lowest formant shift accepted, in semitones. */
  minimumFormantShiftSemitones: number;
  /** Highest formant shift accepted, in semitones. */
  maximumFormantShiftSemitones: number;
}

/** Whether the session has an edit to undo or redo. */
export interface VocalHistory {
  /** True when {@link VocalEditSession.undo} has an edit to revert. */
  canUndo: boolean;
  /** True when {@link VocalEditSession.redo} has an edit to reapply. */
  canRedo: boolean;
}

/** Memory and concurrency ceilings of a session. Omitted fields take the library defaults. */
export interface VocalSessionLimits {
  /** Largest edit history kept, in bytes. */
  maxHistoryBytes?: number;
  /** Largest render cache kept, in bytes. */
  maxCacheBytes?: number;
  /** Deepest undo stack kept. */
  maxUndoDepth?: number;
  /** Most render jobs alive at once. */
  maxRenderJobs?: number;
}

/** Pitch analysis supplied by the caller instead of being computed from the samples. */
export interface VocalAnalysisInput {
  /** Source sample of the first analysis frame. */
  frameOriginSample: number;
  /** Hop between analysis frames, in samples. */
  samplesPerFrame: number;
  /** Analysis window length, in samples. */
  frameLengthSamples: number;
  /** Fundamental frequency per frame, in hertz. */
  f0Hz: Float32Array;
  /** Voicing flag per frame (0 or 1). */
  voiced: Uint8Array;
  /** Identifier of the algorithm that produced the analysis. */
  algorithmId?: string;
  /** Version of that algorithm. */
  algorithmVersion?: number;
  /** Lowest frequency searched, in hertz. */
  fminHz?: number;
  /** Highest frequency searched, in hertz. */
  fmaxHz?: number;
  /** Cutoff on the normalized YIN function, in (0, 1]; 0 is refused. */
  yinThreshold?: number;
  /** Voicing decision threshold, a probability in [0, 1]. */
  voicedThreshold?: number;
  /** True when frames are centred on their position. */
  centered?: boolean;
  /** Pitch change in cents that starts a new note. */
  segmentationThresholdCents?: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs?: number;
  /** Reference tuning (A4) in hertz. */
  referenceHz?: number;
}

/** Session creation settings other than the audio. Omitted fields take the library defaults. */
export interface VocalCreateOptions {
  /** Length of the output in samples; defaults to the source length. */
  outputLengthSamples?: number;
  /** Fade applied at note edges, in milliseconds. */
  edgeFadeMs?: number;
  /** Cutoff separating vibrato from drift, in hertz. */
  vibratoCutoffHz?: number;
  /** Pitch change in cents that starts a new note. */
  segmentationThresholdCents?: number;
  /** Shortest note kept, in milliseconds. */
  minNoteMs?: number;
  /** Analysis window length, in samples. */
  frameLengthSamples?: number;
  /** Analysis hop, in samples. */
  hopLengthSamples?: number;
  /** Lowest frequency searched, in hertz. */
  fminHz?: number;
  /** Highest frequency searched, in hertz. */
  fmaxHz?: number;
  /** Cutoff on the normalized YIN function, in (0, 1]; 0 is refused. */
  yinThreshold?: number;
  /** Voicing decision threshold, a probability in [0, 1]. */
  voicedThreshold?: number;
  /** True when frames are centred on their position. */
  centered?: boolean;
  /** Reference tuning (A4) in hertz. */
  referenceHz?: number;
  /** Runtime limits. */
  limits?: VocalSessionLimits;
  /** Analysis to use instead of analysing the samples. */
  analysis?: VocalAnalysisInput;
}

/** Request for {@link createVocalEditSession}. */
export interface VocalCreateRequest extends VocalCreateOptions {
  /** Mono source audio. */
  samples: Float32Array;
  /** Sample rate of `samples` in hertz. */
  sampleRate: number;
  /** Set false to skip the O(n) finiteness scan of `samples`; default true. */
  validate?: boolean;
}

/** Request for {@link restoreVocalEditSession}. */
export interface VocalRestoreRequest {
  /** Mono source audio the state was exported from. */
  samples: Float32Array;
  /** Sample rate of `samples` in hertz. */
  sampleRate: number;
  /** State bytes from {@link VocalEditSession.exportState}. */
  state: Uint8Array;
  /** Runtime limits; omitted fields take the same defaults as session creation. */
  limits?: VocalSessionLimits;
  /** Set false to skip the O(n) finiteness scan of `samples`; default true. */
  validate?: boolean;
}

/** Replaces a note's whole edit. */
export interface VocalSetEditOperation {
  /** Operation discriminator: `'setEdit'`. */
  kind: 'setEdit';
  /** Note to edit. */
  noteId: number;
  /** The new edit. */
  edit: VocalNoteEdit;
}

/**
 * Each of the four positions is given in samples or in seconds (rounded to the
 * nearest sample at the session's rate), one spelling each.
 */
export interface VocalSetSourceSpanOperation {
  /** Operation discriminator: `'setSourceSpan'`. */
  kind: 'setSourceSpan';
  /** Note to change. */
  noteId: number;
  /** Start of the note in the source. */
  sourceStartSample?: number;
  /** Start of the note in the source, in seconds. */
  sourceStartSec?: number;
  /** End of the note in the source. */
  sourceEndSample?: number;
  /** End of the note in the source, in seconds. */
  sourceEndSec?: number;
  /** Start of the note in the output. */
  destinationStartSample?: number;
  /** Start of the note in the output, in seconds. */
  destinationStartSec?: number;
  /** Length of the note in the output. */
  destinationLengthSamples?: number;
  /** Length of the note in the output, in seconds. */
  destinationLengthSec?: number;
}

/** The cut position is `sourceSample` or `sourceSec`, not both. */
export interface VocalSplitOperation {
  /** Operation discriminator: `'split'`. */
  kind: 'split';
  /** Note to split. */
  noteId: number;
  /** Cut position in the source, in samples. */
  sourceSample?: number;
  /** Cut position in the source, in seconds. */
  sourceSec?: number;
}

/** Merges adjacent notes into one. */
export interface VocalMergeOperation {
  /** Operation discriminator: `'merge'`. */
  kind: 'merge';
  /** Notes to merge, in order. */
  noteIds: readonly number[];
  /** Whether the merged note keeps the first note's edit (`'preserve'`) or starts from defaults (`'reset'`). */
  policy?: 'preserve' | 'reset';
}

/** Sets the crossfade between two adjacent notes. */
export interface VocalSetTransitionOperation {
  /** Operation discriminator: `'setTransition'`. */
  kind: 'setTransition';
  /** The transition to set. */
  transition: VocalTransition;
}

/** Removes the crossfade between two adjacent notes. */
export interface VocalRemoveTransitionOperation {
  /** Operation discriminator: `'removeTransition'`. */
  kind: 'removeTransition';
  /** Id of the earlier note. */
  leftNoteId: number;
  /** Id of the later note. */
  rightNoteId: number;
}

/** Restores notes to their analysed state. */
export interface VocalResetOperation {
  /** Operation discriminator: `'reset'`. */
  kind: 'reset';
  /** Notes to reset. */
  noteIds: readonly number[];
}

/** One edit applied to a draft; `kind` selects the shape. */
export type VocalEditOperation =
  | VocalSetEditOperation
  | VocalSetSourceSpanOperation
  | VocalSplitOperation
  | VocalMergeOperation
  | VocalSetTransitionOperation
  | VocalRemoveTransitionOperation
  | VocalResetOperation;

/** Request for {@link VocalEditDraft.apply}. */
export interface VocalApplyRequest {
  /**
   * Draft generation the operations were written against, a decimal string
   * (64-bit counters exceed 2^53). Omit to apply against the draft's current one.
   */
  expectedGeneration?: string;
  /** Operations applied in order. */
  operations: readonly VocalEditOperation[];
}

/** A note id replaced by an operation. */
export interface VocalIdChange {
  /** Index of the operation in the request. */
  operationIndex: number;
  /** Id that no longer exists. */
  retiredId: number;
  /** Ids that replace it. */
  newIds: readonly number[];
}

/** Outcome of an apply, commit, undo or redo. */
export interface VocalEditResult {
  /** State after the call. */
  token: VocalStateToken;
  /** Output ranges whose audio changed. */
  dirtyRanges: readonly VocalRange[];
  /** Note ids replaced by the call. */
  idChanges: readonly VocalIdChange[];
}

/** Per-sample pitch curves of one note, all of equal length. */
export interface VocalPitchEvaluation {
  /** Source positions of the samples. */
  sourceSamples: Float64Array;
  /** Measured pitch as MIDI note numbers. */
  measuredMidi: Float64Array;
  /** Target pitch as MIDI note numbers. */
  targetMidi: Float64Array;
  /** Pitch after correction as MIDI note numbers. */
  effectiveMidi: Float64Array;
  /** Voicing flag per sample (0 or 1). */
  voiced: Uint8Array;
  /** 1 where a target exists. */
  hasTarget: Uint8Array;
}

/**
 * A destination range to render. Each bound is samples or seconds (rounded to
 * the nearest sample at the session's rate), one spelling each; an omitted start
 * is 0 and an omitted end is the end of the output. Negative bounds are refused.
 */
export interface VocalRangeInput {
  /** Start of the range in output samples. Default 0. */
  startSample?: number;
  /** Start of the range in seconds. Default 0. */
  startSec?: number;
  /** End of the range in output samples. Default the end of the output. */
  endSample?: number;
  /** End of the range in seconds. Default the end of the output. */
  endSec?: number;
}

/** Request for the render calls of {@link VocalRenderSnapshot}. */
export interface VocalRenderRequest {
  /** Omit to render the complete destination output. */
  range?: VocalRangeInput;
  /** Decimal id echoed in the result's token (64-bit ids exceed 2^53); omitted means none. */
  requestId?: string;
  /** Aborts the render. */
  signal?: AbortSignal;
}

/** Audio rendered from a snapshot. */
export interface VocalRenderResult {
  /** Rendered mono samples. */
  samples: Float32Array;
  /** Output sample at which `samples` starts. */
  startSample: number;
  /** State the audio was rendered from. */
  token: VocalStateToken;
  /** Ranges that were actually recomputed. */
  processedRanges: readonly VocalRange[];
  /** Work units served from the cache. */
  cacheHitUnits: number;
  /** Frames passed through unprocessed. */
  dryPassedFrames: number;
  /** Frames whose correction was limited by `maxCorrectionSemitones`. */
  limitedCorrectionFrames: number;
}

/** Exported session state. */
export interface VocalStateBytes {
  /** The state bytes. */
  data: Uint8Array;
}

/** Progress of a {@link VocalRenderJob} step. */
export interface VocalJobProgress {
  /** True when no further work remains. */
  complete: boolean;
}

/**
 * Detail carried as `detail` on the coded `SonareError` a vocal-edit call
 * raises. Counter-valued `expected` / `actual` are decimal strings; the `*Text`
 * fields hold the text spelling when the value is not a number.
 */
export interface VocalErrorDetail {
  /** Why the call was refused. */
  readonly reason: VocalReason;
  /** Name of the offending field, empty when none. */
  readonly field: string;
  /** Expected value, as a decimal string or text. */
  readonly expected: string;
  /** Actual value, as a decimal string or text. */
  readonly actual: string;
  /** Text spelling of the expected value, empty when numeric. */
  readonly expectedText: string;
  /** Text spelling of the actual value, empty when numeric. */
  readonly actualText: string;
}
