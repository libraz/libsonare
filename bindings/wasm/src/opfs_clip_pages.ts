import { ErrorCode, SonareError } from './errors.js';
import type { ClipPageProvider, ClipPageRequest, RealtimeEngine } from './realtime_engine.js';

export interface OpfsClipPageProviderOptions {
  path: string;
  numChannels: number;
  numSamples: number;
  pageFrames: number;
  dataOffsetBytes?: number;
  worker?: Worker;
  terminateWorkerOnClose?: boolean;
  /** Internal bridge hook used to mirror a supplied page into an AudioWorklet. */
  onPageSupplied?: (pageIndex: number, channels: Float32Array[]) => void;
  /** Internal bridge hook used to mirror an eviction into an AudioWorklet. */
  onPageCleared?: (pageIndex: number) => void;
  /** Internal bridge hook called after the local provider is destroyed. */
  onClose?: () => void;
}

export interface OpfsClipPageProviderBinding {
  provider: ClipPageProvider;
  supplyPage(pageIndex: number): Promise<boolean>;
  supplyRequest(request: ClipPageRequest): Promise<boolean>;
  /** Evict one resident page from every configured consumer. */
  clearPage?(pageIndex: number): void;
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

async function sonareOpenClipFile(path, create) {
  const root = await self.navigator.storage.getDirectory();
  let dir = root;
  const parts = String(path).split('/').filter(Boolean);
  for (let i = 0; i < parts.length - 1; ++i) {
    dir = await dir.getDirectoryHandle(parts[i], { create });
  }
  return dir.getFileHandle(parts[parts.length - 1], { create });
}

async function sonareWriteClipChunk(message) {
  const { requestId, path, at, bytes, first } = message;
  try {
    const fileHandle = await sonareOpenClipFile(path, true);
    const access = await fileHandle.createSyncAccessHandle();
    try {
      if (first) {
        access.truncate(0);
      }
      const view = new Uint8Array(bytes);
      let written = 0;
      while (written < view.byteLength) {
        const n = access.write(view.subarray(written), { at: at + written });
        if (n <= 0) {
          throw new Error('short write');
        }
        written += n;
      }
      access.flush();
    } finally {
      access.close();
    }
    self.postMessage({ type: 'sonare:write-clip-result', requestId, ok: true });
  } catch (error) {
    self.postMessage({
      type: 'sonare:write-clip-result',
      requestId,
      ok: false,
      busy: !!error && error.name === 'NoModificationAllowedError',
      error: error instanceof Error ? error.message : String(error),
    });
  }
}

self.onmessage = async (event) => {
  const message = event.data;
  if (message && message.type === 'sonare:write-clip') {
    await sonareEnqueueClipPageRead(String(message.path), () => sonareWriteClipChunk(message));
    return;
  }
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

const heldOpfsClipPaths = new Map<string, number>();

function opfsClipPathKey(path: string): string {
  return path.split('/').filter(Boolean).join('/');
}

export function createOpfsClipPageWorker(): Worker {
  const blob = new Blob([opfsClipPageWorkerSource], { type: 'text/javascript' });
  const url = URL.createObjectURL(blob);
  try {
    return new Worker(url);
  } finally {
    // Worker construction retains the script URL internally; keeping this
    // object URL alive after construction leaks one browser registration per
    // attach/close cycle.
    URL.revokeObjectURL(url);
  }
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
  const heldKey = opfsClipPathKey(options.path);
  heldOpfsClipPaths.set(heldKey, (heldOpfsClipPaths.get(heldKey) ?? 0) + 1);
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
      options.onPageSupplied?.(response.pageIndex, channels);
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
    clearPage(pageIndex: number) {
      if (closed) {
        return;
      }
      provider.clear(pageIndex);
      options.onPageCleared?.(pageIndex);
    },
    close() {
      if (closed) {
        return;
      }
      closed = true;
      const held = (heldOpfsClipPaths.get(heldKey) ?? 1) - 1;
      if (held > 0) {
        heldOpfsClipPaths.set(heldKey, held);
      } else {
        heldOpfsClipPaths.delete(heldKey);
      }
      worker.removeEventListener('message', onMessage as EventListener);
      for (const entry of pending.values()) {
        entry.reject(new Error('OpfsClipPageProvider is closed'));
      }
      pending.clear();
      provider.destroy();
      options.onClose?.();
      if (ownsWorker) {
        worker.terminate();
      }
    },
  };
}

/** Result of {@link writeOpfsClip}. */
export interface OpfsClipWriteResult {
  path: string;
  numChannels: number;
  numSamples: number;
}

/** Result of {@link importOpfsClip}. */
export interface OpfsClipImportResult extends OpfsClipWriteResult {
  /** Sample rate of the decoded audio; pass it as the clip's source rate. */
  sampleRate: number;
}

/** Options for {@link writeOpfsClip} and {@link importOpfsClip}. */
export interface OpfsClipWriteOptions {
  /** Worker that runs the sync-access-handle writes; defaults to a private one. */
  worker?: Worker;
}

interface WriteResponse {
  type: 'sonare:write-clip-result';
  requestId: number;
  ok: boolean;
  busy?: boolean;
  error?: string;
}

const OPFS_WRITE_CHUNK_BYTES = 4 * 1024 * 1024;

function validateOpfsClipChannels(path: unknown, channels: unknown): Float32Array[] {
  if (typeof path !== 'string' || opfsClipPathKey(path) === '') {
    throw new TypeError('path must be a non-empty string');
  }
  if (!Array.isArray(channels) || !channels.every((c) => c instanceof Float32Array)) {
    throw new TypeError('channels must be an array of Float32Array');
  }
  const list = channels as Float32Array[];
  if (list.length === 0 || list[0].length === 0) {
    throw new RangeError('channels must be non-empty');
  }
  if (list.some((c) => c.length !== list[0].length)) {
    throw new RangeError('channels must all have the same length');
  }
  return list;
}

/**
 * Writes planar channels to an OPFS file in the layout the OPFS clip readers
 * expect: headerless little-endian float32, interleaved, starting at byte 0.
 * The file carries no sample rate, channel count or length; pass them to
 * {@link createOpfsClipPageProvider} (or `attachOpfsClipStream`) when reading
 * it back. An existing file at `path` is replaced.
 *
 * The write runs in an OPFS worker through a sync access handle. A path that an
 * open page provider currently reads is refused with `SonareError`
 * `InvalidState`.
 *
 * @param path - OPFS path, `/`-separated; missing directories are created.
 * @param channels - Non-empty, equal-length planar channels.
 * @throws TypeError when `path` or `channels` has the wrong type.
 * @throws RangeError when `channels` is empty or the lengths differ.
 */
export async function writeOpfsClip(
  path: string,
  channels: Float32Array[],
  options: OpfsClipWriteOptions = {},
): Promise<OpfsClipWriteResult> {
  const list = validateOpfsClipChannels(path, channels);
  const numChannels = list.length;
  const numSamples = list[0].length;
  if (heldOpfsClipPaths.has(opfsClipPathKey(path))) {
    throw new SonareError(
      ErrorCode.InvalidState,
      'InvalidState',
      `OPFS path '${path}' is held by an open clip page provider.`,
    );
  }
  const worker = options.worker ?? createOpfsClipPageWorker();
  const ownsWorker = options.worker === undefined;
  let nextRequestId = 1;
  const chunkFrames = Math.max(1, Math.floor(OPFS_WRITE_CHUNK_BYTES / (numChannels * 4)));
  try {
    for (let start = 0; start < numSamples; start += chunkFrames) {
      const frames = Math.min(chunkFrames, numSamples - start);
      const out = new Float32Array(frames * numChannels);
      for (let ch = 0; ch < numChannels; ++ch) {
        const src = list[ch];
        for (let i = 0; i < frames; ++i) {
          out[i * numChannels + ch] = src[start + i];
        }
      }
      const requestId = nextRequestId++;
      const response = await new Promise<WriteResponse>((resolve) => {
        const onMessage = (event: MessageEvent<WriteResponse>) => {
          if (
            event.data?.type !== 'sonare:write-clip-result' ||
            event.data.requestId !== requestId
          ) {
            return;
          }
          worker.removeEventListener('message', onMessage as EventListener);
          resolve(event.data);
        };
        worker.addEventListener('message', onMessage as EventListener);
        worker.postMessage(
          {
            type: 'sonare:write-clip',
            requestId,
            path,
            at: start * numChannels * 4,
            first: start === 0,
            bytes: out.buffer,
          },
          [out.buffer],
        );
      });
      if (!response.ok) {
        if (response.busy) {
          throw new SonareError(
            ErrorCode.InvalidState,
            'InvalidState',
            `OPFS path '${path}' is held by a reader.`,
          );
        }
        throw new Error(`OPFS clip write failed: ${response.error ?? 'unknown error'}`);
      }
    }
  } finally {
    if (ownsWorker) {
      worker.terminate();
    }
  }
  return { path, numChannels, numSamples };
}

function audioBufferChannels(buffer: AudioBuffer): Float32Array[] {
  return Array.from({ length: buffer.numberOfChannels }, (_, ch) => buffer.getChannelData(ch));
}

/**
 * Decodes `source` and writes it with {@link writeOpfsClip}. An `AudioBuffer`
 * keeps all its channels, and so do encoded bytes (`ArrayBuffer` or `Blob`),
 * which go through `decodeChannels` (with the browser codec fallback for formats
 * the native decoder does not carry). No resampling is applied: pass the
 * returned `sampleRate` as the clip's source rate.
 *
 * @param path - OPFS path, as for {@link writeOpfsClip}.
 * @param source - Encoded audio bytes or a decoded `AudioBuffer`.
 * @throws TypeError when `source` is none of the three accepted kinds.
 */
export async function importOpfsClip(
  path: string,
  source: ArrayBuffer | Blob | AudioBuffer,
  options: OpfsClipWriteOptions = {},
): Promise<OpfsClipImportResult> {
  let channels: Float32Array[];
  let sampleRate: number;
  if (
    source !== null &&
    typeof source === 'object' &&
    'numberOfChannels' in source &&
    'getChannelData' in source
  ) {
    channels = audioBufferChannels(source);
    sampleRate = source.sampleRate;
  } else {
    let bytes: ArrayBuffer;
    if (source instanceof ArrayBuffer) {
      bytes = source;
    } else if (typeof Blob !== 'undefined' && source instanceof Blob) {
      bytes = await source.arrayBuffer();
    } else {
      throw new TypeError('source must be an ArrayBuffer, Blob or AudioBuffer');
    }
    // Loaded on demand so the worklet entry does not carry the decoder.
    const { decodeChannelsWithBrowserFallback } = await import('./audio.js');
    const decoded = await decodeChannelsWithBrowserFallback(new Uint8Array(bytes));
    channels = decoded.channels;
    sampleRate = decoded.sampleRate;
  }
  const written = await writeOpfsClip(path, channels, options);
  return { ...written, sampleRate };
}
