import { ErrorCode, isSonareError, SonareError } from './errors.js';
import { failureFromPayload } from './internal/failure_payload.js';
import { releasedHandleError } from './internal/handle_guard.js';
import {
  compareUint64,
  stateTokenEquals,
  type VocalWorkerCreateMessage,
  type VocalWorkerCreateResult,
  type VocalWorkerErrorMessage,
  type VocalWorkerMutateMessage,
  type VocalWorkerMutation,
  type VocalWorkerPreviewMessage,
  type VocalWorkerRequestMessage,
  type VocalWorkerResponseMessage,
} from './internal/vocal_edit_worker_protocol.js';
import { attachVocalErrorDetail, isVocalErrorDetail } from './internal/vocal_error_detail.js';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditResult,
  VocalHistory,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderAsyncRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateToken,
} from './public_types_vocal_edit.js';
import { requestObject } from './validation.js';

/**
 * A browser Worker or a Node `worker_threads.Worker`. The transfer list is
 * `readonly unknown[]` because the DOM and Node `Transferable` unions are not
 * subsets of each other, so neither Worker type would otherwise be assignable.
 */
export interface VocalEditWorker {
  /** Posts a message, optionally transferring ownership of the listed buffers. */
  postMessage(message: unknown, transfer?: readonly unknown[]): void;
  /** Stops the Worker. */
  terminate(): unknown;
  /** Browser-style listener registration. */
  addEventListener?(type: string, listener: EventListener): void;
  /** Browser-style listener removal. */
  removeEventListener?(type: string, listener: EventListener): void;
  /** Node-style listener registration. */
  on?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
  /** Node-style listener removal. */
  off?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
}

/** Construction options of {@link VocalEditWorkerClient}. */
export interface VocalEditWorkerClientOptions {
  /** An existing Worker to use instead of starting one. */
  worker?: VocalEditWorker;
  /** Location of the Worker script when the client starts its own Worker. */
  workerUrl?: string | URL;
  /** Starts the Worker from a resolved URL; required outside a browser unless `worker` is given. */
  workerFactory?: (url: URL) => VocalEditWorker;
  /** Terminate a Worker passed as `worker` when the client is destroyed; the default is to leave it running. */
  terminateWorkerOnDestroy?: boolean;
}

/** Per-call transfer behaviour for a request that carries typed arrays. */
export interface VocalEditWorkerTransferOptions {
  /**
   * Preserve the caller's typed arrays by copying them before dispatch.
   *
   * The default is `false`: typed-array buffers are transferred to the Worker
   * and become detached on the calling thread, avoiding a second copy.
   */
  copy?: boolean;
}

/** Options of a call that can be aborted. */
export interface VocalEditWorkerCallOptions extends VocalEditWorkerTransferOptions {
  /** Aborts the call; it then rejects with a `Cancelled` `SonareError`. */
  signal?: AbortSignal;
}

/** What the Worker reported when it opened a session. */
export interface VocalEditWorkerCreated {
  /** The notes at creation time. */
  notes: VocalNotesResult;
  /** The pitch analysis the session was built from. */
  analysis: VocalAnalysis;
  /** What the loaded engine supports. */
  capabilities: VocalCapabilities;
}

type RevisionArgument = string | { expectedRevision?: string };

function resolveRevision(argument: RevisionArgument | undefined, fallback: string): string {
  const value = typeof argument === 'string' ? argument : argument?.expectedRevision;
  return value ?? fallback;
}

interface DraftState {
  token: VocalStateToken;
  open: boolean;
}

interface SessionState {
  readonly id: string;
  token: VocalStateToken;
  draft?: DraftState;
  disposed: boolean;
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  sessionId: string;
  intent: string;
  baseToken?: VocalStateToken;
  preview: boolean;
  mutation: boolean;
  session?: SessionState;
  cancelFlag?: Int32Array;
  signal?: AbortSignal;
  abortListener?: () => void;
}

interface CallReply<T> {
  result: T;
  token?: VocalStateToken;
  draftToken?: VocalStateToken;
}

