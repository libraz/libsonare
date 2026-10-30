import {
  loadSonareRingCursor,
  requirePositiveCapacity,
  requireRingDimension,
  ringCursorSpan,
  sonareRingCursorDistance,
  sonareRingCursorSlot,
  storeSonareRingCursor,
} from './ring-core.js';

// Clip-page request ring header: [writeIndex, readIndex, capacity,
// recordUint32s, dropped]. Records are [clipId, pageIndex]. This is an SPSC
// queue: the AudioWorklet is the sole writer and the main thread is the sole
// reader. Unlike the telemetry rings, it must not overwrite unread requests:
// the producer increments `dropped` when the fixed queue is full.
export const SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS = 5;
export const SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S = 2;

export interface SonareClipPageRequest {
  clipId: number;
  pageIndex: number;
}

export interface SonareClipPageRequestRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SharedClipPageRequestRingWriter {
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SonareClipPageRequestRingReadResult {
  requests: SonareClipPageRequest[];
  /** Cumulative uint32 drop count for native and SAB-ring overflows. */
  dropped: number;
}

export function sonareClipPageRequestRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return (
    SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S * Uint32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareClipPageRequestRingBuffer(
  capacity = 128,
): SonareClipPageRequestRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareClipPageRequestRingBufferByteLength(clampedCapacity),
  );
  const ring = clipPageRequestRingFromSharedBuffer(sharedBuffer, clampedCapacity);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, 0);
  Atomics.store(ring.header, 2, ring.capacity);
  Atomics.store(ring.header, 3, SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S);
  Atomics.store(ring.header, 4, 0);
  return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
}

/**
 * Writes one request without allocating. Invalid uint32 coordinates and a full
 * ring are counted as drops so the main thread can surface a bounded-loss
 * diagnostic without receiving a worklet postMessage.
 */
export function pushSonareClipPageRequestRingBuffer(
  ring: SharedClipPageRequestRingWriter,
  clipId: number,
  pageIndex: number,
): boolean {
  if (
    !Number.isSafeInteger(clipId) ||
    clipId < 0 ||
    clipId > 0xffff_ffff ||
    !Number.isSafeInteger(pageIndex) ||
    pageIndex < 0 ||
    pageIndex > 0xffff_ffff
  ) {
    Atomics.add(ring.header, 4, 1);
    return false;
  }
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  if (sonareRingCursorDistance(readIndex, writeIndex, ring.capacity) >= ring.capacity) {
    Atomics.add(ring.header, 4, 1);
    return false;
  }
  const offset =
    sonareRingCursorSlot(writeIndex, ring.capacity) * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S;
  Atomics.store(ring.records, offset, clipId);
  Atomics.store(ring.records, offset + 1, pageIndex);
  // Publish only after both scalar fields have been written.
  storeSonareRingCursor(ring.header, 0, writeIndex, ring.capacity);
  return true;
}

/** Drains the SPSC request ring on the main thread. Object creation is here, never in process(). */
export function readSonareClipPageRequestRingBuffer(
  ring: SonareClipPageRequestRingBuffer,
): SonareClipPageRequestRingReadResult {
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const pending = Math.min(
    ring.capacity,
    sonareRingCursorDistance(readIndex, writeIndex, ring.capacity),
  );
  const requests: SonareClipPageRequest[] = [];
  for (let i = 0; i < pending; i++) {
    const cursor = (readIndex + i) % ringCursorSpan(ring.capacity);
    const offset =
      sonareRingCursorSlot(cursor, ring.capacity) * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S;
    requests.push({
      clipId: Atomics.load(ring.records, offset) >>> 0,
      pageIndex: Atomics.load(ring.records, offset + 1) >>> 0,
    });
  }
  storeSonareRingCursor(ring.header, 1, readIndex, ring.capacity, pending);
  return { requests, dropped: Atomics.load(ring.header, 4) >>> 0 };
}

export function clipPageRequestRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedClipPageRequestRingWriter {
  const headerBytes = SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'clipPageRequestRingCapacity',
  );
  const minBytes = sonareClipPageRequestRingBufferByteLength(capacity);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('clipPageRequestSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 2, capacity);
  Atomics.store(header, 3, SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S);
  return {
    header,
    records: new Uint32Array(
      sharedBuffer,
      headerBytes,
      capacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
    ),
    capacity,
  };
}
