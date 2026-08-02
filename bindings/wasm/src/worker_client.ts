import { ErrorCode, SonareError } from './errors';
import type { MasterAudioRequest, MasterAudioStereoRequest } from './mastering_chain';
import type {
  DetectChordsRequest,
  DetectKeyRequest,
  MusicAnalyzeRequest,
  SamplesRequest,
} from './quick_analysis';
import type { OfflineWorkerOperation, OfflineWorkerResponseMessage } from './worker_protocol';

type WorkerRequest<T> = Omit<T, 'onProgress'>;

/** Progress relayed from the Worker at the core's native progress boundaries. */
export interface OfflineWorkerProgress {
  progress: number;
  stage: string;
}

/** Per-call behaviour for a one-shot operation. */
export interface OfflineWorkerCallOptions {
  /**
   * Preserve the caller's input arrays by copying them before dispatch.
   *
   * The default is `false`: `Float32Array` buffers are transferred to the
   * Worker and become detached on the calling thread, avoiding a second copy.
   */
  copy?: boolean;
  /** Receives native progress boundaries for analysis and mastering calls. */
  onProgress?: (update: OfflineWorkerProgress) => void;
}

/** Options used to create an {@link OfflineWorkerClient}. */
export interface OfflineWorkerClientOptions {
  /** Reuse a Worker owned by the host instead of creating one. */
  worker?: OfflineWorker;
  /** URL of the Worker entry; defaults to the adjacent published `worker.js`. */
  workerUrl?: string | URL;
  /** Test/host hook for creating a dedicated Worker. */
  workerFactory?: (url: URL) => OfflineWorker;
  /** Terminate a supplied Worker when {@link OfflineWorkerClient.dispose} runs. */
  terminateWorkerOnDispose?: boolean;
}

/** Browser Worker or Node `worker_threads.Worker` used by the client. */
export interface OfflineWorker {
  postMessage(message: unknown, transfer?: Transferable[]): void;
  terminate(): unknown;
  addEventListener?(type: string, listener: EventListener): void;
  removeEventListener?(type: string, listener: EventListener): void;
  on?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
  off?(type: 'message' | 'error', listener: (...args: unknown[]) => void): unknown;
}

/**
 * A cancellable promise returned by an {@link OfflineWorkerClient} call.
 *
 * It is Promise-like, so `await client.analyze(...)` works. Call `cancel()` to
 * request cancellation at the next native progress boundary. In browsers this
 * uses a `SharedArrayBuffer` flag, which requires cross-origin isolation for
 * prompt cancellation while synchronous WASM is running. The worker forwards
 * that flag through the same native cancellation callback used by synchronous
 * one-shot requests.
 */
export class OfflineWorkerTask<T> implements PromiseLike<T> {
  constructor(
    readonly result: Promise<T>,
    private readonly cancelRequest: () => void,
  ) {}

  cancel(): void {
    this.cancelRequest();
  }

  // biome-ignore lint/suspicious/noThenProperty: this intentionally implements PromiseLike so callers can await a task and cancel it.
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

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  onProgress?: (update: OfflineWorkerProgress) => void;
  cancelFlag?: Int32Array;
}

