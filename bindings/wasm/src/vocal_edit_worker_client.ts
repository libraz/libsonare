import { SonareError } from './errors';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditResult,
  VocalHistoryState,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateBytes,
  VocalStateToken,
  VocalUint64,
} from './public_types_vocal_edit';
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
} from './vocal_edit_worker_protocol';

export interface VocalEditWorker {
  postMessage(message: unknown, transfer?: Transferable[]): void;
  terminate(): unknown;
  addEventListener?(type: string, listener: EventListener): void;
  removeEventListener?(type: string, listener: EventListener): void;
  on?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
  off?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
}

export interface VocalEditWorkerClientOptions {
  worker?: VocalEditWorker;
  workerUrl?: string | URL;
  workerFactory?: (url: URL) => VocalEditWorker;
  terminateWorkerOnDispose?: boolean;
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

export interface VocalEditWorkerCallOptions extends VocalEditWorkerTransferOptions {
  signal?: AbortSignal;
}

export class VocalEditWorkerStaleResultError extends Error {
  constructor(message = 'A vocal edit Worker result is stale') {
    super(message);
    this.name = 'StaleResultError';
  }
}

export class VocalEditWorkerTask<T> implements PromiseLike<T> {
  constructor(
    readonly result: Promise<T>,
    private readonly cancelRequest: () => void,
  ) {}

  cancel(): void {
    this.cancelRequest();
  }

  // biome-ignore lint/suspicious/noThenProperty: PromiseLike intentionally exposes then.
  then<TResult1 = T, TResult2 = never>(
    onfulfilled?: ((value: T) => TResult1 | PromiseLike<TResult1>) | null,
    onrejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null,
  ): Promise<TResult1 | TResult2> {
    return this.result.then(onfulfilled, onrejected);
  }

  catch<TResult = never>(
    onrejected?: ((reason: unknown) => TResult | PromiseLike<TResult>) | null,
  ): Promise<T | TResult> {
    return this.result.catch(onrejected);
  }

  finally(onfinally?: (() => void) | null): Promise<T> {
    return this.result.finally(onfinally);
  }
}

type RevisionArgument = VocalUint64 | { expectedRevision?: VocalUint64 };

function resolveRevision(
  argument: RevisionArgument | undefined,
  fallback: VocalUint64,
): VocalUint64 {
  const value = typeof argument === 'string' ? argument : argument?.expectedRevision;
  return value ?? fallback;
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  sessionId: string;
  intent: VocalUint64;
  baseToken?: VocalStateToken;
  preview: boolean;
  session?: VocalEditWorkerSession;
  cancelFlag?: Int32Array;
  signal?: AbortSignal;
  abortListener?: () => void;
}

function incrementDecimal(value: VocalUint64): VocalUint64 {
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
  if (message.error.name === 'StaleResultError') {
    return new VocalEditWorkerStaleResultError(message.error.message);
  }
  const { code, codeName } = message.error;
  const error =
    message.error.name === 'SonareError' && typeof code === 'number'
      ? new SonareError(code, codeName ?? '', message.error.message)
      : new Error(message.error.message);
  error.name = message.error.name;
  for (const key of [
    'code',
    'reason',
    'field',
    'expected',
    'actual',
    'expectedText',
    'actualText',
  ] as const) {
    const value = message.error[key];
    if (value !== undefined) {
      Object.defineProperty(error, key, { value, enumerable: true, configurable: true });
    }
  }
  return error;
}

function sameSessionToken(a: VocalStateToken, b: VocalStateToken): boolean {
  return a.sessionEpoch === b.sessionEpoch && a.profileId === b.profileId;
}

function nextSessionId(counter: number): string {
  return `vocal-${counter.toString(36)}`;
}

function abortError(): DOMException {
  return new DOMException('The operation was aborted', 'AbortError');
}

export class VocalEditWorkerClient {
  private readonly worker: VocalEditWorker;
  private readonly ownsWorker: boolean;
  private readonly pending = new Map<number, PendingCall>();
  private readonly latestIntent = new Map<string, VocalUint64>();
  private readonly sessions = new Set<VocalEditWorkerSession>();
  private nextId = 1;
  private nextSession = 1;
  private closed = false;
  private usesEventTarget = false;

