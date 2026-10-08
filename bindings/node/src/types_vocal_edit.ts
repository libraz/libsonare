import type { SonareError } from './errors.js';

/** Decimal spelling of a uint64 value. Number is intentionally excluded. */
export type VocalUint64 = string;

export interface VocalRange {
  startSample: number;
  endSample: number;
}

export interface VocalStateToken {
  sessionEpoch: VocalUint64;
  revision: VocalUint64;
  draftId: VocalUint64;
  generation: VocalUint64;
  requestId: VocalUint64;
  profileId: number;
}

export interface VocalPitchPoint {
  sourceSample: number;
  midi: number;
}

export type VocalPitchTarget =
  | { mode: 'none' }
  | { mode: 'center'; midi: number }
  | { mode: 'curve'; points: readonly VocalPitchPoint[] };

export interface VocalPitchEdit {
  target: VocalPitchTarget;
  amount: number;
  speedMs: number;
  maxCorrectionSemitones: number;
  transposeSemitones: number;
  driftScale: number;
  vibratoScale: number;
}

export type VocalFormantMode = 'preserve' | 'shift';

export interface VocalFormantEdit {
  mode: VocalFormantMode;
  shiftSemitones: number;
}

export interface VocalNoteEdit {
  pitch: VocalPitchEdit;
  destinationStartSample: number;
  destinationLengthSamples: number;
  gainDb: number;
  muted: boolean;
  amplitudeEnvelope: readonly number[];
  formant: VocalFormantEdit;
}

export interface VocalNote {
  id: number;
  sourceStartSample: number;
  sourceEndSample: number;
  analysisFrameStart: VocalUint64;
  analysisFrameEnd: VocalUint64;
  hasPitch: boolean;
  medianHz: number;
  centerMidi: number;
  f0Stability: number;
  amplitude: Float32Array;
  edit: VocalNoteEdit;
}

export interface VocalTransition {
  leftNoteId: number;
  rightNoteId: number;
  leftWindowSamples: number;
  rightWindowSamples: number;
  strength: number;
  curve: 'smoothstep';
}

export interface VocalAnalysis {
  frameOriginSample: number;
  samplesPerFrame: number;
  frameLengthSamples: number;
  f0Hz: Float32Array;
  voiced: Uint8Array;
  algorithmId: string;
  algorithmVersion: number;
  fminHz: number;
  fmaxHz: number;
  yinThreshold: number;
  voicedThreshold: number;
  centered: boolean;
  segmentationThresholdCents: number;
  minNoteMs: number;
  referenceHz: number;
  sourceLengthSamples: number;
  sampleRate: number;
  sourceSha256: string;
  analysisSha256: string;
}

export interface VocalCapabilities {
  apiVersion: number;
  profileId: number;
  monophonicOnly: true;
  analysisCancellable: boolean;
  minimumFormantShiftSemitones: number;
  maximumFormantShiftSemitones: number;
}

export interface VocalHistory {
  canUndo: boolean;
  canRedo: boolean;
}

export interface VocalSessionLimits {
  maxHistoryBytes?: VocalUint64;
  maxCacheBytes?: VocalUint64;
  maxUndoDepth?: number;
  maxRenderJobs?: number;
}

export interface VocalAnalysisInput {
  frameOriginSample: number;
  samplesPerFrame: number;
  frameLengthSamples: number;
  f0Hz: Float32Array;
  voiced: Uint8Array;
  algorithmId?: string;
  algorithmVersion?: number;
  fminHz?: number;
  fmaxHz?: number;
  yinThreshold?: number;
  voicedThreshold?: number;
  centered?: boolean;
  segmentationThresholdCents?: number;
  minNoteMs?: number;
  referenceHz?: number;
}

export interface VocalCreateOptions {
  outputLengthSamples?: number;
  edgeFadeMs?: number;
  vibratoCutoffHz?: number;
  segmentationThresholdCents?: number;
  minNoteMs?: number;
  frameLengthSamples?: number;
  hopLengthSamples?: number;
  fminHz?: number;
  fmaxHz?: number;
  yinThreshold?: number;
  voicedThreshold?: number;
  centered?: boolean;
  referenceHz?: number;
  limits?: VocalSessionLimits;
  analysis?: VocalAnalysisInput;
}

