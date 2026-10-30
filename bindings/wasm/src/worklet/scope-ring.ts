import { decodeFrame } from './engine-rings.js';
import {
  attachProtocolVersion,
  loadSonareRingCursor,
  makeOverwriteSlotStates,
  overwriteRingByteLength,
  overwriteRingFirstReadable,
  releaseOverwriteRead,
  requireNoUnpublishedRecords,
  requirePositiveCapacity,
  requireProtocolVersion,
  requireRingDimension,
  ringCursorSpan,
  sonareRingCursorDistance,
  sonareRingCursorSlot,
  tryAcquireOverwriteRead,
  validateOverwriteViews,
} from './ring-core.js';

// Scope ring header: [writeIndex, capacity, recordFloats, bands, maxPoints,
// protocolVersion]. Record layout: [frameLo, frameHi, targetId, bandCount, pointCount,
// band0..band(bands-1), l0, r0, l1, r1, ... (maxPoints stereo pairs)].
export const SONARE_SCOPE_RING_HEADER_INTS = 6;
export const SONARE_SCOPE_RING_PROTOCOL_VERSION = 1;
export const SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS = 5;

/** Scope band and goniometer point counts used when a caller supplies none. */
export const SONARE_SCOPE_DEFAULT_BANDS = 48;
export const SONARE_SCOPE_DEFAULT_MAX_POINTS = 32;
export const SONARE_SCOPE_MAX_BANDS = 64;
export const SONARE_SCOPE_MAX_POINTS = 32;

/**
 * A single target-addressed scope record drained from the realtime engine's
 * scope ring: an FFT magnitude spectrum plus a decimated goniometer/vectorscope
 * point cloud. Unlike {@link SonareWorkletSpectrumSnapshot} (the legacy
 * coarse-DFT meter spectrum), this carries a `targetId` (master/lane/bus) and a
 * stereo point cloud, mirroring the engine's `ScopeTelemetryRecord`.
 */
export interface SonareWorkletScopeSnapshot {
  type: 'scope';
  targetId: number;
  frame: number;
  /** Linear-band magnitudes in dB (length = the configured band count). */
  bands: Float32Array;
  /** Interleaved stereo goniometer points: [l0, r0, l1, r1, ...]. */
  points: Float32Array;
}

export interface SonareScopeRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
  bands: number;
  maxPoints: number;
}

export interface SonareScopeRingReadResult {
  nextReadIndex: number;
  scopes: SonareWorkletScopeSnapshot[];
}

export interface SharedScopeRingWriter {
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
  bands: number;
  maxPoints: number;
  recordFloats: number;
}

export function sonareScopeRingRecordFloats(bands: number, maxPoints: number): number {
  return SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS + bands + 2 * maxPoints;
}

export function sonareScopeRingBufferByteLength(
  capacity: number,
  bands = SONARE_SCOPE_DEFAULT_BANDS,
  maxPoints = SONARE_SCOPE_DEFAULT_MAX_POINTS,
): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const clampedBands = requireRingDimension(bands, 'bands', 1, SONARE_SCOPE_MAX_BANDS);
  const clampedPoints = requireRingDimension(maxPoints, 'maxPoints', 0, SONARE_SCOPE_MAX_POINTS);
  return overwriteRingByteLength(
    SONARE_SCOPE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
    sonareScopeRingRecordFloats(clampedBands, clampedPoints) * Float32Array.BYTES_PER_ELEMENT,
    clampedCapacity,
  );
}

export function createSonareScopeRingBuffer(
  capacity = 64,
  bands = SONARE_SCOPE_DEFAULT_BANDS,
  maxPoints = SONARE_SCOPE_DEFAULT_MAX_POINTS,
): SonareScopeRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const clampedBands = requireRingDimension(bands, 'bands', 1, SONARE_SCOPE_MAX_BANDS);
  const clampedPoints = requireRingDimension(maxPoints, 'maxPoints', 0, SONARE_SCOPE_MAX_POINTS);
  const sharedBuffer = new SharedArrayBuffer(
    sonareScopeRingBufferByteLength(clampedCapacity, clampedBands, clampedPoints),
  );
  const headerBytes = SONARE_SCOPE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordFloats = sonareScopeRingRecordFloats(clampedBands, clampedPoints);
  const recordBytes = recordFloats * Float32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_SCOPE_RING_HEADER_INTS);
  const records = new Float32Array(sharedBuffer, headerBytes, clampedCapacity * recordFloats);
  const slotStates = makeOverwriteSlotStates(
    sharedBuffer,
    headerBytes,
    recordBytes,
    clampedCapacity,
  );
  Atomics.store(header, 0, 0);
  Atomics.store(header, 1, clampedCapacity);
  Atomics.store(header, 2, recordFloats);
  Atomics.store(header, 3, clampedBands);
  Atomics.store(header, 4, clampedPoints);
  Atomics.store(header, 5, 0);
  slotStates.fill(0);
  return {
    sharedBuffer,
    header,
    records,
    slotStates,
    capacity: clampedCapacity,
    bands: clampedBands,
    maxPoints: clampedPoints,
  };
}

