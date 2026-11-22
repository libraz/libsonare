/**
 * Offline one-shot operations hosted inside a dedicated Web Worker.
 *
 * Only value-based calls are exposed here: analysis and preset mastering. Do
 * not add `Project`, `Mixer`, or realtime handles. Their native lifetime is
 * bound to one JavaScript realm, so moving them across a Worker boundary would
 * make ownership and `destroy()` semantics unsound.
 */

import { codedError, withEntry } from './internal/entry.js';
import { failurePayload } from './internal/failure_payload.js';
import './lifetime.js';
import { ErrorCode, type SonareError } from './errors.js';
import { init } from './index.js';
import type { MasterAudioRequest, MasterAudioStereoRequest } from './mastering_chain.js';
import { masterAudio, masterAudioStereo } from './mastering_chain.js';
import type {
  AnalyzeRequest,
  DetectBpmRequest,
  DetectChordsRequest,
  DetectKeyRequest,
} from './quick_analysis.js';
import { analyze, detectBpm, detectChords, detectKey } from './quick_analysis.js';
import type {
  OfflineWorkerRequestMessage,
  OfflineWorkerResponseMessage,
  OfflineWorkerRunMessage,
} from './worker_protocol.js';

export type { OfflineWorkerOperation } from './worker_protocol.js';

/** Minimal endpoint shared by browser Workers and the Node worker-thread test bridge. */
export interface OfflineWorkerEndpoint {
  /** Sends a response message to the client. */
  postMessage(message: OfflineWorkerResponseMessage, transfer?: Transferable[]): void;
  /** Registers the handler for the client's request messages. */
  addEventListener(
    type: 'message',
    listener: (event: MessageEvent<OfflineWorkerRequestMessage>) => void,
  ): void;
}

function transferableBuffers(value: unknown, seen = new Set<object>()): Transferable[] {
  const buffers: Transferable[] = [];
  const seenBuffers = new Set<ArrayBuffer>();
  const visit = (item: unknown): void => {
    if (item === null || item === undefined || typeof item !== 'object') {
      return;
    }
    if (ArrayBuffer.isView(item)) {
      const buffer = item.buffer;
      if (buffer instanceof ArrayBuffer && !seenBuffers.has(buffer)) {
        seenBuffers.add(buffer);
        buffers.push(buffer);
      }
      return;
    }
    if (item instanceof ArrayBuffer) {
      if (!seenBuffers.has(item)) {
        seenBuffers.add(item);
        buffers.push(item);
      }
      return;
    }
    if (seen.has(item)) {
      return;
    }
    seen.add(item);
    if (Array.isArray(item)) {
      item.forEach(visit);
      return;
    }
    Object.values(item).forEach(visit);
  };
  visit(value);
  return buffers;
}

function cancelledError(): SonareError {
  return codedError(ErrorCode.Cancelled, 'Operation cancelled');
}

/**
 * Install the protocol on an endpoint. This export is intentionally useful to
 * the Node `worker_threads` test adapter; browsers install it automatically
 * when this module is loaded as a Worker entry point.
 */
