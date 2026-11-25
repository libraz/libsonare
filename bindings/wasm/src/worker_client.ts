import { ErrorCode, type SonareError } from './errors.js';
import { failureFromPayload, isFailurePayload } from './internal/failure_payload.js';
import './lifetime.js';
import { codedError, entryClass, refuseType } from './internal/entry.js';
import { errorText } from './internal/failure_payload.js';
import { releasedHandleError } from './internal/handle_guard.js';
import { checkWorkerCallOptions } from './internal/worker_call_options.js';
import type { MasterAudioRequest, MasterAudioStereoRequest } from './mastering_chain.js';
import type { ProgressCallback } from './public_types.js';
import type {
  AnalyzeRequest,
  DetectBpmRequest,
  DetectChordsRequest,
  DetectKeyRequest,
} from './quick_analysis.js';
import type { OfflineWorkerOperation, OfflineWorkerResponseMessage } from './worker_protocol.js';

/**
 * A request as a worker call takes it: `onProgress` stays on the calling thread
 * and is invoked with the worker's progress, and the synchronous `cancel`
 * predicate is not accepted because a function cannot cross the thread.
 */
type WorkerRequest<T> = Omit<T, 'onProgress' | 'cancel'> & { onProgress?: ProgressCallback };

/** Per-call behaviour for a one-shot operation. */
export interface OfflineWorkerCallOptions {
  /**
   * Preserve the caller's input arrays by copying them before dispatch.
   *
   * The default is `false`: `Float32Array` buffers are transferred to the
   * Worker and become detached on the calling thread, avoiding a second copy.
   */
  copy?: boolean;
  /**
   * Cancels the call. The promise rejects with a `SonareError` of code
   * `Cancelled` whose `cause` is `signal.reason`, and the Worker is told to stop
   * at the next native progress boundary. A signal already aborted rejects
   * without dispatching work.
   */
  signal?: AbortSignal;
}

/** Options used to create an {@link OfflineWorkerClient}. */
export interface OfflineWorkerClientOptions {
  /** Reuse a Worker owned by the host instead of creating one. */
  worker?: OfflineWorker;
  /** URL of the Worker entry; defaults to the adjacent published `worker.js`. */
  workerUrl?: string | URL;
  /** Test/host hook for creating a dedicated Worker. */
  workerFactory?: (url: URL) => OfflineWorker;
  /** Terminate a supplied Worker when {@link OfflineWorkerClient.destroy} runs. */
  terminateWorkerOnDestroy?: boolean;
}

/** Browser Worker or Node `worker_threads.Worker` used by the client. */
export interface OfflineWorker {
  /** Sends a request message, transferring the listed buffers. */
  postMessage(message: unknown, transfer?: readonly unknown[]): void;
  /** Stops the worker. */
  terminate(): unknown;
  /** Browser listener registration; the client uses it whenever it is present. */
  addEventListener?(type: string, listener: EventListener): void;
  /** Browser listener removal, paired with `addEventListener`. */
  removeEventListener?(type: string, listener: EventListener): void;
  /** Node listener registration, used when `addEventListener` is absent. */
  on?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
  /** Node listener removal, paired with `on`. */
  off?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  onProgress?: ProgressCallback;
}

function cloneForWorker(
  value: unknown,
  copy: boolean,
  transfers: Transferable[],
  transferred = new Set<ArrayBuffer>(),
  path = 'request',
): unknown {
  if (typeof value === 'function') {
    throw refuseType(`${path} is a function and cannot be sent to a Worker`);
  }
  if (value instanceof Float32Array) {
    const samples = copy ? value.slice() : value;
    const buffer = samples.buffer;
    if (!(buffer instanceof ArrayBuffer)) {
      throw refuseType(
        'OfflineWorkerClient only transfers Float32Array values backed by ArrayBuffer',
      );
    }
    if (!transferred.has(buffer)) {
      transferred.add(buffer);
      transfers.push(buffer);
    }
    return samples;
  }
  if (Array.isArray(value)) {
    return value.map((item, index) =>
      cloneForWorker(item, copy, transfers, transferred, `${path}[${index}]`),
    );
  }
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value).map(([key, item]) => [
        key,
        cloneForWorker(
          item,
          copy,
          transfers,
          transferred,
          path === 'request' ? key : `${path}.${key}`,
        ),
      ]),
    );
  }
  return value;
}

function cancellationFlag(): Int32Array | undefined {
  if (typeof SharedArrayBuffer === 'undefined') {
    return undefined;
  }
  return new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT));
}

/**
 * Owns a dedicated Worker for value-based offline analysis and mastering.
 *
 * Worker calls take an `AbortSignal` because the call is asynchronous and an
 * abort can be delivered while it runs; in-thread calls keep the synchronous
 * `cancel` predicate because a synchronous call blocks the thread, so an abort
 * could not be delivered until it returned. Cancellation reaches native code at
 * its next progress boundary; in browsers prompt cancellation of synchronous
 * WASM needs cross-origin isolation for the `SharedArrayBuffer` flag.
 *
 * `Project`, `Mixer`, and realtime APIs are intentionally absent: their native
 * handles are local to one JavaScript realm and cannot be transferred safely.
 */