export function readSonareScopeRingBuffer(
  ring: SonareScopeRingBuffer,
  readIndex = 0,
): SonareScopeRingReadResult {
  const { sharedBuffer, header, records, slotStates, capacity, bands, maxPoints } = ring;
  const recordFloats = sonareScopeRingRecordFloats(bands, maxPoints);
  const headerBytes = SONARE_SCOPE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordBytes = recordFloats * Float32Array.BYTES_PER_ELEMENT;
  validateOverwriteViews(
    sharedBuffer,
    header,
    records,
    slotStates,
    headerBytes,
    recordBytes,
    capacity,
    'scope ring',
  );
  if (
    Atomics.load(header, 1) !== capacity ||
    Atomics.load(header, 2) !== recordFloats ||
    Atomics.load(header, 3) !== bands ||
    Atomics.load(header, 4) !== maxPoints
  ) {
    throw new Error('scope ring has invalid descriptors.');
  }
  const writeIndex = loadSonareRingCursor(header, 0, capacity);
  const version = Atomics.load(header, 5);
  if (version === 0 && writeIndex === 0) {
    return { nextReadIndex: writeIndex, scopes: [] };
  }
  if (version !== SONARE_SCOPE_RING_PROTOCOL_VERSION) {
    throw new Error(
      `scope ring protocol version ${version} is incompatible; expected ${SONARE_SCOPE_RING_PROTOCOL_VERSION}.`,
    );
  }
  const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
  const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
  const scopes: SonareWorkletScopeSnapshot[] = [];
  let nextReadIndex = writeIndex;
  for (let i = 0; i < count; i++) {
    const cursor = (firstReadable + i) % ringCursorSpan(capacity);
    if (!tryAcquireOverwriteRead(slotStates, capacity, cursor)) {
      // A writer is still publishing this record; resume here on the next read.
      nextReadIndex = cursor;
      break;
    }
    try {
      const currentWriteIndex = loadSonareRingCursor(header, 0, capacity);
      const age = sonareRingCursorDistance(cursor, currentWriteIndex, capacity);
      if (age < 1 || age > capacity) {
        continue;
      }
      const offset = sonareRingCursorSlot(cursor, capacity) * recordFloats;
      const bandCount = Math.min(bands, Math.max(0, records[offset + 3]));
      const pointCount = Math.min(maxPoints, Math.max(0, records[offset + 4]));
      const bandsView = new Float32Array(bandCount);
      bandsView.set(
        records.subarray(
          offset + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS,
          offset + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS + bandCount,
        ),
      );
      const pointsBase = offset + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS + bands;
      const pointsView = new Float32Array(pointCount * 2);
      pointsView.set(records.subarray(pointsBase, pointsBase + pointCount * 2));
      scopes.push({
        type: 'scope',
        frame: decodeFrame(records[offset], records[offset + 1]),
        targetId: records[offset + 2],
        bands: bandsView,
        points: pointsView,
      });
    } finally {
      releaseOverwriteRead(slotStates, capacity, cursor);
    }
  }
  return { nextReadIndex, scopes };
}

export function scopeRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
  fallbackBands?: number,
  fallbackMaxPoints?: number,
): SharedScopeRingWriter {
  const headerBytes = SONARE_SCOPE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw new Error('scopeSharedBuffer is too small for the ring header.');
  }
  const header = new Int32Array(sharedBuffer, 0, SONARE_SCOPE_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const existingBands = Atomics.load(header, 3);
  const existingMaxPoints = Atomics.load(header, 4);
  const recordDescriptor = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(existingCapacity, fallbackCapacity, 'scopeRingCapacity');
  const bands = recordDescriptor ? existingBands : (fallbackBands ?? SONARE_SCOPE_DEFAULT_BANDS);
  const maxPoints = recordDescriptor
    ? existingMaxPoints
    : (fallbackMaxPoints ?? SONARE_SCOPE_DEFAULT_MAX_POINTS);
  if (!Number.isSafeInteger(bands) || bands < 1) {
    throw new RangeError('scopeBands must be a positive integer');
  }
  if (!Number.isSafeInteger(maxPoints) || maxPoints < 0) {
    throw new RangeError('scopeMaxPoints must be a non-negative integer');
  }
  if (fallbackBands !== undefined && bands !== fallbackBands) {
    throw new RangeError('scopeBands does not match the shared-buffer descriptor');
  }
  if (fallbackMaxPoints !== undefined && maxPoints !== fallbackMaxPoints) {
    throw new RangeError('scopeMaxPoints does not match the shared-buffer descriptor');
  }
  const recordFloats = sonareScopeRingRecordFloats(bands, maxPoints);
  const recordBytes = recordFloats * Float32Array.BYTES_PER_ELEMENT;
  const records = new Float32Array(sharedBuffer, headerBytes, capacity * recordFloats);
  const slotStates = makeOverwriteSlotStates(sharedBuffer, headerBytes, recordBytes, capacity);
  validateOverwriteViews(
    sharedBuffer,
    header,
    records,
    slotStates,
    headerBytes,
    recordBytes,
    capacity,
    'scope ring',
  );
  if (
    Atomics.load(header, 2) !== recordFloats ||
    Atomics.load(header, 3) !== bands ||
    Atomics.load(header, 4) !== maxPoints
  ) {
    throw new Error('scope ring has invalid descriptors.');
  }
  requireProtocolVersion(header, 5, SONARE_SCOPE_RING_PROTOCOL_VERSION, 'scope ring');
  requireNoUnpublishedRecords(header, 5, capacity, 'scope ring');
  attachProtocolVersion(header, 5, SONARE_SCOPE_RING_PROTOCOL_VERSION, 'scope ring');
  return {
    header,
    records,
    slotStates,
    capacity,
    bands,
    maxPoints,
    recordFloats,
  };
}