function incrementDecimal(value: string): string {
  if (compareUint64(value, '18446744073709551615') >= 0) {
    throw new RangeError('vocal Worker intent sequence exhausted uint64');
  }
  const digits = value.split('').map((digit) => digit.charCodeAt(0) - 48);
  let carry = 1;
  for (let i = digits.length - 1; i >= 0; i--) {
    const next = digits[i] + carry;
    digits[i] = next % 10;
    carry = Math.floor(next / 10);
  }
  if (carry !== 0) {
    digits.unshift(carry);
  }
  return digits.join('');
}

function cancellationFlag(): Int32Array | undefined {
  if (typeof SharedArrayBuffer === 'undefined') {
    return undefined;
  }
  return new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT));
}

function cloneForWorker(
  value: unknown,
  copy: boolean,
  transfers: Transferable[],
  transferred = new Set<ArrayBuffer>(),
): unknown {
  if (value instanceof Float32Array) {
    const next = copy ? value.slice() : value;
    if (next.buffer instanceof ArrayBuffer && !transferred.has(next.buffer)) {
      transferred.add(next.buffer);
      transfers.push(next.buffer);
    }
    return next;
  }
  if (value instanceof Uint8Array) {
    const next = copy ? value.slice() : value;
    if (next.buffer instanceof ArrayBuffer && !transferred.has(next.buffer)) {
      transferred.add(next.buffer);
      transfers.push(next.buffer);
    }
    return next;
  }
  if (Array.isArray(value)) {
    return value.map((item) => cloneForWorker(item, copy, transfers, transferred));
  }
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value).map(([key, item]) => [
        key,
        cloneForWorker(item, copy, transfers, transferred),
      ]),
    );
  }
  return value;
}

function cloneRequest<T>(request: T, copy: boolean, transfers: Transferable[]): T {
  return cloneForWorker(request, copy, transfers) as T;
}

function workerError(message: VocalWorkerErrorMessage): Error {
  const error = failureFromPayload(message.error);
  const detail = message.error.detail;
  if (isVocalErrorDetail(detail)) {
    attachVocalErrorDetail(error, detail);
  }
  return error;
}

function sameSessionToken(a: VocalStateToken, b: VocalStateToken): boolean {
  return a.sessionEpoch === b.sessionEpoch && a.profileId === b.profileId;
}

function nextSessionId(counter: number): string {
  return `vocal-${counter.toString(36)}`;
}

function cancelledError(signal?: AbortSignal): SonareError {
  return new SonareError(ErrorCode.Cancelled, 'The operation was cancelled', {
    cause: signal?.reason,
  });
}

function staleError(): SonareError {
  return new SonareError(ErrorCode.StaleResult, 'A vocal edit Worker result is stale');
}

/** The token a preview is compared against: the open draft's, else the committed one. */
function viewToken(session: SessionState): VocalStateToken {
  return session.draft?.open ? session.draft.token : session.token;
}

/** Module-private marker; the session and draft constructors refuse a call without it. */
const CONSTRUCT = Symbol('VocalEditWorker.construct');

/** The call plumbing shared by a client and the sessions it opened. Not part of the public surface. */
class ClientCore {
  private readonly worker: VocalEditWorker;
  private readonly ownsWorker: boolean;
  private readonly pending = new Map<number, PendingCall>();
  private readonly disposeWaiters = new Map<number, () => void>();
  private readonly latestIntent = new Map<string, string>();
  private readonly sessions = new Set<SessionState>();
  private nextId = 1;
  private nextSession = 1;
  private closed = false;
  private destroying: Promise<void> | undefined;
  private usesEventTarget = false;

  constructor(options: VocalEditWorkerClientOptions) {
    this.ownsWorker = options.worker === undefined || options.terminateWorkerOnDestroy === true;
    if (options.worker) {
      this.worker = options.worker;
    } else {
      if (!options.workerFactory && typeof Worker === 'undefined') {
        throw new SonareError(
          ErrorCode.NotSupported,
          'VocalEditWorkerClient requires a Worker implementation: pass `worker` or `workerFactory` outside a browser (for example a node:worker_threads Worker)',
        );
      }
      const url =
        options.workerUrl === undefined
          ? new URL('./vocal_edit_worker.js', import.meta.url)
          : new URL(/* webpackIgnore: true */ options.workerUrl, import.meta.url);
      this.worker =
        options.workerFactory?.(url) ??
        new Worker(url, { type: 'module', name: 'sonare-vocal-edit' });
    }
    if (this.worker.addEventListener) {
      this.usesEventTarget = true;
      this.worker.addEventListener('message', this.onMessage as EventListener);
      this.worker.addEventListener('error', this.onError as EventListener);
    } else if (this.worker.on) {
      this.worker.on('message', this.onNodeMessage);
      this.worker.on('error', this.onNodeError);
    } else {
      throw new TypeError('VocalEditWorkerClient requires Worker event listeners');
    }
  }