  constructor(options: VocalEditWorkerClientOptions = {}) {
    this.ownsWorker = options.worker === undefined || options.terminateWorkerOnDispose === true;
    if (options.worker) {
      this.worker = options.worker;
    } else {
      if (!options.workerFactory && typeof Worker === 'undefined') {
        throw new Error('VocalEditWorkerClient requires a Worker implementation');
      }
      const url =
        options.workerUrl === undefined
          ? new URL('./vocal_edit_worker.js', import.meta.url)
          : new URL(options.workerUrl, import.meta.url);
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

  create(
    request: VocalCreateRequest,
    options: VocalEditWorkerCallOptions = {},
  ): VocalEditWorkerTask<VocalEditWorkerSession> {
    return this.openSession({ kind: 'create', request }, options);
  }

  restore(
    request: VocalRestoreRequest,
    options: VocalEditWorkerCallOptions = {},
  ): VocalEditWorkerTask<VocalEditWorkerSession> {
    return this.openSession({ kind: 'restore', request }, options);
  }

  private postDisposeMessage(sessionId: string, clientIntentSequence: VocalUint64): void {
    try {
      this.worker.postMessage({
        type: 'sonare:vocal-dispose',
        id: this.nextId++,
        sessionId,
        clientIntentSequence,
      });
    } catch {
      // Disposal is best effort when a host closes the Worker concurrently.
    }
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
      // Disposal still rejects the local task when the Worker is unavailable.
    }
  }

  dispose(): void {
    if (this.closed) {
      return;
    }
    const pendingEntries = [...this.pending.entries()];
    const sessionIntents = new Map<string, VocalUint64>();
    for (const session of this.sessions) {
      sessionIntents.set(session.sessionId, this.latestIntent.get(session.sessionId) ?? '0');
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
    for (const [sessionId, intent] of sessionIntents) {
      this.postDisposeMessage(sessionId, intent);
    }
    if (this.usesEventTarget) {
      this.worker.removeEventListener?.('message', this.onMessage as EventListener);
      this.worker.removeEventListener?.('error', this.onError as EventListener);
    } else {
      this.worker.off?.('message', this.onNodeMessage);
      this.worker.off?.('error', this.onNodeError);
    }
    for (const [, pending] of pendingEntries) {
      pending.abortListener && pending.signal?.removeEventListener('abort', pending.abortListener);
      pending.reject(new Error('VocalEditWorkerClient was disposed'));
    }
    this.pending.clear();
    this.sessions.clear();
    this.latestIntent.clear();
    if (this.ownsWorker) {
      this.worker.terminate();
    }
  }

  /** @internal Called by VocalEditWorkerSession. */
  disposeSession(session: VocalEditWorkerSession): VocalEditWorkerTask<null> {
    const task = this.call({
      type: 'sonare:vocal-dispose',
      id: 0,
      sessionId: session.sessionId,
      clientIntentSequence: this.intent(session.sessionId),
    });
    const result = task.result.then(() => {
      this.sessions.delete(session);
      this.latestIntent.delete(session.sessionId);
      return null;
    });
    return new VocalEditWorkerTask(result, () => task.cancel());
  }

  /** @internal */
  callMutation<T>(
    session: VocalEditWorkerSession,
    mutation: VocalWorkerMutation,
    options: VocalEditWorkerTransferOptions = {},
  ): VocalEditWorkerTask<T> {
    const message: VocalWorkerMutateMessage = {
      type: 'sonare:vocal-mutate',
      id: 0,
      sessionId: session.sessionId,
      clientIntentSequence: this.intent(session.sessionId),
      baseToken: session.currentToken(),
      mutation,
    };
    // The reply token is adopted in onMessage, before any stale-intent check.
    const task = this.call<T>(message, { copy: options.copy }, session);
    const result = task.result.then((response) => response.result);
    // Mutations are FIFO and never dropped, so a mutation cannot be cancelled.
    return new VocalEditWorkerTask(result, () => {});
  }

  /** @internal */
  callPreview(
    session: VocalEditWorkerSession,
    request: VocalRenderRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): VocalEditWorkerTask<VocalRenderResult> {
    const signal = request.signal;
    const { signal: _ignoredSignal, ...portableRequest } = request;
    const message: VocalWorkerPreviewMessage = {
      type: 'sonare:vocal-preview',
      id: 0,
      sessionId: session.sessionId,
      clientIntentSequence: this.intent(session.sessionId),
      baseToken: session.currentToken(),
      request: portableRequest,
    };
    const task = this.call<VocalRenderResult>(message, { signal, copy: options.copy }, session);
    // A render result carries requestId/profile metadata in its token. It is
    // a snapshot of the state used for rendering, not a new session token, so
    // it is compared against the session token and never adopted.
    const result = task.result.then((response) => response.result);
    return new VocalEditWorkerTask(result, () => task.cancel());
  }

  private intent(sessionId: string): VocalUint64 {
    const next = incrementDecimal(this.latestIntent.get(sessionId) ?? '0');
    this.latestIntent.set(sessionId, next);
    return next;
  }

  private openSession(
    request: VocalWorkerCreateMessage['request'],
    options: VocalEditWorkerCallOptions,
  ): VocalEditWorkerTask<VocalEditWorkerSession> {
    const sessionId = nextSessionId(this.nextSession++);
    let cancelled = false;
    const task = this.callCreate(sessionId, request, options);
    const result = task.result.then((value) => {
      const session = new VocalEditWorkerSession(this, sessionId, value.token, value);
      // The Worker finished before it saw the cancellation; release its session.
      if (cancelled || options.signal?.aborted) {
        if (!this.closed) {
          session.dispose().result.catch(() => {});
        }
        throw abortError();
      }
      this.sessions.add(session);
      return session;
    });
    return new VocalEditWorkerTask(result, () => {
      cancelled = true;
      task.cancel();
    });
  }

  private callCreate(
    sessionId: string,
    request: VocalWorkerCreateMessage['request'],
    options: VocalEditWorkerCallOptions,
  ): VocalEditWorkerTask<VocalWorkerCreateResult> {
    const message: VocalWorkerCreateMessage = {
      type: 'sonare:vocal-create',
      id: 0,
      sessionId,
      clientIntentSequence: this.intent(sessionId),
      request,
    };
    const task = this.call<unknown>(message, options);
    const result = task.result.then((response) => response.result as VocalWorkerCreateResult);
    return new VocalEditWorkerTask(result, () => task.cancel());
  }

  private call<T = unknown>(
    input: VocalWorkerRequestMessage,
    options: VocalEditWorkerCallOptions = {},
    session?: VocalEditWorkerSession,
  ): VocalEditWorkerTask<{ result: T; token?: VocalStateToken }> {
    if (this.closed) {
      throw new Error('VocalEditWorkerClient was disposed');
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
    const result = new Promise<{ result: T; token?: VocalStateToken }>((resolve, reject) => {
      if (options.signal?.aborted) {
        reject(abortError());
        return;
      }
      const pending: PendingCall = {
        resolve: (value) => resolve(value as { result: T; token?: VocalStateToken }),
        reject,
        sessionId: message.sessionId,
        intent: message.clientIntentSequence,
        baseToken:
          message.type === 'sonare:vocal-mutate' || message.type === 'sonare:vocal-preview'
            ? message.baseToken
            : undefined,
        preview: message.type === 'sonare:vocal-preview',
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
    return new VocalEditWorkerTask(result, () => {
      const pending = this.pending.get(id);
      if (!pending) {
        return;
      }
      if (pending.cancelFlag) {
        Atomics.store(pending.cancelFlag, 0, 1);
      }
      this.worker.postMessage({
        type: 'sonare:vocal-cancel',
        id,
        sessionId: pending.sessionId,
        clientIntentSequence: pending.intent,
      });
    });
  }

  private readonly onMessage = (event: MessageEvent<VocalWorkerResponseMessage>): void => {
    const message = event.data;
    if (message.type === 'sonare:vocal-progress') {
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
    // advances the token the next pipelined call will be compared against.
    if (
      !pending.preview &&
      pending.session &&
      message.token &&
      pending.baseToken &&
      sameSessionToken(message.token, pending.baseToken)
    ) {
      pending.session.updateToken(message.token);
    }
    const latest = this.latestIntent.get(pending.sessionId) ?? pending.intent;
    if (compareUint64(pending.intent, latest) < 0) {
      pending.reject(new VocalEditWorkerStaleResultError());
      return;
    }
    if (message.token && pending.baseToken) {
      const tokenMatches =
        pending.preview && pending.session
          ? stateTokenEquals(message.token, pending.session.latestToken())
          : sameSessionToken(message.token, pending.baseToken);
      if (!tokenMatches) {
        pending.reject(new VocalEditWorkerStaleResultError());
        return;
      }
    }
    if (pending.preview && !message.token) {
      pending.reject(new VocalEditWorkerStaleResultError());
      return;
    }
    if (message.type === 'sonare:vocal-error') {
      pending.reject(workerError(message));
      return;
    }
    pending.resolve({ result: message.result, token: message.token });
  };

  private readonly onError = (event: ErrorEvent): void => {
    const error = new Error(event.message || 'Vocal edit Worker failed');
    for (const pending of this.pending.values()) {
      pending.reject(error);
    }
    this.pending.clear();
  };

  private readonly onNodeMessage = (data: unknown): void => {
    this.onMessage({ data } as MessageEvent<VocalWorkerResponseMessage>);
  };

  private readonly onNodeError = (error: unknown): void => {
    this.onError({ message: error instanceof Error ? error.message : String(error) } as ErrorEvent);
  };
}

/** Selects the open draft instead of the committed session for a read-only call. */
export interface VocalEditWorkerReadOptions {
  draft?: boolean;
}

export class VocalEditWorkerSession {
  readonly sessionId: string;
  private tokenValue: VocalStateToken;
  private disposed = false;
  readonly created: VocalWorkerCreateResult;
  private readonly client: VocalEditWorkerClient;

  /** @internal Created by VocalEditWorkerClient.create/restore. */
  constructor(
    client: VocalEditWorkerClient,
    sessionId: string,
    token: VocalStateToken,
    created: VocalWorkerCreateResult,
  ) {
    this.client = client;
    this.sessionId = sessionId;
    this.tokenValue = token;
    this.created = created;
  }

  currentToken(): VocalStateToken {
    this.requireAlive();
    return this.tokenValue;
  }

  updateToken(token: VocalStateToken): void {
    this.tokenValue = token;
  }

  /** @internal The adopted token, readable after dispose for reply checks. */
  latestToken(): VocalStateToken {
    return this.tokenValue;
  }

  token(): VocalStateToken {
    this.requireAlive();
    return { ...this.tokenValue };
  }

  /** The adopted revision; like {@link token}, it follows replies already received. */
  revision(): VocalUint64 {
    return this.token().revision;
  }

  notes(): VocalEditWorkerTask<VocalNotesResult> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'notes' });
  }

  analysis(): VocalEditWorkerTask<VocalAnalysis> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'analysis' });
  }

  capabilities(): VocalEditWorkerTask<VocalCapabilities> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'capabilities' });
  }

  outputLengthSamples(): VocalEditWorkerTask<number> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'outputLength' });
  }

  history(): VocalEditWorkerTask<VocalHistoryState> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'history' });
  }

  /** Token of the open draft; rejects when no draft is open. */
  draftToken(): VocalEditWorkerTask<VocalStateToken> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'draftToken' });
  }

  evaluatePitch(
    noteId: number,
    options: VocalEditWorkerReadOptions = {},
  ): VocalEditWorkerTask<VocalPitchEvaluation> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'evaluatePitch',
      noteId,
      draft: options.draft === true,
    });
  }

  sourceSampleToDestinationSample(
    noteId: number,
    sourceSample: number,
    options: VocalEditWorkerReadOptions = {},
  ): VocalEditWorkerTask<number> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'mapCoordinate',
      noteId,
      sample: sourceSample,
      inverse: false,
      draft: options.draft === true,
    });
  }

  destinationSampleToSourceSample(
    noteId: number,
    destinationSample: number,
    options: VocalEditWorkerReadOptions = {},
  ): VocalEditWorkerTask<number> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'mapCoordinate',
      noteId,
      sample: destinationSample,
      inverse: true,
      draft: options.draft === true,
    });
  }

  beginEdit(options: RevisionArgument = {}): VocalEditWorkerTask<VocalStateToken> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'beginEdit',
      expectedRevision: resolveRevision(options, this.tokenValue.revision),
    });
  }

  apply(
    request: VocalApplyRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): VocalEditWorkerTask<VocalEditResult> {
    this.requireAlive();
    if (request.expectedGeneration === undefined) {
      throw new TypeError('expectedGeneration is required');
    }
    return this.client.callMutation(this, { kind: 'apply', request }, options);
  }

  commit(options: RevisionArgument = {}): VocalEditWorkerTask<VocalEditResult> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'commit',
      expectedRevision: resolveRevision(options, this.tokenValue.revision),
    });
  }

  cancel(): VocalEditWorkerTask<null> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'cancel' });
  }

  undo(options: RevisionArgument = {}): VocalEditWorkerTask<VocalEditResult> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'undo',
      expectedRevision: resolveRevision(options, this.tokenValue.revision),
    });
  }

  redo(options: RevisionArgument = {}): VocalEditWorkerTask<VocalEditResult> {
    this.requireAlive();
    return this.client.callMutation(this, {
      kind: 'redo',
      expectedRevision: resolveRevision(options, this.tokenValue.revision),
    });
  }

  exportState(): VocalEditWorkerTask<VocalStateBytes> {
    this.requireAlive();
    return this.client.callMutation(this, { kind: 'exportState' });
  }

  preview(
    request: VocalRenderRequest,
    options: VocalEditWorkerTransferOptions = {},
  ): VocalEditWorkerTask<VocalRenderResult> {
    this.requireAlive();
    return this.client.callPreview(this, request, options);
  }

  /** Releases the Worker session and any open draft; later calls on it are rejected. */
  dispose(): VocalEditWorkerTask<null> {
    if (this.disposed) {
      return new VocalEditWorkerTask(Promise.resolve(null), () => {});
    }
    this.disposed = true;
    return this.client.disposeSession(this);
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw new TypeError('vocal Worker session has already been disposed');
    }
  }
}
