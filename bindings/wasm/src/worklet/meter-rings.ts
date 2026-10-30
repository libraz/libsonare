import type { EngineMeterTelemetryWide } from '../index.js';
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

export interface SonareWorkletMeterSnapshot {
  type: 'meter';
  targetId: number;
  frame: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4 polyphase reconstruction
   * at 4x. A streaming measurement that runs one reconstruction-filter group delay (a few samples)
   * behind the block, so every value has its whole stencil and the reading does not depend on the
   * block size. Use `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /** Maximum pre-trim input peak for the left channel, in dBFS. */
  inputPeakDbL: number;
  /** Maximum pre-trim input peak for the right channel, in dBFS. */
  inputPeakDbR: number;
  /**
   * Per-insert gain reduction in dB (each <= 0) of the target's strip, in combined
   * pre-to-post insert order, folded to the deepest value across the sub-blocks of the
   * latest committed host block. It describes that latest block as of the drain, not
   * necessarily this record's own block, so it is attached only to the newest record per
   * target within one drain; older backlogged records omit it. Present only on snapshots
   * delivered over postMessage; the SharedArrayBuffer meter ring has a fixed layout that
   * does not carry it, so ring hosts read it with `pollInsertGainReduction(targetId)`.
   */
  insertGainReductionDb?: number[];
  /**
   * Number of metered planes of the target, present together with the per-channel
   * arrays below only when it exceeds 2 (a 5.1 or 7.1 bus). The stereo fields
   * above always hold planes 0 and 1.
   */
  channelCount?: number;
  /** Per-plane peak in dBFS (length `channelCount`), canonical WAVE order. */
  peakDb?: number[];
  /** Per-plane RMS in dBFS (length `channelCount`). */
  rmsDb?: number[];
  /** Per-plane inter-sample (true) peak in dB (length `channelCount`). See {@link truePeakDbL}. */
  truePeakDb?: number[];
  /** Per-plane pre-trim input peak in dBFS (length `channelCount`). */
  inputPeakDb?: number[];
}

export interface SonareWorkletSpectrumSnapshot {
  type: 'spectrum';
  frame: number;
  bands: Float32Array;
}

export const SONARE_METER_RING_HEADER_INTS = 4;

// Record layout: [frameLo, frameHi, targetId, peakDbL, peakDbR, rmsDbL, rmsDbR,
// correlation, truePeakDbL, truePeakDbR, momentaryLufs, shortTermLufs,
// integratedLufs, gainReductionDb, inputPeakDbL, inputPeakDbR, channelCount,
// peakDb[0..7], rmsDb[0..7], truePeakDb[0..7], inputPeakDb[0..7]].
// Header slot 3 holds SONARE_METER_RING_PROTOCOL_VERSION; older records stopped
// after inputPeakDbR (16 floats) and carried no per-plane data.
// The sample-frame index is monotonically increasing and quickly exceeds the
// 2^24 exact-integer range of a single Float32 slot (~349 s at 48 kHz), so it is
// stored split across two Float32 lanes (low 24 bits + high bits) for exact
// reconstruction. See encodeFrameLo/encodeFrameHi/decodeFrame.
export const SONARE_METER_RING_PROTOCOL_VERSION = 3;
export const SONARE_METER_RING_MAX_CHANNELS = 8;
export const SONARE_METER_RING_CHANNEL_COUNT_OFFSET = 16;
export const SONARE_METER_RING_PLANES_OFFSET = 17;
export const SONARE_METER_RING_RECORD_FLOATS =
  SONARE_METER_RING_PLANES_OFFSET + 4 * SONARE_METER_RING_MAX_CHANNELS;
export const SONARE_SPECTRUM_RING_HEADER_INTS = 5;
export const SONARE_SPECTRUM_RING_PROTOCOL_VERSION = 1;

export interface SonareMeterRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
}

export interface SonareMeterRingReadResult {
  nextReadIndex: number;
  meters: SonareWorkletMeterSnapshot[];
}

export interface SonareSpectrumRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
  bands: number;
}

export interface SonareSpectrumRingReadResult {
  nextReadIndex: number;
  spectra: SonareWorkletSpectrumSnapshot[];
}

export interface SharedMeterRingWriter {
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
}

export interface SharedSpectrumRingWriter {
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
  bands: number;
  recordFloats: number;
}

/**
 * Shared finite dB floor (`sonare::constants::kFloorDb`). Every level-in-dB
 * field on a worklet snapshot bottoms out here rather than at `-Infinity`.
 */
export const SONARE_FLOOR_DB = -120;

export function sonareMeterRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return overwriteRingByteLength(
    SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
    SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT,
    clampedCapacity,
  );
}

export function createSonareMeterRingBuffer(capacity = 128): SonareMeterRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(sonareMeterRingBufferByteLength(clampedCapacity));
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordBytes = SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_METER_RING_HEADER_INTS);
  const records = new Float32Array(
    sharedBuffer,
    headerBytes,
    clampedCapacity * SONARE_METER_RING_RECORD_FLOATS,
  );
  const slotStates = makeOverwriteSlotStates(
    sharedBuffer,
    headerBytes,
    recordBytes,
    clampedCapacity,
  );
  Atomics.store(header, 0, 0);
  Atomics.store(header, 1, clampedCapacity);
  Atomics.store(header, 2, SONARE_METER_RING_RECORD_FLOATS);
  Atomics.store(header, 3, 0);
  slotStates.fill(0);
  return { sharedBuffer, header, records, slotStates, capacity: clampedCapacity };
}

export function readSonareMeterRingBuffer(
  ring: SonareMeterRingBuffer,
  readIndex = 0,
): SonareMeterRingReadResult {
  const { sharedBuffer, header, records, slotStates, capacity } = ring;
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordBytes = SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
  validateOverwriteViews(
    sharedBuffer,
    header,
    records,
    slotStates,
    headerBytes,
    recordBytes,
    capacity,
    'meter ring',
  );
  if (
    Atomics.load(header, 1) !== capacity ||
    Atomics.load(header, 2) !== SONARE_METER_RING_RECORD_FLOATS
  ) {
    throw new Error('meter ring has invalid descriptors.');
  }
  const writeIndex = loadSonareRingCursor(header, 0, capacity);
  const version = Atomics.load(header, 3);
  if (version === 0 && writeIndex === 0) {
    return { nextReadIndex: writeIndex, meters: [] };
  }
  if (version !== SONARE_METER_RING_PROTOCOL_VERSION) {
    throw new Error(
      `meter ring protocol version ${version} is incompatible; expected ${SONARE_METER_RING_PROTOCOL_VERSION}.`,
    );
  }
  const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
  const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
  const meters: SonareWorkletMeterSnapshot[] = [];
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
      const offset = sonareRingCursorSlot(cursor, capacity) * SONARE_METER_RING_RECORD_FLOATS;
      const meter: SonareWorkletMeterSnapshot = {
        type: 'meter',
        frame: decodeFrame(records[offset], records[offset + 1]),
        targetId: records[offset + 2],
        peakDbL: records[offset + 3],
        peakDbR: records[offset + 4],
        rmsDbL: records[offset + 5],
        rmsDbR: records[offset + 6],
        correlation: records[offset + 7],
        truePeakDbL: records[offset + 8],
        truePeakDbR: records[offset + 9],
        momentaryLufs: records[offset + 10],
        shortTermLufs: records[offset + 11],
        integratedLufs: records[offset + 12],
        gainReductionDb: records[offset + 13],
        inputPeakDbL: records[offset + 14],
        inputPeakDbR: records[offset + 15],
      };
      const channelCount = records[offset + SONARE_METER_RING_CHANNEL_COUNT_OFFSET];
      if (channelCount > 2) {
        const planes = Math.min(channelCount, SONARE_METER_RING_MAX_CHANNELS);
        const plane = (kind: number): number[] =>
          Array.from(
            records.subarray(
              offset + SONARE_METER_RING_PLANES_OFFSET + kind * SONARE_METER_RING_MAX_CHANNELS,
              offset +
                SONARE_METER_RING_PLANES_OFFSET +
                kind * SONARE_METER_RING_MAX_CHANNELS +
                planes,
            ),
          );
        meter.channelCount = planes;
        meter.peakDb = plane(0);
        meter.rmsDb = plane(1);
        meter.truePeakDb = plane(2);
        meter.inputPeakDb = plane(3);
      }
      meters.push(meter);
    } finally {
      releaseOverwriteRead(slotStates, capacity, cursor);
    }
  }
  return { nextReadIndex, meters };
}

export function sonareSpectrumRingBufferByteLength(capacity: number, bands = 16): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const clampedBands = requireRingDimension(bands, 'bands', 1);
  // Record layout: [frameLo, frameHi, bandCount, band0, band1, ...]. frame is
  // split across two Float32 lanes for exact reconstruction beyond 2^24.
  return overwriteRingByteLength(
    SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
    (3 + clampedBands) * Float32Array.BYTES_PER_ELEMENT,
    clampedCapacity,
  );
}

export function createSonareSpectrumRingBuffer(
  capacity = 128,
  bands = 16,
): SonareSpectrumRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const clampedBands = requireRingDimension(bands, 'bands', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareSpectrumRingBufferByteLength(clampedCapacity, clampedBands),
  );
  const headerBytes = SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordFloats = 3 + clampedBands;
  const recordBytes = recordFloats * Float32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_SPECTRUM_RING_HEADER_INTS);
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
  Atomics.store(header, 4, 0);
  slotStates.fill(0);
  return {
    sharedBuffer,
    header,
    records,
    slotStates,
    capacity: clampedCapacity,
    bands: clampedBands,
  };
}

export function readSonareSpectrumRingBuffer(
  ring: SonareSpectrumRingBuffer,
  readIndex = 0,
): SonareSpectrumRingReadResult {
  const { sharedBuffer, header, records, slotStates, capacity, bands } = ring;
  const recordFloats = 3 + bands;
  const headerBytes = SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordBytes = recordFloats * Float32Array.BYTES_PER_ELEMENT;
  validateOverwriteViews(
    sharedBuffer,
    header,
    records,
    slotStates,
    headerBytes,
    recordBytes,
    capacity,
    'spectrum ring',
  );
  if (
    Atomics.load(header, 1) !== capacity ||
    Atomics.load(header, 2) !== recordFloats ||
    Atomics.load(header, 3) !== bands
  ) {
    throw new Error('spectrum ring has invalid descriptors.');
  }
  const writeIndex = loadSonareRingCursor(header, 0, capacity);
  const version = Atomics.load(header, 4);
  if (version === 0 && writeIndex === 0) {
    return { nextReadIndex: writeIndex, spectra: [] };
  }
  if (version !== SONARE_SPECTRUM_RING_PROTOCOL_VERSION) {
    throw new Error(
      `spectrum ring protocol version ${version} is incompatible; expected ${SONARE_SPECTRUM_RING_PROTOCOL_VERSION}.`,
    );
  }
  const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
  const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
  const spectra: SonareWorkletSpectrumSnapshot[] = [];
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
      const values = new Float32Array(bands);
      values.set(records.subarray(offset + 3, offset + 3 + bands));
      spectra.push({
        type: 'spectrum',
        frame: decodeFrame(records[offset], records[offset + 1]),
        bands: values,
      });
    } finally {
      releaseOverwriteRead(slotStates, capacity, cursor);
    }
  }
  return { nextReadIndex, spectra };
}

export function meterRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedMeterRingWriter {
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw new Error('meterSharedBuffer is too small for the ring header.');
  }
  const header = new Int32Array(sharedBuffer, 0, SONARE_METER_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const capacity = requirePositiveCapacity(existingCapacity, fallbackCapacity, 'meterRingCapacity');
  const recordBytes = SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
  const records = new Float32Array(
    sharedBuffer,
    headerBytes,
    capacity * SONARE_METER_RING_RECORD_FLOATS,
  );
  const slotStates = makeOverwriteSlotStates(sharedBuffer, headerBytes, recordBytes, capacity);
  validateOverwriteViews(
    sharedBuffer,
    header,
    records,
    slotStates,
    headerBytes,
    recordBytes,
    capacity,
    'meter ring',
  );
  if (
    Atomics.load(header, 2) !== SONARE_METER_RING_RECORD_FLOATS ||
    Atomics.load(header, 1) !== capacity
  ) {
    throw new Error('meter ring has invalid descriptors.');
  }
  requireProtocolVersion(header, 3, SONARE_METER_RING_PROTOCOL_VERSION, 'meter ring');
  requireNoUnpublishedRecords(header, 3, capacity, 'meter ring');
  attachProtocolVersion(header, 3, SONARE_METER_RING_PROTOCOL_VERSION, 'meter ring');
  return {
    header,
    records,
    slotStates,
    capacity,
  };
}

export function spectrumRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
  fallbackBands?: number,
): SharedSpectrumRingWriter {
  const headerBytes = SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw new Error('spectrumSharedBuffer is too small for the ring header.');
  }
  const header = new Int32Array(sharedBuffer, 0, SONARE_SPECTRUM_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const existingBands = Atomics.load(header, 3);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'spectrumRingCapacity',
  );
  const bands = existingBands || fallbackBands || 16;
  if (!Number.isSafeInteger(bands) || bands < 1) {
    throw new RangeError('spectrumBands must be a positive integer');
  }
  if (fallbackBands !== undefined && existingBands !== 0 && existingBands !== fallbackBands) {
    throw new RangeError('spectrumBands does not match the shared-buffer descriptor');
  }
  const recordFloats = 3 + bands;
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
    'spectrum ring',
  );
  if (Atomics.load(header, 2) !== recordFloats || Atomics.load(header, 3) !== bands) {
    throw new Error('spectrum ring has invalid descriptors.');
  }
  requireProtocolVersion(header, 4, SONARE_SPECTRUM_RING_PROTOCOL_VERSION, 'spectrum ring');
  requireNoUnpublishedRecords(header, 4, capacity, 'spectrum ring');
  attachProtocolVersion(header, 4, SONARE_SPECTRUM_RING_PROTOCOL_VERSION, 'spectrum ring');
  return {
    header,
    records,
    slotStates,
    capacity,
    bands,
    recordFloats,
  };
}

// A plane the target does not have (the right side of a mono lane) reads as the floor.
export function meterFromEngine(meter: EngineMeterTelemetryWide): SonareWorkletMeterSnapshot {
  const snapshot: SonareWorkletMeterSnapshot = {
    type: 'meter',
    targetId: meter.targetId,
    frame: meter.renderFrame,
    peakDbL: meter.peakDb[0] ?? SONARE_FLOOR_DB,
    peakDbR: meter.peakDb[1] ?? SONARE_FLOOR_DB,
    rmsDbL: meter.rmsDb[0] ?? SONARE_FLOOR_DB,
    rmsDbR: meter.rmsDb[1] ?? SONARE_FLOOR_DB,
    correlation: meter.correlation,
    truePeakDbL: meter.truePeakDb[0] ?? SONARE_FLOOR_DB,
    truePeakDbR: meter.truePeakDb[1] ?? SONARE_FLOOR_DB,
    momentaryLufs: meter.momentaryLufs,
    shortTermLufs: meter.shortTermLufs,
    integratedLufs: meter.integratedLufs,
    gainReductionDb: meter.gainReductionDb,
    inputPeakDbL: meter.inputPeakDb[0] ?? SONARE_FLOOR_DB,
    inputPeakDbR: meter.inputPeakDb[1] ?? SONARE_FLOOR_DB,
  };
  if (meter.channelCount > 2) {
    snapshot.channelCount = meter.channelCount;
    snapshot.peakDb = meter.peakDb;
    snapshot.rmsDb = meter.rmsDb;
    snapshot.truePeakDb = meter.truePeakDb;
    snapshot.inputPeakDb = meter.inputPeakDb;
  }
  return snapshot;
}

export function magnitudeToDb(value: number): number {
  return value > 1.0e-12 ? 20 * Math.log10(value) : SONARE_FLOOR_DB;
}