  open(
    request: VocalWorkerCreateMessage['request'],
    options: VocalEditWorkerCallOptions,
  ): Promise<VocalEditWorkerSession> {
    const sessionId = nextSessionId(this.nextSession++);
    let reply: Promise<CallReply<VocalWorkerCreateResult>>;
    try {
      const message: VocalWorkerCreateMessage = {
        type: 'sonare:vocal-create',
        id: 0,
        sessionId,
        clientIntentSequence: this.intent(sessionId),
        request,
      };
      reply = this.call<VocalWorkerCreateResult>(message, options);
    } catch (error) {
      return Promise.reject(error);
    }
    return reply.then(({ result }) => {
      const state: SessionState = { id: sessionId, token: result.token, disposed: false };
      const session = new VocalEditWorkerSessionHandle(CONSTRUCT, this, state, {
        notes: result.notes,
        analysis: result.analysis,
        capabilities: result.capabilities,
      });
      // The Worker finished before it saw the cancellation; release its session.
      if (options.signal?.aborted) {
        if (!this.closed) {
          session.destroy().catch(() => {});
        }
        throw cancelledError(options.signal);
      }
      this.sessions.add(state);
      return session;
    });
  }

  private postCancelMessage(id: number, pending: PendingCall): void {
    if (pending.cancelFlag) {
      Atomics.store(pending.cancelFlag, 0, 1);
    }
    try {
      this.worker.postMessage({
        type: 'sonare:vocal-cancel',
        id,
        sessionId: pending.sessionId,
        clientIntentSequence: pending.intent,
      });
    } catch {
      // Disposal still rejects the local call when the Worker is unavailable.
    }
  }

  /** Posts a dispose for `sessionId`; resolves when the Worker replies or can no longer. */
  private postDisposeMessage(sessionId: string, clientIntentSequence: string): Promise<void> {
    const id = this.nextId++;
    return new Promise<void>((resolve) => {
      this.disposeWaiters.set(id, resolve);
      try {
        this.worker.postMessage({
          type: 'sonare:vocal-dispose',
          id,
          sessionId,
          clientIntentSequence,
        });
      } catch {
        // The host closed the Worker concurrently; there is no reply to wait for.
        this.disposeWaiters.delete(id);
        resolve();
      }
    });
  }

  destroy(): Promise<void> {
    if (this.destroying) {
      return this.destroying;
    }
    this.destroying = this.shutdown();
    return this.destroying;
  }

  private async shutdown(): Promise<void> {
    const pendingEntries = [...this.pending.entries()];
    const sessionIntents = new Map<string, string>();
    for (const session of this.sessions) {
      sessionIntents.set(session.id, this.latestIntent.get(session.id) ?? '0');
    }
    for (const [, pending] of pendingEntries) {
      if (!sessionIntents.has(pending.sessionId)) {
        sessionIntents.set(pending.sessionId, pending.intent);
      }
    }
    this.closed = true;
    // A create can finish after this method returns. Cancel it first, then
    // dispose every known session id to cover the race where native creation
    // has already installed the session before the cancel is observed.
    for (const [id, pending] of pendingEntries) {
      this.postCancelMessage(id, pending);
    }
    const released: Promise<void>[] = [];
    for (const [sessionId, intent] of sessionIntents) {
      released.push(this.postDisposeMessage(sessionId, intent));
    }
    for (const [, pending] of pendingEntries) {
      pending.abortListener && pending.signal?.removeEventListener('abort', pending.abortListener);
      pending.reject(releasedHandleError('VocalEditWorkerClient', 'was disposed'));
    }
    this.pending.clear();
    for (const session of this.sessions) {
      session.disposed = true;
    }
    this.sessions.clear();
    this.latestIntent.clear();
    // Terminating the Worker releases its sessions, so only a Worker that outlives the client is awaited.
    if (!this.ownsWorker) {
      await Promise.all(released);
    }
    this.disposeWaiters.clear();
    if (this.usesEventTarget) {
      this.worker.removeEventListener?.('message', this.onMessage as EventListener);
      this.worker.removeEventListener?.('error', this.onError as EventListener);
    } else {
      this.worker.off?.('message', this.onNodeMessage);
      this.worker.off?.('error', this.onNodeError);
    }
    if (this.ownsWorker) {
      await this.worker.terminate();
    }
  }

