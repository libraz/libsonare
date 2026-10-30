import {
  loadSonareRingCursor,
  RING_CURSOR_RANGE,
  requirePositiveCapacity,
  requireRingDimension,
  ringCursorSpan,
  sonareRingCursorDistance,
  sonareRingCursorSlot,
  storeSonareRingCursor,
} from './ring-core.js';

// External MIDI records: destination ID, render frame (low 32 bits), packed
// MIDI-1 bytes (little-endian), byte count, render frame (high bits). SPSC
// worklet -> main-thread ring.
export const SONARE_EXTERNAL_MIDI_RING_HEADER_INTS = 5;
export const SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S = 5;

export interface SonareExternalMidiRingEvent {
  destinationId: number;
  renderFrame: number;
  byteWord: number;
  byteCount: number;
}

export interface SonareExternalMidiRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SharedExternalMidiRingWriter {
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SonareExternalMidiRingReadResult {
  events: SonareExternalMidiRingEvent[];
  dropped: number;
}

export function sonareExternalMidiRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return (
    SONARE_EXTERNAL_MIDI_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S * Uint32Array.BYTES_PER_ELEMENT
  );
}

export function externalMidiRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedExternalMidiRingWriter {
  const headerBytes = SONARE_EXTERNAL_MIDI_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_EXTERNAL_MIDI_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'externalMidiRingCapacity',
  );
  if (sharedBuffer.byteLength < sonareExternalMidiRingBufferByteLength(capacity)) {
    throw new Error('externalMidiSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 2, capacity);
  Atomics.store(header, 3, SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S);
  return {
    header,
    records: new Uint32Array(
      sharedBuffer,
      headerBytes,
      capacity * SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S,
    ),
    capacity,
  };
}

export function createSonareExternalMidiRingBuffer(capacity = 256): SonareExternalMidiRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareExternalMidiRingBufferByteLength(clampedCapacity),
  );
  const ring = externalMidiRingFromSharedBuffer(sharedBuffer, clampedCapacity);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, 0);
  Atomics.store(ring.header, 4, 0);
  return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
}

/** Whether one more external-MIDI record fits the ring now. */
export function sonareExternalMidiRingHasRoom(ring: SharedExternalMidiRingWriter): boolean {
  return (
    sonareRingCursorDistance(
      loadSonareRingCursor(ring.header, 1, ring.capacity),
      loadSonareRingCursor(ring.header, 0, ring.capacity),
      ring.capacity,
    ) < ring.capacity
  );
}

/** Write one lowered MIDI-1 event without allocations from the audio worklet. */
export function pushSonareExternalMidiRingBuffer(
  ring: SharedExternalMidiRingWriter,
  destinationId: number,
  renderFrame: number,
  byteWord: number,
  byteCount: number,
): boolean {
  // Emscripten can expose an int64 render-frame scalar as bigint when the
  // host enables WASM_BIGINT. Normalize at this fixed-record boundary.
  const destination = Number(destinationId);
  const frame = Number(renderFrame);
  const packedBytes = Number(byteWord);
  const count = Number(byteCount);
  if (
    !Number.isSafeInteger(destination) ||
    !Number.isSafeInteger(frame) ||
    frame < 0 ||
    !Number.isSafeInteger(packedBytes) ||
    packedBytes < 0 ||
    packedBytes > 0xff_ffff ||
    !Number.isSafeInteger(count) ||
    count < 1 ||
    count > 3
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
    sonareRingCursorSlot(writeIndex, ring.capacity) * SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S;
  Atomics.store(ring.records, offset, destination);
  Atomics.store(ring.records, offset + 1, frame >>> 0);
  Atomics.store(ring.records, offset + 2, packedBytes);
  Atomics.store(ring.records, offset + 3, count);
  Atomics.store(ring.records, offset + 4, Math.floor(frame / RING_CURSOR_RANGE));
  storeSonareRingCursor(ring.header, 0, writeIndex, ring.capacity);
  return true;
}

/** Main-thread drain. It is intentionally the only place MIDI byte arrays are built. */
export function readSonareExternalMidiRingBuffer(
  ring: SonareExternalMidiRingBuffer,
): SonareExternalMidiRingReadResult {
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const pending = Math.min(
    ring.capacity,
    sonareRingCursorDistance(readIndex, writeIndex, ring.capacity),
  );
  const events: SonareExternalMidiRingEvent[] = [];
  for (let i = 0; i < pending; i++) {
    const cursor = (readIndex + i) % ringCursorSpan(ring.capacity);
    const offset =
      sonareRingCursorSlot(cursor, ring.capacity) * SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S;
    events.push({
      destinationId: Atomics.load(ring.records, offset) >>> 0,
      renderFrame:
        (Atomics.load(ring.records, offset + 4) >>> 0) * RING_CURSOR_RANGE +
        (Atomics.load(ring.records, offset + 1) >>> 0),
      byteWord: Atomics.load(ring.records, offset + 2) >>> 0,
      byteCount: Atomics.load(ring.records, offset + 3) >>> 0,
    });
  }
  storeSonareRingCursor(ring.header, 1, readIndex, ring.capacity, pending);
  return { events, dropped: Atomics.load(ring.header, 4) >>> 0 };
}