export interface VocalCreateRequest extends VocalCreateOptions {
  samples: Float32Array;
  sampleRate: number;
}

export interface VocalRestoreRequest {
  samples: Float32Array;
  sampleRate: number;
  state: Uint8Array;
  /** Runtime limits; omitted fields take the same defaults as session creation. */
  limits?: VocalSessionLimits;
}

export interface VocalSetEditOperation {
  kind: 'setEdit';
  noteId: number;
  edit: VocalNoteEdit;
}

/**
 * Each of the four positions is given in samples or in seconds (rounded to the
 * nearest sample at the session's rate), one spelling each.
 */
export interface VocalSetSourceSpanOperation {
  kind: 'setSourceSpan';
  noteId: number;
  sourceStartSample?: number;
  sourceStartSec?: number;
  sourceEndSample?: number;
  sourceEndSec?: number;
  destinationStartSample?: number;
  destinationStartSec?: number;
  destinationLengthSamples?: number;
  destinationLengthSec?: number;
}

/** The cut position is `sourceSample` or `sourceSec`, not both. */
export interface VocalSplitOperation {
  kind: 'split';
  noteId: number;
  sourceSample?: number;
  sourceSec?: number;
}

export interface VocalMergeOperation {
  kind: 'merge';
  noteIds: readonly number[];
  policy?: 'preserve' | 'reset';
}

export interface VocalSetTransitionOperation {
  kind: 'setTransition';
  transition: VocalTransition;
}

export interface VocalRemoveTransitionOperation {
  kind: 'removeTransition';
  leftNoteId: number;
  rightNoteId: number;
}

export interface VocalResetOperation {
  kind: 'reset';
  noteIds: readonly number[];
}

export type VocalEditOperation =
  | VocalSetEditOperation
  | VocalSetSourceSpanOperation
  | VocalSplitOperation
  | VocalMergeOperation
  | VocalSetTransitionOperation
  | VocalRemoveTransitionOperation
  | VocalResetOperation;

export interface VocalApplyRequest {
  expectedGeneration: VocalUint64;
  operations: readonly VocalEditOperation[];
}

export interface VocalIdChange {
  operationIndex: number;
  retiredId: number;
  newIds: readonly number[];
}

export interface VocalEditResult {
  token: VocalStateToken;
  dirtyRanges: readonly VocalRange[];
  idChanges: readonly VocalIdChange[];
}

export interface VocalPitchEvaluation {
  sourceSamples: Float64Array;
  measuredMidi: Float64Array;
  targetMidi: Float64Array;
  effectiveMidi: Float64Array;
  voiced: Uint8Array;
  hasTarget: Uint8Array;
}

/**
 * A destination range to render. Each bound is samples or seconds (rounded to
 * the nearest sample at the session's rate), one spelling each; an omitted start
 * is 0 and an omitted end is the end of the output. Negative bounds are refused.
 */
export interface VocalRangeInput {
  startSample?: number;
  startSec?: number;
  endSample?: number;
  endSec?: number;
}

export interface VocalRenderRequest {
  /** Omit to render the complete destination output. */
  range?: VocalRangeInput;
  requestId?: VocalUint64;
  signal?: AbortSignal;
}

export interface VocalRenderResult {
  samples: Float32Array;
  startSample: number;
  token: VocalStateToken;
  processedRanges: readonly VocalRange[];
  cacheHitUnits: VocalUint64;
  dryPassedFrames: VocalUint64;
  limitedCorrectionFrames: VocalUint64;
}

export interface VocalStateBytes {
  data: Uint8Array;
}

export interface VocalJobProgress {
  complete: boolean;
}

export interface VocalError extends SonareError {
  readonly reason: number;
  readonly field: string;
  readonly expected: VocalUint64;
  readonly actual: VocalUint64;
  readonly expectedText: string;
  readonly actualText: string;
}