  /** Releases one session in the Worker and resolves once the Worker has acknowledged it. */
  async disposeSession(session: SessionState): Promise<void> {
    await this.call({
      type: 'sonare:vocal-dispose',
      id: 0,
      sessionId: session.id,
      clientIntentSequence: this.intent(session.id),
    });
    this.sessions.delete(session);
    this.latestIntent.delete(session.id);
  }

  mutate<T>(
    session: SessionState,
    mutation: VocalWorkerMutation,
    options: VocalEditWorkerTransferOptions = {},
  ): Promise<T> {
    const message: VocalWorkerMutateMessage = {
      type: 'sonare:vocal-mutate',
      id: 0,
      sessionId: session.id,
      clientIntentSequence: this.intent(session.id),
      baseToken: session.token,
      mutation,
    };
    // The reply tokens are adopted in onMessage, before any stale-intent check.
    return this.call<T>(message, { copy: options.copy }, session).then((reply) => reply.result);
  }

  preview(
    session: SessionState,
    request: VocalRenderAsyncRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): Promise<VocalRenderResult> {
    const signal = request.signal;
    const { signal: _ignoredSignal, ...portableRequest } = request;
    const message: VocalWorkerPreviewMessage = {
      type: 'sonare:vocal-preview',
      id: 0,
      sessionId: session.id,
      clientIntentSequence: this.intent(session.id),
      baseToken: session.token,
      request: portableRequest,
    };
    // A render result carries requestId/profile metadata in its token. It is
    // a snapshot of the state used for rendering, not a new session token, so
    // it is compared against the session state and never adopted.
    return this.call<VocalRenderResult>(message, { signal, copy: options.copy }, session).then(
      (reply) => reply.result,
    );
  }

  private intent(sessionId: string): string {
    const next = incrementDecimal(this.latestIntent.get(sessionId) ?? '0');
    this.latestIntent.set(sessionId, next);
    return next;
  }

  private call<T = unknown>(
    input: VocalWorkerRequestMessage,
    options: VocalEditWorkerCallOptions = {},
    session?: SessionState,
  ): Promise<CallReply<T>> {
    if (this.closed) {
      throw releasedHandleError('VocalEditWorkerClient', 'was disposed');
    }
    const id = this.nextId++;
    const transfers: Transferable[] = [];
    const message = cloneRequest(
      { ...input, id },
      options.copy === true,
      transfers,
    ) as VocalWorkerRequestMessage;
    const cancelFlag = cancellationFlag();
    if (cancelFlag) {
      (
        message as VocalWorkerCreateMessage | VocalWorkerMutateMessage | VocalWorkerPreviewMessage
      ).cancelBuffer = cancelFlag.buffer as SharedArrayBuffer;
    }
    const result = new Promise<CallReply<T>>((resolve, reject) => {
      if (options.signal?.aborted) {
        reject(cancelledError(options.signal));
        return;
      }
      const pending: PendingCall = {
        resolve: (value) => resolve(value as CallReply<T>),
        reject,
        sessionId: message.sessionId,
        intent: message.clientIntentSequence,
        baseToken:
          message.type === 'sonare:vocal-mutate' || message.type === 'sonare:vocal-preview'
            ? message.baseToken
            : undefined,
        preview: message.type === 'sonare:vocal-preview',
        mutation: message.type === 'sonare:vocal-mutate',
        session,
        cancelFlag,
        signal: options.signal,
      };
      if (options.signal) {
        pending.abortListener = () => {
          if (cancelFlag) {
            Atomics.store(cancelFlag, 0, 1);
          }
          this.worker.postMessage({
            type: 'sonare:vocal-cancel',
            id,
            sessionId: message.sessionId,
            clientIntentSequence: message.clientIntentSequence,
          });
        };
        options.signal.addEventListener('abort', pending.abortListener, { once: true });
      }
      this.pending.set(id, pending);
    });
    if (!options.signal?.aborted) {
      try {
        this.worker.postMessage(message, transfers);
      } catch (error) {
        // e.g. DataCloneError: the Worker never saw the call, so nothing will answer it.
        const pending = this.pending.get(id);
        if (pending) {
          this.pending.delete(id);
          if (pending.abortListener && pending.signal) {
            pending.signal.removeEventListener('abort', pending.abortListener);
          }
          pending.reject(error);
        }
      }
    }
    return result;
  }

