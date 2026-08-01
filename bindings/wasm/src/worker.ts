/**
 * Offline one-shot operations hosted inside a dedicated Web Worker.
 *
 * Only value-based calls are exposed here: analysis and preset mastering. Do
 * not add `Project`, `Mixer`, or realtime handles. Their native lifetime is
 * bound to one JavaScript realm, so moving them across a Worker boundary would
 * make ownership and `delete()` semantics unsound.
 */

import { ErrorCode, SonareError } from './errors';
import { init } from './index';
import type { MasterAudioRequest, MasterAudioStereoRequest } from './mastering_chain';
import { masterAudio, masterAudioStereo } from './mastering_chain';
import type {
  DetectChordsRequest,
  DetectKeyRequest,
  MusicAnalyzeRequest,
  SamplesRequest,
} from './quick_analysis';
import { analyzeWithProgress, detectBpm, detectChords, detectKey } from './quick_analysis';
import type {
  OfflineWorkerErrorMessage,
  OfflineWorkerRequestMessage,
  OfflineWorkerResponseMessage,
  OfflineWorkerRunMessage,
} from './worker_protocol';

export type { OfflineWorkerOperation } from './worker_protocol';

/** Minimal endpoint shared by browser Workers and the Node worker-thread test bridge. */
export interface OfflineWorkerEndpoint {
  postMessage(message: OfflineWorkerResponseMessage, transfer?: Transferable[]): void;
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

function errorMessage(error: unknown): OfflineWorkerErrorMessage['error'] {
  if (error instanceof SonareError) {
    return {
      name: error.name,
      message: error.message,
      code: error.code,
      codeName: error.codeName,
    };
  }
  if (error instanceof Error) {
    return { name: error.name, message: error.message };
  }
  return { name: 'Error', message: String(error) };
}

function cancelledError(): SonareError {
  return new SonareError(ErrorCode.Cancelled, 'Cancelled', 'Operation cancelled');
}

/**
 * Install the protocol on an endpoint. This export is intentionally useful to
 * the Node `worker_threads` test adapter; browsers install it automatically
 * when this module is loaded as a Worker entry point.
 */
export function installOfflineWorkerEndpoint(endpoint: OfflineWorkerEndpoint): void {
  const cancelled = new Set<number>();

  const run = async (message: OfflineWorkerRunMessage): Promise<void> => {
    const cancelFlag = message.cancelBuffer ? new Int32Array(message.cancelBuffer) : undefined;
    const isCancelled = (): boolean =>
      cancelled.has(message.id) || (cancelFlag !== undefined && Atomics.load(cancelFlag, 0) !== 0);
    const onProgress = (progress: number, stage: string): undefined | false => {
      endpoint.postMessage({ type: 'sonare:offline-progress', id: message.id, progress, stage });
      return isCancelled() ? false : undefined;
    };

    try {
      await init();
      if (isCancelled()) {
        throw cancelledError();
      }

      let result: unknown;
      switch (message.operation) {
        case 'analyze':
          result = analyzeWithProgress({
            ...(message.request as unknown as MusicAnalyzeRequest),
            onProgress,
          });
          break;
        case 'detectBpm':
          result = detectBpm(message.request as unknown as SamplesRequest);
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
          });
          break;
        case 'masterAudioStereo':
          result = masterAudioStereo({
            ...(message.request as unknown as MasterAudioStereoRequest),
            onProgress,
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
        error: errorMessage(error),
      });
    } finally {
      cancelled.delete(message.id);
    }
  };

  endpoint.addEventListener('message', (event) => {
    const message = event.data;
    if (message.type === 'sonare:offline-cancel') {
      cancelled.add(message.id);
      return;
    }
    void run(message);
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

const endpoint = browserWorkerEndpoint();
if (endpoint) {
  installOfflineWorkerEndpoint(endpoint);
}
