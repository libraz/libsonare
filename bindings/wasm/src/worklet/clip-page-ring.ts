import type { ClipPagePageRequest } from '../clip_page_streamer.js';
import { refuse, withEntry } from '../internal/entry.js';
import {
  loadSonareRingCursor,
  type RingCapacityOptions,
  requirePositiveCapacity,
  resolveRingCapacity,
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
/** Number of Int32 slots in the request ring header. */
export const SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS = 5;
/** Number of uint32 words per request record: `[clipId, pageIndex]`. */
export const SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S = 2;

export type { ClipPagePageRequest } from '../clip_page_streamer.js';

/** Shared-memory clip-page request ring. */
export interface ClipPageRequestRingBuffer {
  /** Backing buffer holding the header and the records. */
  sharedBuffer: SharedArrayBuffer;
  /** Ring header: write index, read index, capacity, record size and drop count. */
  header: Int32Array;
  /** Request records, `[clipId, pageIndex]` each. */
  records: Uint32Array;
  /** Number of records the ring holds. */
  capacity: number;
}

export interface SharedClipPageRequestRingWriter {
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

/** Requests drained from a clip-page request ring. */
export interface ClipPageRequestRingReadResult {
  /** Requests read, oldest first. */
  requests: ClipPagePageRequest[];
  /** Cumulative uint32 drop count for native and SAB-ring overflows. */
  dropped: number;
}

/** Byte length of the ring. `capacity` is required; RangeError outside the domain, TypeError for an unknown key. */
export function clipPageRequestRingBufferByteLength(
  options: Required<RingCapacityOptions>,
): number {
  return withEntry('clipPageRequestRingBufferByteLength', () => {
    const clampedCapacity = resolveRingCapacity(options, undefined);
    return (
      SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
      clampedCapacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S * Uint32Array.BYTES_PER_ELEMENT
    );
  });
}

/** Creates a shared clip-page request ring (default `capacity` 128). RangeError outside the domain, TypeError for an unknown key. */
export function createClipPageRequestRingBuffer(
  options?: RingCapacityOptions,
): ClipPageRequestRingBuffer {
  return withEntry('createClipPageRequestRingBuffer', () => {
    const clampedCapacity = resolveRingCapacity(options, 128);
    const sharedBuffer = new SharedArrayBuffer(
      clipPageRequestRingBufferByteLength({ capacity: clampedCapacity }),
    );
    const ring = clipPageRequestRingFromSharedBuffer(sharedBuffer, clampedCapacity);
    Atomics.store(ring.header, 0, 0);
    Atomics.store(ring.header, 1, 0);
    Atomics.store(ring.header, 2, ring.capacity);
    Atomics.store(ring.header, 3, SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S);
    Atomics.store(ring.header, 4, 0);
    return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
  });
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
export function readClipPageRequestRingBuffer(
  ring: ClipPageRequestRingBuffer,
): ClipPageRequestRingReadResult {
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const pending = Math.min(
    ring.capacity,
    sonareRingCursorDistance(readIndex, writeIndex, ring.capacity),
  );
  const requests: ClipPagePageRequest[] = [];
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
  const minBytes = clipPageRequestRingBufferByteLength({ capacity });
  if (sharedBuffer.byteLength < minBytes) {
    throw refuse('clipPageRequestSharedBuffer is too small for the requested ring capacity.');
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