  private readonly onMessage = (event: MessageEvent<VocalWorkerResponseMessage>): void => {
    const message = event.data;
    if (message.type === 'sonare:vocal-progress') {
      return;
    }
    const waiter = this.disposeWaiters.get(message.id);
    if (waiter) {
      this.disposeWaiters.delete(message.id);
      waiter();
      return;
    }
    const pending = this.pending.get(message.id);
    if (!pending) {
      return;
    }
    this.pending.delete(message.id);
    if (pending.abortListener && pending.signal) {
      pending.signal.removeEventListener('abort', pending.abortListener);
    }
    // Every mutation reply, result or error, carries the Worker's state after
    // it ran. Adopt it before the intent check so a superseded reply still
    // advances the tokens the next pipelined call will be compared against.
    if (
      !pending.preview &&
      pending.session &&
      message.token &&
      pending.baseToken &&
      sameSessionToken(message.token, pending.baseToken)
    ) {
      pending.session.token = message.token;
      if (message.draftToken && pending.session.draft?.open) {
        pending.session.draft.token = message.draftToken;
      }
    }
    const reply = (): CallReply<unknown> => ({
      result: (message as { result?: unknown }).result,
      token: message.token,
      draftToken: message.draftToken,
    });
    // A mutation already ran in the Worker, so its caller gets the actual
    // outcome; only previews and reads are discarded as superseded.
    if (pending.mutation) {
      if (message.type === 'sonare:vocal-error') {
        pending.reject(workerError(message));
      } else {
        pending.resolve(reply());
      }
      return;
    }
    const latest = this.latestIntent.get(pending.sessionId) ?? pending.intent;
    if (compareUint64(pending.intent, latest) < 0) {
      pending.reject(staleError());
      return;
    }
    if (pending.preview && pending.session) {
      // A render reports the state it was produced from; an error reports the Worker's current view.
      const observed =
        message.type === 'sonare:vocal-result'
          ? (message.result as VocalRenderResult | null)?.token
          : (message.draftToken ?? message.token);
      if (!observed || !stateTokenEquals(observed, viewToken(pending.session))) {
        pending.reject(staleError());
        return;
      }
    } else if (
      message.token &&
      pending.baseToken &&
      !sameSessionToken(message.token, pending.baseToken)
    ) {
      pending.reject(staleError());
      return;
    }
    if (message.type === 'sonare:vocal-error') {
      pending.reject(workerError(message));
      return;
    }
    pending.resolve(reply());
  };

  private readonly onError = (event: ErrorEvent): void => {
    const error = new SonareError(ErrorCode.Unknown, event.message || 'Vocal edit Worker failed');
    for (const pending of this.pending.values()) {
      pending.abortListener && pending.signal?.removeEventListener('abort', pending.abortListener);
      pending.reject(error);
    }
    this.pending.clear();
    for (const waiter of this.disposeWaiters.values()) {
      waiter();
    }
    this.disposeWaiters.clear();
  };

  private readonly onNodeMessage = (data: unknown): void => {
    this.onMessage({ data } as MessageEvent<VocalWorkerResponseMessage>);
  };

  private readonly onNodeError = (error: unknown): void => {
    this.onError({ message: error instanceof Error ? error.message : String(error) } as ErrorEvent);
  };
}

/**
 * Runs vocal-edit sessions in a Worker so analysis and rendering stay off the calling
 * thread. A session mirrors {@link VocalEditSession}, with every call returning a promise.
 */
export class VocalEditWorkerClient {
  private readonly core: ClientCore;