@entryClass('OfflineWorkerClient', ['rejectAll', 'call'])
export class OfflineWorkerClient {
  private readonly worker: OfflineWorker;
  private readonly ownsWorker: boolean;
  private readonly pending = new Map<number, PendingCall>();
  private nextId = 1;
  private closed = false;
  private destroying: Promise<void> | undefined;
  private usesEventTarget = false;

  /**
   * @param options Worker source and ownership.
   * @throws SonareError `NotSupported` when no Worker implementation is available and neither `worker` nor `workerFactory` is given.
   */
  constructor(options: OfflineWorkerClientOptions = {}) {
    this.ownsWorker = options.worker === undefined || options.terminateWorkerOnDestroy === true;
    if (options.worker) {
      this.worker = options.worker;
    } else {
      if (!options.workerFactory && typeof Worker === 'undefined') {
        throw codedError(
          ErrorCode.NotSupported,
          'OfflineWorkerClient requires a Worker implementation',
        );
      }
      const url =
        options.workerUrl === undefined
          ? new URL('./worker.js', import.meta.url)
          : new URL(/* webpackIgnore: true */ options.workerUrl, import.meta.url);
      this.worker =
        options.workerFactory?.(url) ?? new Worker(url, { type: 'module', name: 'sonare-offline' });
    }
    if (this.worker.addEventListener) {
      this.usesEventTarget = true;
      this.worker.addEventListener('message', this.onMessage as EventListener);
      this.worker.addEventListener('error', this.onError as EventListener);
    } else if (this.worker.on) {
      this.worker.on('message', this.onNodeMessage);
      this.worker.on('error', this.onNodeError);
    } else {
      throw refuseType('OfflineWorkerClient requires Worker event listeners');
    }
  }

