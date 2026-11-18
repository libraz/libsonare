import { ErrorCode, SonareError } from './errors.js';
import { failurePayload } from './internal/failure_payload.js';
import { releasedHandleError } from './internal/handle_guard.js';
import {
  compareUint64,
  type VocalWorkerCreateMessage,
  type VocalWorkerCreateResult,
  type VocalWorkerErrorMessage,
  type VocalWorkerMutateMessage,
  type VocalWorkerPreviewMessage,
  type VocalWorkerRequestMessage,
  type VocalWorkerResponseMessage,
  type VocalWorkerResult,
} from './internal/vocal_edit_worker_protocol.js';
import { isVocalErrorDetail } from './internal/vocal_error_detail.js';
import './lifetime.js';
import { init } from './index.js';
import type {
  VocalAnalysis,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalHistory,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateToken,
} from './public_types_vocal_edit.js';
import {
  createVocalEditSession,
  restoreVocalEditSession,
  type VocalRenderJob,
  type VocalRenderSnapshot,
} from './vocal_edit.js';

/** The message channel a vocal-edit Worker listens and replies on. */
export interface VocalEditWorkerEndpoint {
  /** Sends a response to the main thread, optionally transferring buffers. */
  postMessage(message: VocalWorkerResponseMessage, transfer?: Transferable[]): void;
  /** Registers the listener that receives request messages. */
  addEventListener(
    type: 'message',
    listener: (event: MessageEvent<VocalWorkerRequestMessage>) => void,
  ): void;
}

/** The draft operations the Worker drives; {@link VocalEditDraft} satisfies it. */
export interface VocalEditWorkerNativeDraft {
  /** Current state token. */
  readonly token: VocalStateToken;
  /** Notes of the draft. */
  notes(): VocalNotesResult;
  /** Applies operations; `expectedGeneration` must match the draft's current generation. */
  apply(request: {
    expectedGeneration: string;
    operations: readonly VocalEditOperation[];
  }): VocalEditResult;
  /** Commits the draft; `expectedRevision` must match the session's current revision. */
  commit(expectedRevision: string): VocalEditResult;
  /** Discards the draft. */
  cancel(): void;
  /** Pitch curves of a note after the draft's edits. */
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  /** Maps a sample position in a note's source to the output. */
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  /** Maps an output sample position in a note to the source. */
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  /** Captures a snapshot to render from. */
  captureRenderSnapshot(): VocalRenderSnapshot;
  /** Releases the draft. */
  destroy(): void;
}

/** The session operations the Worker drives; {@link VocalEditSession} satisfies it. */
export interface VocalEditWorkerNativeSession {
  /** Current state token. */
  readonly token: VocalStateToken;
  /** Notes of the session. */
  notes(): VocalNotesResult;
  /** Analysis of the source. */
  analysis(): VocalAnalysis;
  /** Capabilities of the session. */
  capabilities(): VocalCapabilities;
  /** Opens a draft; `expectedRevision`, when given, must match the current revision. */
  beginEdit(expectedRevision?: string): VocalEditWorkerNativeDraft;
  /** Undoes the last edit; `expectedRevision`, when given, must match the current revision. */
  undo(expectedRevision?: string): VocalEditResult;
  /** Redoes the last undone edit; `expectedRevision`, when given, must match the current revision. */
  redo(expectedRevision?: string): VocalEditResult;
  /** Length of the output in samples. */
  readonly outputLengthSamples: number;
  /** Undo/redo history. */
  history(): VocalHistory;
  /** Pitch curves of a note after the session's edits. */
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  /** Maps a sample position in a note's source to the output. */
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  /** Maps an output sample position in a note to the source. */
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  /** Captures a snapshot to render from. */
  captureRenderSnapshot(): VocalRenderSnapshot;
  /** Serializes the session state for {@link VocalEditWorkerFactory.restore}. */
  exportState(): Uint8Array;
  /** Releases the session. */
  destroy(): void;
}

/** Creates the native sessions a Worker serves; replaceable for tests. */
export interface VocalEditWorkerFactory {
  /** Creates a session from source audio. */
  create(request: VocalCreateRequest): VocalEditWorkerNativeSession;
  /** Restores a session from exported state. */
  restore(request: VocalRestoreRequest): VocalEditWorkerNativeSession;
}