  /** Starts a Worker, or adopts the one in `options.worker`. */
  constructor(options: VocalEditWorkerClientOptions = {}) {
    this.core = new ClientCore(options);
  }

  /** Analyses mono PCM in the Worker and opens an editing session. */
  create(
    request: VocalCreateRequest,
    options: VocalEditWorkerCallOptions = {},
  ): Promise<VocalEditWorkerSession> {
    return this.core.open({ kind: 'create', request }, options);
  }

  /** Opens a session in the Worker from an exported state blob and the same source PCM. */
  restore(
    request: VocalRestoreRequest,
    options: VocalEditWorkerCallOptions = {},
  ): Promise<VocalEditWorkerSession> {
    return this.core.open({ kind: 'restore', request }, options);
  }

  /**
   * Stop accepting calls, cancel outstanding work (rejected with `InvalidState`),
   * release every open session, and terminate the Worker when this client owns
   * it. Resolves once the Worker has acknowledged the release of every session;
   * a second call returns the same promise.
   */
  destroy(): Promise<void> {
    return this.core.destroy();
  }

  /** Same as {@link destroy}, for `await using`. */
  [Symbol.asyncDispose](): Promise<void> {
    return this.destroy();
  }
}

/** An open batch of edits on a {@link VocalEditWorkerSession}. */
export class VocalEditWorkerDraft {
  private readonly core: ClientCore;
  private readonly sessionState: SessionState;
  private readonly draftState: DraftState;
  private destroying: Promise<void> | undefined;

  /** @internal Created by {@link VocalEditWorkerSession.beginEdit}. */
  protected constructor(
    guard: typeof CONSTRUCT,
    core: ClientCore,
    sessionState: SessionState,
    draftState: DraftState,
  ) {
    if (guard !== CONSTRUCT) {
      throw new TypeError('VocalEditWorkerDraft is created by VocalEditWorkerSession.beginEdit');
    }
    this.core = core;
    this.sessionState = sessionState;
    this.draftState = draftState;
  }

  /** The draft's token as of the last reply the client received. */
  get token(): VocalStateToken {
    this.requireOpen();
    return { ...this.draftState.token };
  }

  /** The revision the draft was begun from, a decimal string (64-bit counter). */
  get revision(): string {
    return this.token.revision;
  }

  /** The draft's notes and transitions, including uncommitted edits. */
  async notes(): Promise<VocalNotesResult> {
    this.requireOpen();
    return this.core.mutate(this.sessionState, { kind: 'notes', draft: true });
  }

  /**
   * Applies the operations as one batch. Omitting `expectedGeneration` uses the
   * generation the draft last reported.
   */
  async apply(
    request: VocalApplyRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): Promise<VocalEditResult> {
    this.requireOpen();
    requestObject('vocalApply', request, 'request', true);
    return this.core.mutate(
      this.sessionState,
      {
        kind: 'apply',
        request: {
          expectedGeneration: request.expectedGeneration ?? this.draftState.token.generation,
          operations: request.operations,
        },
      },
      options,
    );
  }

  /** Commits the draft and releases it. `expectedRevision` defaults to the session's revision. */
  async commit(options: RevisionArgument = {}): Promise<VocalEditResult> {
    this.requireOpen();
    const result = await this.core.mutate<VocalEditResult>(this.sessionState, {
      kind: 'commit',
      expectedRevision: resolveRevision(options, this.sessionState.token.revision),
    });
    this.close();
    return result;
  }

  /** Discards the draft and releases it. */
  async cancel(): Promise<void> {
    this.requireOpen();
    await this.core.mutate<null>(this.sessionState, { kind: 'cancel' });
    this.close();
  }

  /** Per-frame pitch of one note including uncommitted edits. */
  async evaluatePitch(noteId: number): Promise<VocalPitchEvaluation> {
    this.requireOpen();
    return this.core.mutate(this.sessionState, { kind: 'evaluatePitch', noteId, draft: true });
  }

  /** Maps a source sample of a note to its position in the output, including uncommitted edits. */
  async sourceSampleToDestinationSample(noteId: number, sourceSample: number): Promise<number> {
    this.requireOpen();
    return this.core.mutate(this.sessionState, {
      kind: 'mapCoordinate',
      noteId,
      sample: sourceSample,
      inverse: false,
      draft: true,
    });
  }

