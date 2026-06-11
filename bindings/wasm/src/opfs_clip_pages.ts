import type { ClipPageProvider, ClipPageRequest, RealtimeEngine } from './realtime_engine';

export interface OpfsClipPageProviderOptions {
  path: string;
  numChannels: number;
  numSamples: number;
  pageFrames: number;
  dataOffsetBytes?: number;
  worker?: Worker;
  terminateWorkerOnClose?: boolean;
}

export interface OpfsClipPageProviderBinding {
  provider: ClipPageProvider;
  supplyPage(pageIndex: number): Promise<boolean>;
  supplyRequest(request: ClipPageRequest): Promise<boolean>;
  close(): void;
}

interface PageResponse {
  type: 'sonare:clip-page';
  requestId: number;
  pageIndex: number;
  ok: boolean;
  frames?: number;
  channels?: Float32Array[];
  channelBuffers?: ArrayBufferLike[];
  error?: string;
}

export const opfsClipPageWorkerSource = `
const sonareClipPageReadQueues = new Map();

function sonareEnqueueClipPageRead(key, task) {
  const previous = sonareClipPageReadQueues.get(key) || Promise.resolve();
  const next = previous.catch(() => undefined).then(task);
  const queued = next.finally(() => {
    if (sonareClipPageReadQueues.get(key) === queued) {
      sonareClipPageReadQueues.delete(key);
    }
  });
  sonareClipPageReadQueues.set(key, queued);
  return next;
}

self.onmessage = async (event) => {
  const message = event.data;
  if (!message || message.type !== 'sonare:read-clip-page') return;
  const { requestId, path, pageIndex, numChannels, numSamples, pageFrames, dataOffsetBytes = 0 } = message;
  await sonareEnqueueClipPageRead(String(path), async () => {
  try {
    if (pageIndex < 0) {
      self.postMessage({ type: 'sonare:clip-page', requestId, pageIndex, ok: false });
      return;
    }
    const startFrame = pageIndex * pageFrames;
    if (startFrame >= numSamples) {
      self.postMessage({ type: 'sonare:clip-page', requestId, pageIndex, ok: false });
      return;
    }
    const root = await self.navigator.storage.getDirectory();
    let dir = root;
    const parts = String(path).split('/').filter(Boolean);
    for (let i = 0; i < parts.length - 1; ++i) {
      dir = await dir.getDirectoryHandle(parts[i]);
    }
    const fileHandle = await dir.getFileHandle(parts[parts.length - 1]);
    const access = await fileHandle.createSyncAccessHandle();
    try {
      const frames = Math.min(pageFrames, numSamples - startFrame);
      const frameBytes = numChannels * 4;
      const bytes = new Uint8Array(frames * frameBytes);
      let bytesReadTotal = 0;
      const readOffset = dataOffsetBytes + startFrame * frameBytes;
      while (bytesReadTotal < bytes.byteLength) {
        const bytesRead = access.read(bytes.subarray(bytesReadTotal), {
          at: readOffset + bytesReadTotal,
        });
        if (bytesRead <= 0) {
          break;
        }
        bytesReadTotal += bytesRead;
      }
      if (bytesReadTotal !== bytes.byteLength || bytesReadTotal % frameBytes !== 0) {
        self.postMessage({ type: 'sonare:clip-page', requestId, pageIndex, ok: false });
        return;
      }
      const framesRead = bytesReadTotal / frameBytes;
      const view = new DataView(bytes.buffer, 0, framesRead * frameBytes);
      const channelBuffers = Array.from({ length: numChannels }, () => new ArrayBuffer(framesRead * 4));
      for (let ch = 0; ch < numChannels; ++ch) {
        const channel = new Float32Array(channelBuffers[ch]);
        for (let frame = 0; frame < framesRead; ++frame) {
          channel[frame] = view.getFloat32((frame * numChannels + ch) * 4, true);
        }
      }
      self.postMessage(
        { type: 'sonare:clip-page', requestId, pageIndex, ok: true, frames: framesRead, channelBuffers },
        channelBuffers,
      );
    } finally {
      access.close();
    }
  } catch (error) {
    self.postMessage({
      type: 'sonare:clip-page',
      requestId,
      pageIndex,
      ok: false,
      error: error instanceof Error ? error.message : String(error),
    });
  }
  });
};
`;