const productionFactory: VocalEditWorkerFactory = {
  create: createVocalEditSession,
  restore: restoreVocalEditSession,
};

interface ActiveCommand {
  id: number;
  cancelled: boolean;
  preview: boolean;
  cancelBuffer?: SharedArrayBuffer;
}

interface QueuedCommand {
  message: VocalWorkerMutateMessage | VocalWorkerPreviewMessage;
  active: ActiveCommand;
}

interface WorkerSession {
  value: VocalEditWorkerNativeSession;
  draft?: VocalEditWorkerNativeDraft;
  latestIntent: string;
  queue: QueuedCommand[];
  running: ActiveCommand | undefined;
  draining: boolean;
  /** Set synchronously when a dispose arrives; the session accepts no more work. */
  disposing: boolean;
  /** Settles once the native session is released. */
  released?: Promise<void>;
}

function errorPayload(error: unknown): VocalWorkerErrorMessage['error'] {
  const detail = (error as { detail?: unknown } | null)?.detail;
  return {
    ...failurePayload(error),
    ...(isVocalErrorDetail(detail) ? { detail } : {}),
  };
}

function transferables(value: unknown): Transferable[] {
  const result: Transferable[] = [];
  const seen = new Set<ArrayBuffer>();
  const visit = (item: unknown): void => {
    if (item === null || item === undefined || typeof item !== 'object') {
      return;
    }
    if (ArrayBuffer.isView(item)) {
      const buffer = item.buffer;
      if (buffer instanceof ArrayBuffer && !seen.has(buffer)) {
        seen.add(buffer);
        result.push(buffer);
      }
      return;
    }
    if (item instanceof ArrayBuffer) {
      if (!seen.has(item)) {
        seen.add(item);
        result.push(item);
      }
      return;
    }
    if (Array.isArray(item)) {
      item.forEach(visit);
      return;
    }
    Object.values(item).forEach(visit);
  };
  visit(value);
  return result;
}

function cancelledError(): SonareError {
  return new SonareError(ErrorCode.Cancelled, 'Vocal render cancelled');
}

function staleError(message: string): SonareError {
  return new SonareError(ErrorCode.StaleResult, message);
}

function isCancelled(active: ActiveCommand): boolean {
  return (
    active.cancelled ||
    (active.cancelBuffer !== undefined &&
      Atomics.load(new Int32Array(active.cancelBuffer), 0) !== 0)
  );
}