  /**
   * Run full music analysis in the Worker.
   *
   * @param request Analysis request; `onProgress` receives `(progress, stage)` on the calling thread.
   * @param options Copy and cancellation behaviour.
   * @returns The analysis result.
   * @throws SonareError `Cancelled` on abort, `InvalidState` after {@link destroy}; the call rejects with `TypeError` for a function-valued request field such as `cancel`.
   */
  analyze(
    request: WorkerRequest<AnalyzeRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<ReturnType<typeof import('./quick_analysis.js').analyze>> {
    return this.call('analyze', request, options);
  }

  /** Detect BPM in the Worker. Same request, option and error contract as {@link analyze}. */
  detectBpm(
    request: WorkerRequest<DetectBpmRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<number | undefined> {
    return this.call('detectBpm', request, options);
  }

  /** Detect the key in the Worker. Same request, option and error contract as {@link analyze}. */
  detectKey(
    request: WorkerRequest<DetectKeyRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<ReturnType<typeof import('./quick_analysis.js').detectKey>> {
    return this.call('detectKey', request, options);
  }

  /** Detect chords in the Worker. Same request, option and error contract as {@link analyze}. */
  detectChords(
    request: WorkerRequest<DetectChordsRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<ReturnType<typeof import('./quick_analysis.js').detectChords>> {
    return this.call('detectChords', request, options);
  }

  /** Run mono preset mastering in the Worker. Same request, option and error contract as {@link analyze}. */
  masterAudio(
    request: WorkerRequest<MasterAudioRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<ReturnType<typeof import('./mastering_chain.js').masterAudio>> {
    return this.call('masterAudio', request, options);
  }

  /** Run stereo preset mastering in the Worker. Same request, option and error contract as {@link analyze}. */
  masterAudioStereo(
    request: WorkerRequest<MasterAudioStereoRequest>,
    options?: OfflineWorkerCallOptions,
  ): Promise<ReturnType<typeof import('./mastering_chain.js').masterAudioStereo>> {
    return this.call('masterAudioStereo', request, options);
  }

  /**
   * Stop accepting calls, reject outstanding work with `InvalidState`, and
   * release the Worker if owned, resolving once the Worker has terminated.
   * Calling it again returns the same completion.
   */
  destroy(): Promise<void> {
    if (this.destroying) {
      return this.destroying;
    }
    this.closed = true;
    if (this.usesEventTarget) {
      this.worker.removeEventListener?.('message', this.onMessage as EventListener);
      this.worker.removeEventListener?.('error', this.onError as EventListener);
    } else {
      this.worker.off?.('message', this.onNodeMessage);
      this.worker.off?.('error', this.onNodeError);
    }
    this.rejectAll(() => destroyedError());
    this.destroying = this.ownsWorker
      ? Promise.resolve(this.worker.terminate()).then(() => undefined)
      : Promise.resolve();
    return this.destroying;
  }

  /** Same as {@link destroy}, for `await using`. */
  [Symbol.asyncDispose](): Promise<void> {
    return this.destroy();
  }

  private rejectAll(makeError: () => Error): void {
    const calls = [...this.pending.values()];
    this.pending.clear();
    for (const { reject } of calls) {
      reject(makeError());
    }
  }

  private call<T>(
    operation: OfflineWorkerOperation,
    request: WorkerRequest<object>,
    given: OfflineWorkerCallOptions | null = {},
  ): Promise<T> {
    if (this.closed) {
      return Promise.reject(destroyedError());
    }
    const options: OfflineWorkerCallOptions = given ?? {};
    try {
      if (typeof options === 'object' && Object.keys(options as object).includes('onProgress')) {
        throw refuseType(
          'unknown key options.onProgress (expected copy, signal); pass onProgress in the request',
        );
      }
      checkWorkerCallOptions(options, ['copy', 'signal']);
    } catch (error) {
      return Promise.reject(error);
    }
    const signal = options.signal;
    if (signal?.aborted) {
      return Promise.reject(cancelledError(signal));
    }
    const { onProgress, ...posted } = request as { onProgress?: ProgressCallback };
    let preparedRequest: Record<string, unknown>;
    const transfers: Transferable[] = [];
    try {
      if (onProgress !== undefined && typeof onProgress !== 'function') {
        throw refuseType('onProgress must be a function');
      }
      preparedRequest = cloneForWorker(posted, options.copy === true, transfers) as Record<
        string,
        unknown
      >;
    } catch (error) {
      return Promise.reject(error);
    }
    const id = this.nextId++;
    const cancelFlag = cancellationFlag();
    return new Promise<T>((resolve, reject) => {
      const onAbort = (): void => {
        if (!this.pending.delete(id)) {
          return;
        }
        if (cancelFlag) {
          Atomics.store(cancelFlag, 0, 1);
        }
        // A message also covers a call cancelled before it begins and is the
        // best available fallback where SharedArrayBuffer is unavailable.
        this.worker.postMessage({ type: 'sonare:offline-cancel', id });
        reject(cancelledError(signal as AbortSignal));
      };
      signal?.addEventListener('abort', onAbort, { once: true });
      const settle = (): void => signal?.removeEventListener('abort', onAbort);
      this.pending.set(id, {
        resolve: (value) => {
          settle();
          resolve(value as T);
        },
        reject: (reason) => {
          settle();
          reject(reason);
        },
        onProgress,
      });
      try {
        this.worker.postMessage(
          {
            type: 'sonare:offline-run',
            id,
            operation,
            request: preparedRequest,
            ...(cancelFlag ? { cancelBuffer: cancelFlag.buffer as SharedArrayBuffer } : {}),
          },
          transfers,
        );
      } catch (error) {
        // The entry is inserted before dispatch so a reentrant response can
        // settle the call first; only a still-pending entry is rolled back.
        const entry = this.pending.get(id);
        if (entry) {
          this.pending.delete(id);
          entry.reject(error);
        }
      }
    });
  }

  private readonly onMessage = (event: MessageEvent<OfflineWorkerResponseMessage>): void => {
    const message = event.data;
    if (!isResponseMessage(message)) {
      this.rejectAll(() =>
        codedError(ErrorCode.InvalidFormat, 'Offline Worker sent a malformed message'),
      );
      return;
    }
    const pending = this.pending.get(message.id);
    if (!pending) {
      return;
    }
    if (message.type === 'sonare:offline-progress') {
      pending.onProgress?.(message.progress, message.stage);
      return;
    }
    this.pending.delete(message.id);
    if (message.type === 'sonare:offline-result') {
      pending.resolve(message.result);
      return;
    }
    pending.reject(
      isFailurePayload(message.error)
        ? failureFromPayload(message.error)
        : codedError(ErrorCode.InvalidFormat, 'Offline Worker sent a malformed error'),
    );
  };

  private readonly onError = (event: ErrorEvent): void => {
    // A crash has no finer class than Unknown; the original event is the cause.
    this.rejectAll(() =>
      codedError(ErrorCode.Unknown, event.message || 'Offline Worker failed', {
        cause: event,
      }),
    );
  };

  private readonly onNodeMessage = (data: unknown): void => {
    this.onMessage({ data } as MessageEvent<OfflineWorkerResponseMessage>);
  };

  private readonly onNodeError = (error: unknown): void => {
    this.onError({ message: errorText(error), error } as ErrorEvent);
  };
}

function destroyedError(): SonareError {
  return releasedHandleError('OfflineWorkerClient');
}

function cancelledError(signal: AbortSignal): SonareError {
  return codedError(ErrorCode.Cancelled, 'Operation cancelled', { cause: signal.reason });
}

function isResponseMessage(value: unknown): value is OfflineWorkerResponseMessage {
  if (typeof value !== 'object' || value === null) {
    return false;
  }
  const m = value as Record<string, unknown>;
  return (
    typeof m.id === 'number' &&
    (m.type === 'sonare:offline-progress'
      ? typeof m.progress === 'number' && typeof m.stage === 'string'
      : m.type === 'sonare:offline-result' || m.type === 'sonare:offline-error')
  );
}