export function createOpfsClipPageWorker(): Worker {
  const blob = new Blob([opfsClipPageWorkerSource], { type: 'text/javascript' });
  return new Worker(URL.createObjectURL(blob));
}

export function createOpfsClipPageProvider(
  engine: RealtimeEngine,
  options: OpfsClipPageProviderOptions,
): OpfsClipPageProviderBinding {
  if (options.numChannels <= 0 || options.numSamples <= 0 || options.pageFrames <= 0) {
    throw new Error('numChannels, numSamples, and pageFrames must be positive');
  }
  const provider = engine.createClipPageProvider(
    options.numChannels,
    options.numSamples,
    options.pageFrames,
  );
  const worker = options.worker ?? createOpfsClipPageWorker();
  const ownsWorker = options.worker === undefined || options.terminateWorkerOnClose === true;
  let nextRequestId = 1;
  let closed = false;
  let readQueue: Promise<void> = Promise.resolve();
  const pending = new Map<
    number,
    { resolve: (value: boolean) => void; reject: (reason: unknown) => void }
  >();

  const onMessage = (event: MessageEvent<PageResponse>) => {
    const response = event.data;
    if (response?.type !== 'sonare:clip-page') {
      return;
    }
    const entry = pending.get(response.requestId);
    if (!entry) {
      return;
    }
    pending.delete(response.requestId);
    if (!response.ok) {
      entry.resolve(false);
      return;
    }
    const channels =
      response.channels ??
      response.channelBuffers?.map(
        (buffer) => new Float32Array(buffer, 0, response.frames ?? buffer.byteLength / 4),
      );
    if (!channels || channels.length === 0) {
      entry.resolve(false);
      return;
    }
    try {
      provider.supply(response.pageIndex, channels);
    } catch {
      entry.resolve(false);
      return;
    }
    entry.resolve(true);
  };
  worker.addEventListener('message', onMessage as EventListener);

  const supplyPage = (pageIndex: number): Promise<boolean> => {
    if (closed) {
      return Promise.reject(new Error('OpfsClipPageProvider is closed'));
    }
    const requestId = nextRequestId++;
    const promise = new Promise<boolean>((resolve, reject) => {
      pending.set(requestId, { resolve, reject });
    });
    readQueue = readQueue
      .catch(() => undefined)
      .then(() => {
        if (closed) {
          const entry = pending.get(requestId);
          pending.delete(requestId);
          entry?.reject(new Error('OpfsClipPageProvider is closed'));
          return;
        }
        worker.postMessage({
          type: 'sonare:read-clip-page',
          requestId,
          path: options.path,
          pageIndex,
          numChannels: options.numChannels,
          numSamples: options.numSamples,
          pageFrames: options.pageFrames,
          dataOffsetBytes: options.dataOffsetBytes ?? 0,
        });
        return promise.then(
          () => undefined,
          () => undefined,
        );
      });
    readQueue.catch(() => {
      // The per-request promise carries the user-visible failure.
    });
    return promise;
  };

  return {
    provider,
    supplyPage,
    supplyRequest(request: ClipPageRequest) {
      return supplyPage(Math.floor(request.sample / options.pageFrames));
    },
    close() {
      if (closed) {
        return;
      }
      closed = true;
      worker.removeEventListener('message', onMessage as EventListener);
      for (const entry of pending.values()) {
        entry.reject(new Error('OpfsClipPageProvider is closed'));
      }
      pending.clear();
      provider.destroy();
      if (ownsWorker) {
        worker.terminate();
      }
    },
  };
}