export function installOfflineWorkerEndpoint(endpoint: OfflineWorkerEndpoint): void {
  withEntry('installOfflineWorkerEndpoint', () => {
    const cancelled = new Set<number>();
    // IDs with a run() in flight, from message dispatch to its finally block.
    // A cancel naming an ID outside this set has nothing left to cancel -- the
    // run already settled, or never started -- and must not be recorded, or it
    // leaks a Set entry no later message ever clears.
    const inFlight = new Set<number>();

    const run = async (message: OfflineWorkerRunMessage): Promise<void> => {
      // Synchronous: runs before this async function's first `await`, so it is
      // visible to the listener below before any other message can be handled.
      inFlight.add(message.id);
      const cancelFlag = message.cancelBuffer ? new Int32Array(message.cancelBuffer) : undefined;
      const isCancelled = (): boolean =>
        cancelled.has(message.id) ||
        (cancelFlag !== undefined && Atomics.load(cancelFlag, 0) !== 0);
      const onProgress = (progress: number, stage: string): undefined | false => {
        endpoint.postMessage({ type: 'sonare:offline-progress', id: message.id, progress, stage });
        return isCancelled() ? false : undefined;
      };
      try {
        await init();
        if (isCancelled()) {
          throw cancelledError();
        }

        // `cancel` is a separate channel from `onProgress`: the native call polls
        // it and never reads what the progress callback returns, so passing only
        // onProgress left a running operation no way to learn it was cancelled.
        let result: unknown;
        switch (message.operation) {
          case 'analyze':
            result = analyze({
              ...(message.request as unknown as AnalyzeRequest),
              onProgress,
              cancel: isCancelled,
            });
            break;
          case 'detectBpm':
            result = detectBpm(message.request as unknown as DetectBpmRequest);
            break;
          case 'detectKey':
            result = detectKey(message.request as unknown as DetectKeyRequest);
            break;
          case 'detectChords':
            result = detectChords(message.request as unknown as DetectChordsRequest);
            break;
          case 'masterAudio':
            result = masterAudio({
              ...(message.request as unknown as MasterAudioRequest),
              onProgress,
              cancel: isCancelled,
            });
            break;
          case 'masterAudioStereo':
            result = masterAudioStereo({
              ...(message.request as unknown as MasterAudioStereoRequest),
              onProgress,
              cancel: isCancelled,
            });
            break;
        }
        if (isCancelled()) {
          throw cancelledError();
        }
        endpoint.postMessage(
          { type: 'sonare:offline-result', id: message.id, result },
          transferableBuffers(result),
        );
      } catch (error) {
        endpoint.postMessage({
          type: 'sonare:offline-error',
          id: message.id,
          error: failurePayload(error),
        });
      } finally {
        cancelled.delete(message.id);
        inFlight.delete(message.id);
      }
    };

    endpoint.addEventListener('message', (event) => {
      const message = event.data;
      if (message.type === 'sonare:offline-cancel') {
        if (inFlight.has(message.id)) {
          cancelled.add(message.id);
        }
        return;
      }
      void run(message);
    });
  });
}

function browserWorkerEndpoint(): OfflineWorkerEndpoint | null {
  const scope = globalThis as unknown as {
    document?: unknown;
    postMessage?: (message: OfflineWorkerResponseMessage, transfer?: Transferable[]) => void;
    addEventListener?: (
      type: 'message',
      listener: (event: MessageEvent<OfflineWorkerRequestMessage>) => void,
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

/** The part of a Node `worker_threads` parent port the endpoint uses. */
interface NodeParentPort {
  postMessage(message: unknown, transfer?: Transferable[]): void;
  on(type: 'message', listener: (data: OfflineWorkerRequestMessage) => void): void;
}

/**
 * The parent port when this module runs as a Node `worker_threads` Worker, which
 * has no global `postMessage` / `addEventListener`. Reached through
 * `process.getBuiltinModule` so no bundler or dev server sees a module specifier.
 */
function nodeWorkerEndpoint(): OfflineWorkerEndpoint | null {
  const scope = globalThis as unknown as {
    process?: { getBuiltinModule?: (id: string) => unknown };
  };
  const workerThreads = scope.process?.getBuiltinModule?.('worker_threads') as
    | { parentPort: NodeParentPort | null }
    | undefined;
  const parentPort = workerThreads?.parentPort;
  if (!parentPort) {
    return null;
  }
  return {
    postMessage: (message, transfer) => parentPort.postMessage(message, transfer),
    // The port may be shared with whoever started the thread, so only this protocol is answered.
    addEventListener: (_type, listener) =>
      parentPort.on('message', (data: OfflineWorkerRequestMessage) => {
        if (typeof data?.type === 'string' && data.type.startsWith('sonare:offline-')) {
          listener({ data } as MessageEvent<OfflineWorkerRequestMessage>);
        }
      }),
  };
}

const endpoint = browserWorkerEndpoint();
if (endpoint) {
  installOfflineWorkerEndpoint(endpoint);
} else {
  const node = nodeWorkerEndpoint();
  if (node) {
    installOfflineWorkerEndpoint(node);
  }
}