  /** Maps an output sample of a note back to its source position, including uncommitted edits. */
  async destinationSampleToSourceSample(
    noteId: number,
    destinationSample: number,
  ): Promise<number> {
    this.requireOpen();
    return this.core.mutate(this.sessionState, {
      kind: 'mapCoordinate',
      noteId,
      sample: destinationSample,
      inverse: true,
      draft: true,
    });
  }

  /**
   * Renders the draft in the Worker. A newer preview supersedes an older one, which then
   * rejects with a `StaleResult` error; `request.signal` aborts with a `Cancelled` error.
   */
  async preview(
    request: VocalRenderAsyncRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): Promise<VocalRenderResult> {
    this.requireOpen();
    return this.core.preview(this.sessionState, request, options);
  }

  /**
   * Discards the draft if it is still open and resolves once the Worker has acknowledged it;
   * a draft already committed, cancelled or released with its session resolves at once.
   * A second call returns the same promise.
   */
  destroy(): Promise<void> {
    if (this.destroying) {
      return this.destroying;
    }
    const open = this.draftState.open && !this.sessionState.disposed;
    this.destroying = open
      ? (async () => {
          try {
            await this.cancel();
          } catch (error) {
            // A client destroyed meanwhile has already released the draft with its session.
            if (!isSonareError(error) || error.code !== ErrorCode.InvalidState) {
              throw error;
            }
          }
        })()
      : Promise.resolve();
    return this.destroying;
  }

  /** Same as {@link destroy}, for `await using`. */
  [Symbol.asyncDispose](): Promise<void> {
    return this.destroy();
  }

  private close(): void {
    this.draftState.open = false;
    if (this.sessionState.draft === this.draftState) {
      this.sessionState.draft = undefined;
    }
  }

  private requireOpen(): void {
    if (!this.draftState.open || this.sessionState.disposed) {
      throw releasedHandleError('vocal Worker draft', 'is no longer open');
    }
  }
}

/** A vocal editing session living in a Worker. */
export class VocalEditWorkerSession {
  /** Identifier of the session inside the Worker. */
  readonly sessionId: string;
  /** What the Worker reported when it opened the session. */
  readonly created: VocalEditWorkerCreated;
  private readonly core: ClientCore;
  private readonly state: SessionState;
  private destroying: Promise<void> | undefined;

  /** @internal Created by {@link VocalEditWorkerClient.create} and {@link VocalEditWorkerClient.restore}. */
  protected constructor(
    guard: typeof CONSTRUCT,
    core: ClientCore,
    state: SessionState,
    created: VocalEditWorkerCreated,
  ) {
    if (guard !== CONSTRUCT) {
      throw new TypeError('VocalEditWorkerSession is created by VocalEditWorkerClient');
    }
    this.core = core;
    this.state = state;
    this.sessionId = state.id;
    this.created = created;
  }

  /** The committed token as of the last reply the client received. */
  get token(): VocalStateToken {
    this.requireAlive();
    return { ...this.state.token };
  }

  /** The committed revision, a decimal string (64-bit counter); follows replies already received. */
  get revision(): string {
    return this.token.revision;
  }

