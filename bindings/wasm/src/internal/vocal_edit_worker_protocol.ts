import type {
  VocalAnalysis,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalErrorDetail,
  VocalHistory,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderJobRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateToken,
} from '../public_types_vocal_edit.js';
import type { SonareFailurePayload } from './failure_payload.js';

/** An apply request with the generation resolved by the client. */
export interface VocalWorkerApplyRequest {
  expectedGeneration: string;
  operations: readonly VocalEditOperation[];
}

export type VocalWorkerMutation =
  | { kind: 'beginEdit'; expectedRevision?: string }
  | { kind: 'apply'; request: VocalWorkerApplyRequest }
  | { kind: 'commit'; expectedRevision: string }
  | { kind: 'cancel' }
  | { kind: 'undo'; expectedRevision?: string }
  | { kind: 'redo'; expectedRevision?: string }
  | { kind: 'notes'; draft?: boolean }
  | { kind: 'analysis' }
  | { kind: 'capabilities' }
  | { kind: 'exportState' }
  | { kind: 'outputLength' }
  | { kind: 'history' }
  | { kind: 'draftToken' }
  | { kind: 'evaluatePitch'; noteId: number; draft?: boolean }
  | {
      kind: 'mapCoordinate';
      noteId: number;
      sample: number;
      /** `false` maps source to destination; `true` maps destination to source. */
      inverse: boolean;
      draft?: boolean;
    };

export type VocalWorkerCreateRequest =
  | { kind: 'create'; request: VocalCreateRequest }
  | { kind: 'restore'; request: VocalRestoreRequest };

export interface VocalWorkerCreateMessage {
  type: 'sonare:vocal-create';
  id: number;
  sessionId: string;
  clientIntentSequence: string;
  request: VocalWorkerCreateRequest;
  cancelBuffer?: SharedArrayBuffer;
}

export interface VocalWorkerMutateMessage {
  type: 'sonare:vocal-mutate';
  id: number;
  sessionId: string;
  clientIntentSequence: string;
  baseToken?: VocalStateToken;
  mutation: VocalWorkerMutation;
  cancelBuffer?: SharedArrayBuffer;
}

export interface VocalWorkerPreviewMessage {
  type: 'sonare:vocal-preview';
  id: number;
  sessionId: string;
  clientIntentSequence: string;
  baseToken?: VocalStateToken;
  request: VocalRenderJobRequest;
  cancelBuffer?: SharedArrayBuffer;
}

export interface VocalWorkerDisposeMessage {
  type: 'sonare:vocal-dispose';
  id: number;
  sessionId: string;
  clientIntentSequence: string;
}

export interface VocalWorkerCancelMessage {
  type: 'sonare:vocal-cancel';
  id: number;
  sessionId: string;
  clientIntentSequence: string;
}

export type VocalWorkerRequestMessage =
  | VocalWorkerCreateMessage
  | VocalWorkerMutateMessage
  | VocalWorkerPreviewMessage
  | VocalWorkerDisposeMessage
  | VocalWorkerCancelMessage;

export interface VocalWorkerCreateResult {
  sessionId: string;
  token: VocalStateToken;
  notes: VocalNotesResult;
  analysis: VocalAnalysis;
  capabilities: VocalCapabilities;
}

export interface VocalWorkerResponseBase {
  id: number;
  sessionId: string;
  clientIntentSequence: string;
  /** Committed session token after the call. */
  token?: VocalStateToken;
  /** Open draft token after the call; absent when no draft is open. */
  draftToken?: VocalStateToken;
}

export type VocalWorkerResult =
  | VocalWorkerCreateResult
  | VocalEditResult
  | VocalNotesResult
  | VocalAnalysis
  | VocalCapabilities
  | Uint8Array
  | VocalRenderResult
  | VocalStateToken
  | VocalHistory
  | VocalPitchEvaluation
  | number
  | null;

export interface VocalWorkerResultMessage extends VocalWorkerResponseBase {
  type: 'sonare:vocal-result';
  result: VocalWorkerResult;
}

export interface VocalWorkerProgressMessage extends VocalWorkerResponseBase {
  type: 'sonare:vocal-progress';
  complete: boolean;
}

export interface VocalWorkerErrorMessage extends VocalWorkerResponseBase {
  type: 'sonare:vocal-error';
  error: SonareFailurePayload & { detail?: VocalErrorDetail };
}

export type VocalWorkerResponseMessage =
  | VocalWorkerResultMessage
  | VocalWorkerProgressMessage
  | VocalWorkerErrorMessage;

export function tokenEquals(
  a: VocalStateToken | undefined,
  b: VocalStateToken | undefined,
): boolean {
  if (!a || !b) {
    return a === b;
  }
  return (
    a.sessionEpoch === b.sessionEpoch &&
    a.revision === b.revision &&
    a.draftId === b.draftId &&
    a.generation === b.generation &&
    a.requestId === b.requestId &&
    a.profileId === b.profileId
  );
}

/** Compare the mutable session state portion of a token.
 *
 * Render results add request/profile metadata to the state token. That
 * metadata identifies the render artifact, so it must not make a snapshot's
 * state appear different from the session state used to create it.
 */
export function stateTokenEquals(
  a: VocalStateToken | undefined,
  b: VocalStateToken | undefined,
): boolean {
  if (!a || !b) {
    return a === b;
  }
  return (
    a.sessionEpoch === b.sessionEpoch &&
    a.revision === b.revision &&
    a.draftId === b.draftId &&
    a.generation === b.generation &&
    a.profileId === b.profileId
  );
}

/** Decimal sequence comparison without converting a uint64 to Number. */
export function compareUint64(a: string, b: string): number {
  const left = a.replace(/^0+(?=\d)/, '');
  const right = b.replace(/^0+(?=\d)/, '');
  if (left.length !== right.length) {
    return left.length < right.length ? -1 : 1;
  }
  if (left === right) {
    return 0;
  }
  return left < right ? -1 : 1;
}