function cloneForWorker(
  value: unknown,
  copy: boolean,
  transfers: Transferable[],
  transferred = new Set<ArrayBuffer>(),
): unknown {
  if (value instanceof Float32Array) {
    const samples = copy ? value.slice() : value;
    const buffer = samples.buffer;
    if (!(buffer instanceof ArrayBuffer)) {
      throw new TypeError(
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

function cancellationFlag(): Int32Array | undefined {
  if (typeof SharedArrayBuffer === 'undefined') {
    return undefined;
  }
  return new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT));
}

function workerError(
  message: Extract<OfflineWorkerResponseMessage, { type: 'sonare:offline-error' }>,
): Error {
  if (message.error.code !== undefined) {
    return new SonareError(
      message.error.code,
      message.error.codeName ?? 'Unknown',
      message.error.message,
    );
  }
  const error = new Error(message.error.message);
  error.name = message.error.name;
  return error;
}

/**
 * Owns a dedicated Worker for value-based offline analysis and mastering.
 *
 * `Project`, `Mixer`, and realtime APIs are intentionally absent: their native
 * handles are local to one JavaScript realm and cannot be transferred safely.
 */
export class OfflineWorkerClient {
  private readonly worker: OfflineWorker;
  private readonly ownsWorker: boolean;
  private readonly pending = new Map<number, PendingCall>();
  private nextId = 1;
  private closed = false;
  private usesEventTarget = false;

  constructor(options: OfflineWorkerClientOptions = {}) {
    this.ownsWorker = options.worker === undefined || options.terminateWorkerOnDispose === true;
    if (options.worker) {
      this.worker = options.worker;
    } else {
      if (!options.workerFactory && typeof Worker === 'undefined') {
        throw new Error('OfflineWorkerClient requires a browser Worker implementation');
      }
      const url =
        options.workerUrl === undefined
          ? new URL('./worker.js', import.meta.url)
          : new URL(options.workerUrl, import.meta.url);
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
      throw new TypeError('OfflineWorkerClient requires Worker event listeners');
    }
  }

  /** Dispatch full music analysis to the Worker. */
  analyze(
    request: WorkerRequest<MusicAnalyzeRequest>,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<ReturnType<typeof import('./quick_analysis').analyze>> {
    return this.call('analyze', request, options);
  }

  /** Dispatch BPM detection to the Worker. */
  detectBpm(
    request: SamplesRequest,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<number> {
    return this.call('detectBpm', request, options);
  }

  /** Dispatch key detection to the Worker. */
  detectKey(
    request: DetectKeyRequest,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<ReturnType<typeof import('./quick_analysis').detectKey>> {
    return this.call('detectKey', request, options);
  }

  /** Dispatch chord detection to the Worker. */
  detectChords(
    request: DetectChordsRequest,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<ReturnType<typeof import('./quick_analysis').detectChords>> {
    return this.call('detectChords', request, options);
  }

  /** Dispatch mono preset mastering to the Worker. */
  masterAudio(
    request: WorkerRequest<MasterAudioRequest>,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<ReturnType<typeof import('./mastering_chain').masterAudio>> {
    return this.call('masterAudio', request, options);
  }

  /** Dispatch stereo preset mastering to the Worker. */
  masterAudioStereo(
    request: WorkerRequest<MasterAudioStereoRequest>,
    options?: OfflineWorkerCallOptions,
  ): OfflineWorkerTask<ReturnType<typeof import('./mastering_chain').masterAudioStereo>> {
    return this.call('masterAudioStereo', request, options);
  }

  /** Stop accepting calls, reject outstanding work, and release the Worker if owned. */
  dispose(): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    if (this.usesEventTarget) {
      this.worker.removeEventListener?.('message', this.onMessage as EventListener);
      this.worker.removeEventListener?.('error', this.onError as EventListener);
    } else {
      this.worker.off?.('message', this.onNodeMessage);
      this.worker.off?.('error', this.onNodeError);
    }
    for (const { reject } of this.pending.values()) {
      reject(new Error('OfflineWorkerClient was disposed'));
    }
    this.pending.clear();
    if (this.ownsWorker) {
      this.worker.terminate();
    }
  }

  private call<T>(
    operation: OfflineWorkerOperation,
    request: object,
    options: OfflineWorkerCallOptions = {},
  ): OfflineWorkerTask<T> {
    if (this.closed) {
      throw new Error('OfflineWorkerClient was disposed');
    }
    const id = this.nextId++;
    const transfers: Transferable[] = [];
    const preparedRequest = cloneForWorker(request, options.copy === true, transfers) as Record<
      string,
      unknown
    >;
    const cancelFlag = cancellationFlag();
    const result = new Promise<T>((resolve, reject) => {
      this.pending.set(id, {
        resolve: (value) => resolve(value as T),
        reject,
        onProgress: options.onProgress,
        cancelFlag,
      });
    });
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
    return new OfflineWorkerTask(result, () => {
      if (!this.pending.has(id)) {
        return;
      }
      if (cancelFlag) {
        Atomics.store(cancelFlag, 0, 1);
      }
      // A message also covers a call cancelled before it begins and is the
      // best available fallback where SharedArrayBuffer is unavailable.
      this.worker.postMessage({ type: 'sonare:offline-cancel', id });
    });
  }

  private readonly onMessage = (event: MessageEvent<OfflineWorkerResponseMessage>): void => {
    const message = event.data;
    const pending = this.pending.get(message.id);
    if (!pending) {
      return;
    }
    if (message.type === 'sonare:offline-progress') {
      pending.onProgress?.({ progress: message.progress, stage: message.stage });
      return;
    }
    this.pending.delete(message.id);
    if (message.type === 'sonare:offline-result') {
      pending.resolve(message.result);
      return;
    }
    pending.reject(workerError(message));
  };

  private readonly onError = (event: ErrorEvent): void => {
    const error = new Error(event.message || 'Offline Worker failed');
    for (const { reject } of this.pending.values()) {
      reject(error);
    }
    this.pending.clear();
  };

  private readonly onNodeMessage = (data: unknown): void => {
    this.onMessage({ data } as MessageEvent<OfflineWorkerResponseMessage>);
  };

  private readonly onNodeError = (error: unknown): void => {
    this.onError(
      error instanceof Error
        ? ({ message: error.message } as ErrorEvent)
        : ({ message: String(error) } as ErrorEvent),
    );
  };
}

export { ErrorCode };