function nextMacrotask(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

interface ReplyTokens {
  token: VocalStateToken;
  draftToken?: VocalStateToken;
}

/** The committed token, plus the draft token while a draft is open. */
function replyTokens(session: WorkerSession): ReplyTokens {
  return {
    token: session.value.token,
    ...(session.draft ? { draftToken: session.draft.token } : {}),
  };
}

function currentToken(session: WorkerSession): VocalStateToken {
  return session.draft?.token ?? session.value.token;
}

function postResult(
  endpoint: VocalEditWorkerEndpoint,
  id: number,
  sessionId: string,
  clientIntentSequence: string,
  result: VocalWorkerResult,
  tokens?: ReplyTokens,
): void {
  endpoint.postMessage(
    {
      type: 'sonare:vocal-result',
      id,
      sessionId,
      clientIntentSequence,
      result,
      ...tokens,
    },
    transferables(result),
  );
}

function postError(
  endpoint: VocalEditWorkerEndpoint,
  id: number,
  sessionId: string,
  clientIntentSequence: string,
  error: unknown,
  tokens?: ReplyTokens,
): void {
  endpoint.postMessage({
    type: 'sonare:vocal-error',
    id,
    sessionId,
    clientIntentSequence,
    error: errorPayload(error),
    ...tokens,
  });
}

function rejectQueuedCommands(
  endpoint: VocalEditWorkerEndpoint,
  session: WorkerSession,
  error: Error,
): void {
  for (const queued of session.queue) {
    postError(
      endpoint,
      queued.message.id,
      queued.message.sessionId,
      queued.message.clientIntentSequence,
      error,
      replyTokens(session),
    );
  }
  session.queue = [];
}

// Mutations are validated by the native expectedRevision/expectedGeneration
// checks when they run, not against the token the client held at enqueue time:
// a pipelined mutation queued behind another one legitimately carries an older
// base token. Only a token from a different session epoch is refused here.
function ensureSameEpoch(
  message: VocalWorkerMutateMessage | VocalWorkerPreviewMessage,
  session: WorkerSession,
): void {
  if (message.baseToken && message.baseToken.sessionEpoch !== currentToken(session).sessionEpoch) {
    throw staleError('The vocal edit request targets another session epoch');
  }
}

function draftRequired(session: WorkerSession): VocalEditWorkerNativeDraft {
  if (!session.draft) {
    throw new SonareError(ErrorCode.InvalidState, 'No vocal edit draft is open');
  }
  return session.draft;
}

/** The open draft when `draft` is set, otherwise the session. */
function readTarget(
  session: WorkerSession,
  draft: boolean | undefined,
): VocalEditWorkerNativeDraft | VocalEditWorkerNativeSession {
  return draft === true ? draftRequired(session) : session.value;
}

async function runPreview(
  message: VocalWorkerPreviewMessage,
  session: WorkerSession,
  active: ActiveCommand,
  endpoint: VocalEditWorkerEndpoint,
): Promise<VocalRenderResult> {
  const snapshot = session.draft
    ? session.draft.captureRenderSnapshot()
    : session.value.captureRenderSnapshot();
  let job: VocalRenderJob | undefined;
  try {
    job = snapshot.beginRenderJob(message.request);
    while (true) {
      if (isCancelled(active)) {
        job.abort();
        throw cancelledError();
      }
      const progress = job.next();
      endpoint.postMessage({
        type: 'sonare:vocal-progress',
        id: message.id,
        sessionId: message.sessionId,
        clientIntentSequence: message.clientIntentSequence,
        complete: progress.complete,
        ...replyTokens(session),
      });
      if (progress.complete) {
        return job.finalize();
      }
      // One job_next call per macrotask is intentional. Without the yield a
      // cancel message cannot reach a synchronous WASM renderer on browsers
      // that do not expose SharedArrayBuffer.
      await nextMacrotask();
    }
  } finally {
    job?.destroy();
    snapshot.destroy();
  }
}

async function execute(
  command: QueuedCommand,
  session: WorkerSession,
  endpoint: VocalEditWorkerEndpoint,
): Promise<void> {
  const message = command.message;
  const active = command.active;
  session.running = active;
  try {
    ensureSameEpoch(message, session);
    let result: VocalWorkerResult;
    if (message.type === 'sonare:vocal-preview') {
      result = await runPreview(message, session, active, endpoint);
    } else {
      const mutation = message.mutation;
      switch (mutation.kind) {
        case 'beginEdit':
          if (session.draft) {
            throw new SonareError(ErrorCode.InvalidState, 'A vocal edit draft is already open');
          }
          session.draft = session.value.beginEdit(mutation.expectedRevision);
          result = session.draft.token;
          break;
        case 'apply':
          result = draftRequired(session).apply(mutation.request);
          break;
        case 'commit':
          result = draftRequired(session).commit(mutation.expectedRevision);
          session.draft?.destroy();
          session.draft = undefined;
          break;
        case 'cancel':
          draftRequired(session).cancel();
          session.draft?.destroy();
          session.draft = undefined;
          result = null;
          break;
        case 'undo':
          if (session.draft) {
            throw new SonareError(
              ErrorCode.InvalidState,
              'Cancel or commit the vocal draft before undo',
            );
          }
          result = session.value.undo(mutation.expectedRevision);
          break;
        case 'redo':
          if (session.draft) {
            throw new SonareError(
              ErrorCode.InvalidState,
              'Cancel or commit the vocal draft before redo',
            );
          }
          result = session.value.redo(mutation.expectedRevision);
          break;
        case 'notes':
          result = readTarget(session, mutation.draft).notes();
          break;
        case 'analysis':
          result = session.value.analysis();
          break;
        case 'capabilities':
          result = session.value.capabilities();
          break;
        case 'exportState':
          if (session.draft) {
            throw new SonareError(
              ErrorCode.InvalidState,
              'Commit or cancel the vocal draft before export',
            );
          }
          result = session.value.exportState();
          break;
        case 'outputLength':
          result = session.value.outputLengthSamples;
          break;
        case 'history':
          result = session.value.history();
          break;
        case 'draftToken':
          result = draftRequired(session).token;
          break;
        case 'evaluatePitch':
          result = readTarget(session, mutation.draft).evaluatePitch(mutation.noteId);
          break;
        case 'mapCoordinate': {
          const target = readTarget(session, mutation.draft);
          result = mutation.inverse
            ? target.destinationSampleToSourceSample(mutation.noteId, mutation.sample)
            : target.sourceSampleToDestinationSample(mutation.noteId, mutation.sample);
          break;
        }
      }
    }
    // A mutation that ran is reported as run: cancellation covers renders only.
    if (message.type === 'sonare:vocal-preview' && isCancelled(active)) {
      throw cancelledError();
    }
    postResult(
      endpoint,
      message.id,
      message.sessionId,
      message.clientIntentSequence,
      result,
      replyTokens(session),
    );
  } catch (error) {
    postError(
      endpoint,
      message.id,
      message.sessionId,
      message.clientIntentSequence,
      error,
      replyTokens(session),
    );
  } finally {
    session.running = undefined;
  }
}

async function drain(session: WorkerSession, endpoint: VocalEditWorkerEndpoint): Promise<void> {
  if (session.draining) {
    return;
  }
  session.draining = true;
  try {
    while (session.queue.length !== 0 && !session.disposing) {
      const command = session.queue.shift() as QueuedCommand;
      await execute(command, session, endpoint);
    }
  } finally {
    session.draining = false;
  }
}

function queueCommand(
  endpoint: VocalEditWorkerEndpoint,
  sessions: Map<string, WorkerSession>,
  message: VocalWorkerMutateMessage | VocalWorkerPreviewMessage,
): void {
  const session = sessions.get(message.sessionId);
  if (!session || session.disposing) {
    postError(
      endpoint,
      message.id,
      message.sessionId,
      message.clientIntentSequence,
      new SonareError(ErrorCode.InvalidState, 'Unknown vocal session'),
    );
    return;
  }
  if (compareUint64(message.clientIntentSequence, session.latestIntent) <= 0) {
    postError(
      endpoint,
      message.id,
      message.sessionId,
      message.clientIntentSequence,
      staleError('A stale vocal client intent was rejected'),
      replyTokens(session),
    );
    return;
  }
  session.latestIntent = message.clientIntentSequence;
  if (message.type === 'sonare:vocal-preview') {
    // Only previews that have not started may be coalesced. Mutations remain
    // FIFO so a rapid sequence of drag updates cannot reorder the edit state.
    const stale = session.queue.filter(
      (command) => command.message.type === 'sonare:vocal-preview',
    );
    session.queue = session.queue.filter(
      (command) => command.message.type !== 'sonare:vocal-preview',
    );
    for (const command of stale) {
      postError(
        endpoint,
        command.message.id,
        command.message.sessionId,
        command.message.clientIntentSequence,
        staleError('Preview superseded'),
        replyTokens(session),
      );
    }
  }
  session.queue.push({
    message,
    active: {
      id: message.id,
      cancelled: false,
      preview: message.type === 'sonare:vocal-preview',
      cancelBuffer: message.cancelBuffer,
    },
  });
  void drain(session, endpoint);
}

/** Seams a Worker entry point can replace: the native session factory and the module initialiser. */
export interface VocalEditWorkerDependencies {
  /** Native session factory; defaults to the production one. */
  factory?: VocalEditWorkerFactory;
  /** Module initialiser awaited before serving; defaults to the production one. */
  initialize?: () => Promise<void>;
}

const UINT64_MAX = '18446744073709551615';

const mutationKinds = new Set<string>([
  'beginEdit',
  'apply',
  'commit',
  'cancel',
  'undo',
  'redo',
  'notes',
  'analysis',
  'capabilities',
  'exportState',
  'outputLength',
  'history',
  'draftToken',
  'evaluatePitch',
  'mapCoordinate',
]);

function malformed(message: string): TypeError {
  return new TypeError(`malformed vocal Worker message: ${message}`);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object';
}

function isDecimalUint64(value: unknown): value is string {
  return (
    typeof value === 'string' &&
    /^(0|[1-9]\d*)$/.test(value) &&
    compareUint64(value, UINT64_MAX) <= 0
  );
}

function optionalDecimal(value: unknown, field: string): void {
  if (value !== undefined && !isDecimalUint64(value)) {
    throw malformed(`${field} must be a decimal uint64 string`);
  }
}

/** The request id a reply can be addressed to, or undefined when none can. */
function replyId(data: unknown): number | undefined {
  // A cancel names another request's id; answering it would settle that call.
  if (!isRecord(data) || data.type === 'sonare:vocal-cancel') {
    return undefined;
  }
  return Number.isSafeInteger(data.id) && (data.id as number) > 0 ? (data.id as number) : undefined;
}

/** Check the shape of an incoming message; payload contents are checked natively. */
function validateRequest(data: unknown): VocalWorkerRequestMessage {
  if (!isRecord(data)) {
    throw malformed('message must be an object');
  }
  if (!Number.isSafeInteger(data.id) || (data.id as number) <= 0) {
    throw malformed('id must be a positive safe integer');
  }
  if (typeof data.sessionId !== 'string' || data.sessionId.length === 0) {
    throw malformed('sessionId must be a non-empty string');
  }
  if (!isDecimalUint64(data.clientIntentSequence)) {
    throw malformed('clientIntentSequence must be a decimal uint64 string');
  }
  if (
    data.cancelBuffer !== undefined &&
    !(
      typeof SharedArrayBuffer !== 'undefined' &&
      data.cancelBuffer instanceof SharedArrayBuffer &&
      data.cancelBuffer.byteLength >= Int32Array.BYTES_PER_ELEMENT
    )
  ) {
    throw malformed('cancelBuffer must be a SharedArrayBuffer');
  }
  if (
    data.baseToken !== undefined &&
    !(isRecord(data.baseToken) && isDecimalUint64(data.baseToken.sessionEpoch))
  ) {
    throw malformed('baseToken must be a state token');
  }
  switch (data.type) {
    case 'sonare:vocal-create': {
      const request = data.request;
      if (
        !isRecord(request) ||
        (request.kind !== 'create' && request.kind !== 'restore') ||
        !isRecord(request.request)
      ) {
        throw malformed('create request must name create or restore with a request object');
      }
      break;
    }
    case 'sonare:vocal-mutate': {
      const mutation = data.mutation;
      if (
        !isRecord(mutation) ||
        typeof mutation.kind !== 'string' ||
        !mutationKinds.has(mutation.kind)
      ) {
        throw malformed('mutation.kind is not a vocal Worker mutation');
      }
      if (mutation.kind === 'apply' && !isRecord(mutation.request)) {
        throw malformed('apply mutation must carry a request object');
      }
      if (mutation.kind === 'commit' && !isDecimalUint64(mutation.expectedRevision)) {
        throw malformed('commit expectedRevision must be a decimal uint64 string');
      }
      optionalDecimal(mutation.expectedRevision, 'expectedRevision');
      break;
    }
    case 'sonare:vocal-preview':
      if (!isRecord(data.request)) {
        throw malformed('preview request must be an object');
      }
      break;
    case 'sonare:vocal-dispose':
    case 'sonare:vocal-cancel':
      break;
    default:
      throw malformed('unknown message type');
  }
  return data as unknown as VocalWorkerRequestMessage;
}

/** Install the dedicated vocal-edit protocol on a browser or Node endpoint. */
export function installVocalEditWorkerEndpoint(
  endpoint: VocalEditWorkerEndpoint,
  dependencies: VocalEditWorkerDependencies = {},
): void {
  const sessions = new Map<string, WorkerSession>();
  const creating = new Map<number, ActiveCommand>();
  const factory = dependencies.factory ?? productionFactory;
  const initialize = dependencies.initialize ?? init;

  const cancel = (message: VocalWorkerRequestMessage): void => {
    const pendingCreate = creating.get(message.id);
    if (pendingCreate) {
      pendingCreate.cancelled = true;
      return;
    }
    const session = sessions.get(message.sessionId);
    if (!session) {
      return;
    }
    // Mutations are FIFO and never dropped, so a cancel reaches previews only.
    const running = session.running;
    if (running?.id === message.id && running.preview) {
      running.cancelled = true;
      return;
    }
    const queuedIndex = session.queue.findIndex(
      (command) =>
        command.message.id === message.id && command.message.type === 'sonare:vocal-preview',
    );
    if (queuedIndex >= 0) {
      const [queued] = session.queue.splice(queuedIndex, 1);
      postError(
        endpoint,
        queued.message.id,
        queued.message.sessionId,
        queued.message.clientIntentSequence,
        cancelledError(),
        replyTokens(session),
      );
    }
  };

  const create = async (message: VocalWorkerCreateMessage): Promise<void> => {
    const active: ActiveCommand = {
      id: message.id,
      cancelled: false,
      preview: false,
      cancelBuffer: message.cancelBuffer,
    };
    creating.set(message.id, active);
    let value: VocalEditWorkerNativeSession | undefined;
    try {
      await initialize();
      if (isCancelled(active)) {
        throw cancelledError();
      }
      if (sessions.has(message.sessionId)) {
        throw new SonareError(ErrorCode.InvalidState, 'Vocal session id is already in use');
      }
      value =
        message.request.kind === 'create'
          ? factory.create(message.request.request)
          : factory.restore(message.request.request);
      if (isCancelled(active)) {
        throw cancelledError();
      }
      const result: VocalWorkerCreateResult = {
        sessionId: message.sessionId,
        token: value.token,
        notes: value.notes(),
        analysis: value.analysis(),
        capabilities: value.capabilities(),
      };
      sessions.set(message.sessionId, {
        value,
        latestIntent: message.clientIntentSequence,
        queue: [],
        running: undefined,
        draining: false,
        disposing: false,
      });
      value = undefined;
      postResult(endpoint, message.id, message.sessionId, message.clientIntentSequence, result, {
        token: result.token,
      });
    } catch (error) {
      value?.destroy();
      postError(endpoint, message.id, message.sessionId, message.clientIntentSequence, error);
    } finally {
      creating.delete(message.id);
    }
  };

  const dispose = (message: VocalWorkerRequestMessage): void => {
    const session = sessions.get(message.sessionId);
    if (!session) {
      // Nothing to release, but the client still waits for this reply.
      postResult(endpoint, message.id, message.sessionId, message.clientIntentSequence, null);
      return;
    }
    if (session.disposing) {
      // A repeated dispose settles with the first one.
      void session.released?.then(() =>
        postResult(endpoint, message.id, message.sessionId, message.clientIntentSequence, null),
      );
      return;
    }
    session.disposing = true;
    const active = session.running;
    if (active) {
      active.cancelled = true;
    }
    rejectQueuedCommands(endpoint, session, releasedHandleError('Vocal session', 'disposed'));
    session.released = (async (): Promise<void> => {
      while (session.draining) {
        await nextMacrotask();
      }
      session.draft?.destroy();
      session.value.destroy();
      sessions.delete(message.sessionId);
    })();
    void session.released.then(() =>
      postResult(endpoint, message.id, message.sessionId, message.clientIntentSequence, null),
    );
  };

  endpoint.addEventListener('message', (event) => {
    const data: unknown = event.data;
    try {
      const message = validateRequest(data);
      switch (message.type) {
        case 'sonare:vocal-cancel':
          cancel(message);
          return;
        case 'sonare:vocal-create':
          void create(message);
          return;
        case 'sonare:vocal-dispose':
          dispose(message);
          return;
        default:
          queueCommand(endpoint, sessions, message);
      }
    } catch (error) {
      const id = replyId(data);
      if (id === undefined) {
        return;
      }
      const record = data as Record<string, unknown>;
      try {
        postError(
          endpoint,
          id,
          typeof record.sessionId === 'string' ? record.sessionId : '',
          isDecimalUint64(record.clientIntentSequence) ? record.clientIntentSequence : '0',
          error,
        );
      } catch {
        // The endpoint itself failed; there is no channel left to report on.
      }
    }
  });
}

function browserEndpoint(): VocalEditWorkerEndpoint | null {
  const scope = globalThis as unknown as {
    document?: unknown;
    postMessage?: (message: VocalWorkerResponseMessage, transfer?: Transferable[]) => void;
    addEventListener?: (
      type: 'message',
      listener: (event: MessageEvent<VocalWorkerRequestMessage>) => void,
    ) => void;
  };
  if (scope.document !== undefined || !scope.postMessage || !scope.addEventListener) {
    return null;
  }
  return {
    postMessage: (message, transfer) => scope.postMessage?.(message, transfer),
    addEventListener: (type, listener) => scope.addEventListener?.(type, listener),
  };
}

const endpoint = browserEndpoint();
if (endpoint) {
  installVocalEditWorkerEndpoint(endpoint);
}