  /** The committed notes and transitions. */
  async notes(): Promise<VocalNotesResult> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'notes' });
  }

  /** The pitch analysis the session was built from. */
  async analysis(): Promise<VocalAnalysis> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'analysis' });
  }

  /** What the loaded engine supports. */
  async capabilities(): Promise<VocalCapabilities> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'capabilities' });
  }

  /** Length of the rendered output in samples; a round trip to the Worker. */
  async outputLengthSamples(): Promise<number> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'outputLength' });
  }

  /** Whether undo and redo have anything to act on. */
  async history(): Promise<VocalHistory> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'history' });
  }

  /** Per-frame pitch of one committed note. */
  async evaluatePitch(noteId: number): Promise<VocalPitchEvaluation> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'evaluatePitch', noteId, draft: false });
  }

  /** Maps a source sample of a note to its position in the output. */
  async sourceSampleToDestinationSample(noteId: number, sourceSample: number): Promise<number> {
    this.requireAlive();
    return this.core.mutate(this.state, {
      kind: 'mapCoordinate',
      noteId,
      sample: sourceSample,
      inverse: false,
      draft: false,
    });
  }

  /** Maps an output sample of a note back to its source position. */
  async destinationSampleToSourceSample(
    noteId: number,
    destinationSample: number,
  ): Promise<number> {
    this.requireAlive();
    return this.core.mutate(this.state, {
      kind: 'mapCoordinate',
      noteId,
      sample: destinationSample,
      inverse: true,
      draft: false,
    });
  }

  /**
   * Opens a draft for batching edits. `expectedRevision` defaults to the revision the
   * client last saw; a mismatch is refused with a `revisionConflict` detail.
   */
  async beginEdit(options: RevisionArgument = {}): Promise<VocalEditWorkerDraft> {
    this.requireAlive();
    if (this.state.draft?.open) {
      throw new SonareError(ErrorCode.InvalidState, 'A vocal edit draft is already open');
    }
    const token = await this.core.mutate<VocalStateToken>(this.state, {
      kind: 'beginEdit',
      expectedRevision: resolveRevision(options, this.state.token.revision),
    });
    const draftState: DraftState = { token, open: true };
    this.state.draft = draftState;
    return new VocalEditWorkerDraftHandle(CONSTRUCT, this.core, this.state, draftState);
  }

  /** Reverts the last commit. `expectedRevision` defaults to the current revision. */
  async undo(options: RevisionArgument = {}): Promise<VocalEditResult> {
    this.requireAlive();
    return this.core.mutate(this.state, {
      kind: 'undo',
      expectedRevision: resolveRevision(options, this.state.token.revision),
    });
  }

  /** Re-applies the last undone commit. `expectedRevision` defaults to the current revision. */
  async redo(options: RevisionArgument = {}): Promise<VocalEditResult> {
    this.requireAlive();
    return this.core.mutate(this.state, {
      kind: 'redo',
      expectedRevision: resolveRevision(options, this.state.token.revision),
    });
  }

  /** Serialises the edit state; pass it to {@link VocalEditWorkerClient.restore} with the same source PCM. */
  async exportState(): Promise<Uint8Array> {
    this.requireAlive();
    return this.core.mutate(this.state, { kind: 'exportState' });
  }

  /**
   * Renders in the Worker: the open draft when there is one, otherwise the committed state.
   * A newer preview supersedes an older one, which then rejects with a `StaleResult` error;
   * `request.signal` aborts with a `Cancelled` error.
   */
  async preview(
    request: VocalRenderAsyncRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): Promise<VocalRenderResult> {
    this.requireAlive();
    return this.core.preview(this.state, request, options);
  }

  /**
   * Releases the Worker session and any open draft, resolving once the Worker
   * has acknowledged it; later calls on the session are rejected. A second
   * call returns the same promise.
   */
  destroy(): Promise<void> {
    if (this.destroying) {
      return this.destroying;
    }
    this.state.disposed = true;
    if (this.state.draft) {
      this.state.draft.open = false;
    }
    this.destroying = (async () => {
      try {
        await this.core.disposeSession(this.state);
      } catch (error) {
        // A client destroyed meanwhile has already released the session.
        if (!isSonareError(error) || error.code !== ErrorCode.InvalidState) {
          throw error;
        }
      }
    })();
    return this.destroying;
  }

  /** Same as {@link destroy}, for `await using`. */
  [Symbol.asyncDispose](): Promise<void> {
    return this.destroy();
  }

  private requireAlive(): void {
    if (this.state.disposed) {
      throw releasedHandleError('vocal Worker session', 'has already been disposed');
    }
  }
}

// The public wrappers have protected constructors; these subclasses are the module's only
// construction path, and the guard still refuses a call from outside it at runtime.
class VocalEditWorkerDraftHandle extends VocalEditWorkerDraft {
  constructor(
    guard: typeof CONSTRUCT,
    core: ClientCore,
    sessionState: SessionState,
    draftState: DraftState,
  ) {
    super(guard, core, sessionState, draftState);
  }
}

class VocalEditWorkerSessionHandle extends VocalEditWorkerSession {
  constructor(
    guard: typeof CONSTRUCT,
    core: ClientCore,
    state: SessionState,
    created: VocalEditWorkerCreated,
  ) {
    super(guard, core, state, created);
  }
}
