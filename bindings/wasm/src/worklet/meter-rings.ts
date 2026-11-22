import { ErrorCode } from '../errors.js';
import type { EngineMeterTelemetry } from '../index.js';
import { codedError, refuse, withEntry } from '../internal/entry.js';
import { decodeFrame } from './engine-rings.js';
import {
  attachProtocolVersion,
  loadSonareRingCursor,
  makeOverwriteSlotStates,
  overwriteRingByteLength,
  overwriteRingFirstReadable,
  type RingCapacityOptions,
  releaseOverwriteRead,
  requireNoUnpublishedRecords,
  requirePositiveCapacity,
  requireProtocolVersion,
  requireRingDimension,
  resolveRingCapacity,
  resolveRingOptions,
  ringCursorSpan,
  sonareRingCursorDistance,
  sonareRingCursorSlot,
  tryAcquireOverwriteRead,
  validateOverwriteViews,
} from './ring-core.js';

/** One meter reading delivered by the worklet, for a single target. */
export interface WorkletMeterSnapshot {
  /** Message discriminator, always `'meter'`. */
  type: 'meter';
  /** Id of the metered target. */
  targetId: number;
  /** Sample-frame index of the reading. */
  frame: number;
  /** Left-channel peak in dBFS. */
  peakDbL: number;
  /** Right-channel peak in dBFS. */
  peakDbR: number;
  /** Left-channel RMS level in dBFS. */
  rmsDbL: number;
  /** Right-channel RMS level in dBFS. */
  rmsDbR: number;
  /** Inter-channel correlation in `[-1, 1]`. */
  correlation: number;
  /**
   * Left-channel inter-sample (true) peak in dBTP, from the ITU-R BS.1770-4 polyphase reconstruction
   * at 4x. A streaming measurement that runs one reconstruction-filter group delay (a few samples)
   * behind the block, so every value has its whole stencil and the reading does not depend on the
   * block size. Use `meteringTruePeakDbtp` over the whole signal for an exact dBTP number.
   */
  truePeakDbtpL: number;
  /** Right-channel inter-sample (true) peak in dBTP. See {@link truePeakDbtpL}. */
  truePeakDbtpR: number;
  /** Momentary loudness in LUFS. */
  momentaryLufs: number;
  /** Short-term loudness in LUFS. */
  shortTermLufs: number;
  /** Integrated loudness in LUFS. */
  integratedLufs: number;
  /** Gain reduction in dB. */
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
  numChannels?: number;
  /** Per-plane peak in dBFS (length `numChannels`), canonical WAVE order. */
  peakDb?: number[];
  /** Per-plane RMS in dBFS (length `numChannels`). */
  rmsDb?: number[];
  /** Per-plane inter-sample (true) peak in dBTP (length `numChannels`). See {@link truePeakDbtpL}. */
  truePeakDbtp?: number[];
  /** Per-plane pre-trim input peak in dBFS (length `numChannels`). */
  inputPeakDb?: number[];
}

/** One spectrum reading delivered by the worklet. */
export interface WorkletSpectrumSnapshot {
  /** Message discriminator, always `'spectrum'`. */
  type: 'spectrum';
  /** Sample-frame index of the reading. */
  frame: number;
  /** Band levels of the spectrum. */
  bands: Float32Array;
}

/** Number of Int32 header words in a meter ring. */
export const SONARE_METER_RING_HEADER_INTS = 4;

// Record layout: [frameLo, frameHi, targetId, peakDbL, peakDbR, rmsDbL, rmsDbR,
// correlation, truePeakDbtpL, truePeakDbtpR, momentaryLufs, shortTermLufs,
// integratedLufs, gainReductionDb, inputPeakDbL, inputPeakDbR, channelCount,
// peakDb[0..7], rmsDb[0..7], truePeakDbtp[0..7], inputPeakDb[0..7]].
// Header slot 3 holds SONARE_METER_RING_PROTOCOL_VERSION; older records stopped
// after inputPeakDbR (16 floats) and carried no per-plane data.
// The sample-frame index is monotonically increasing and quickly exceeds the
// 2^24 exact-integer range of a single Float32 slot (~349 s at 48 kHz), so it is
// stored split across two Float32 lanes (low 24 bits + high bits) for exact
// reconstruction. See encodeFrameLo/encodeFrameHi/decodeFrame.
/** Meter ring record-layout version, stored in header slot 3. */
export const SONARE_METER_RING_PROTOCOL_VERSION = 3;
export const SONARE_METER_RING_MAX_CHANNELS = 8;
export const SONARE_METER_RING_CHANNEL_COUNT_OFFSET = 16;
export const SONARE_METER_RING_PLANES_OFFSET = 17;
/** Number of Float32 values in one meter ring record. */
export const SONARE_METER_RING_RECORD_FLOATS =
  SONARE_METER_RING_PLANES_OFFSET + 4 * SONARE_METER_RING_MAX_CHANNELS;
/** Number of Int32 header words in a spectrum ring. */
export const SONARE_SPECTRUM_RING_HEADER_INTS = 5;
/** Spectrum ring record-layout version. */
export const SONARE_SPECTRUM_RING_PROTOCOL_VERSION = 1;

/** Shared-memory ring of meter records; create one with `createMeterRingBuffer`. */
export interface MeterRingBuffer {
  /** Backing shared memory. */
  sharedBuffer: SharedArrayBuffer;
  /** Header words, including the protocol version. */
  header: Int32Array;
  /** Record storage, one fixed-size record per slot. */
  records: Float32Array;
  /** Per-slot publish state words. */
  slotStates: Int32Array;
  /** Number of record slots. */
  capacity: number;
}

/** Meter snapshots read from a ring and the index to resume from. */
export interface MeterRingReadResult {
  /** Index to pass as `readIndex` on the next read. */
  nextReadIndex: number;
  /** Snapshots read, oldest first. */
  meters: WorkletMeterSnapshot[];
}

/** Shared-memory ring of spectrum records; create one with `createSpectrumRingBuffer`. */
export interface SpectrumRingBuffer {
  /** Backing shared memory. */
  sharedBuffer: SharedArrayBuffer;
  /** Header words, including the protocol version. */
  header: Int32Array;
  /** Record storage, one fixed-size record per slot. */
  records: Float32Array;
  /** Per-slot publish state words. */
  slotStates: Int32Array;
  /** Number of record slots. */
  capacity: number;
  /** Bands per spectrum record. */
  bands: number;
}

/** Spectrum snapshots read from a ring and the index to resume from. */
export interface SpectrumRingReadResult {
  /** Index to pass as `readIndex` on the next read. */
  nextReadIndex: number;
  /** Snapshots read, oldest first. */
  spectra: WorkletSpectrumSnapshot[];
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

/** Byte length of a meter ring. `capacity` is required; RangeError outside the domain, TypeError for an unknown key. */
export function meterRingBufferByteLength(options: Required<RingCapacityOptions>): number {
  return withEntry('meterRingBufferByteLength', () => {
    const capacity = resolveRingCapacity(options, undefined);
    return overwriteRingByteLength(
      SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
      SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT,
      capacity,
    );
  });
}

/** Creates a shared meter ring. RangeError outside the domain, TypeError for an unknown key. */
export function createMeterRingBuffer(options?: RingCapacityOptions): MeterRingBuffer {
  return withEntry('createMeterRingBuffer', () => {
    const clampedCapacity = resolveRingCapacity(options, 128);
    const sharedBuffer = new SharedArrayBuffer(
      meterRingBufferByteLength({ capacity: clampedCapacity }),
    );
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
  });
}

/** Reads the meter snapshots published since `readIndex` (default `0`) and returns them with the index to resume from. */
export function readMeterRingBuffer(ring: MeterRingBuffer, readIndex = 0): MeterRingReadResult {
  return withEntry('readMeterRingBuffer', () => {
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
      throw codedError(ErrorCode.InvalidFormat, 'meter ring has invalid descriptors.');
    }
    const writeIndex = loadSonareRingCursor(header, 0, capacity);
    const version = Atomics.load(header, 3);
    if (version === 0 && writeIndex === 0) {
      return { nextReadIndex: writeIndex, meters: [] };
    }
    if (version !== SONARE_METER_RING_PROTOCOL_VERSION) {
      throw codedError(
        ErrorCode.AbiMismatch,
        `meter ring protocol version ${version} is incompatible; expected ${SONARE_METER_RING_PROTOCOL_VERSION}.`,
      );
    }
    const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
    const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
    const meters: WorkletMeterSnapshot[] = [];
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
        const meter: WorkletMeterSnapshot = {
          type: 'meter',
          frame: decodeFrame(records[offset], records[offset + 1]),
          targetId: records[offset + 2],
          peakDbL: records[offset + 3],
          peakDbR: records[offset + 4],
          rmsDbL: records[offset + 5],
          rmsDbR: records[offset + 6],
          correlation: records[offset + 7],
          truePeakDbtpL: records[offset + 8],
          truePeakDbtpR: records[offset + 9],
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
          meter.numChannels = planes;
          meter.peakDb = plane(0);
          meter.rmsDb = plane(1);
          meter.truePeakDbtp = plane(2);
          meter.inputPeakDb = plane(3);
        }
        meters.push(meter);
      } finally {
        releaseOverwriteRead(slotStates, capacity, cursor);
      }
    }
    return { nextReadIndex, meters };
  });
}

/** Options of the spectrum ring helpers; the byte-length helper requires `capacity`. */
export interface SpectrumRingOptions {
  /** Records the ring holds, an integer of at least 1. Default 128 when creating. */
  capacity?: number;
  /** Bands per record, an integer of at least 1. Default 16. */
  bands?: number;
}

function resolveSpectrumRingOptions(
  options: unknown,
  defaultCapacity: number | undefined,
): { capacity: number; bands: number } {
  const resolved = resolveRingOptions(options, { capacity: defaultCapacity, bands: 16 });
  return {
    capacity: requireRingDimension(resolved.capacity, 'capacity', 1),
    bands: requireRingDimension(resolved.bands, 'bands', 1),
  };
}

/** Byte length of a spectrum ring. `capacity` is required; RangeError outside the domain, TypeError for an unknown key. */
export function spectrumRingBufferByteLength(
  options: SpectrumRingOptions & Required<RingCapacityOptions>,
): number {
  return withEntry('spectrumRingBufferByteLength', () => {
    const { capacity, bands } = resolveSpectrumRingOptions(options, undefined);
    // Record layout: [frameLo, frameHi, bandCount, band0, band1, ...]. frame is
    // split across two Float32 lanes for exact reconstruction beyond 2^24.
    return overwriteRingByteLength(
      SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
      (3 + bands) * Float32Array.BYTES_PER_ELEMENT,
      capacity,
    );
  });
}

/** Creates a shared spectrum ring. RangeError outside the domain, TypeError for an unknown key. */
export function createSpectrumRingBuffer(options?: SpectrumRingOptions): SpectrumRingBuffer {
  return withEntry('createSpectrumRingBuffer', () => {
    const { capacity: clampedCapacity, bands: clampedBands } = resolveSpectrumRingOptions(
      options,
      128,
    );
    const sharedBuffer = new SharedArrayBuffer(
      spectrumRingBufferByteLength({ capacity: clampedCapacity, bands: clampedBands }),
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
  });
}

/** Reads the spectrum snapshots published since `readIndex` (default `0`) and returns them with the index to resume from. */
export function readSpectrumRingBuffer(
  ring: SpectrumRingBuffer,
  readIndex = 0,
): SpectrumRingReadResult {
  return withEntry('readSpectrumRingBuffer', () => {
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
      throw codedError(ErrorCode.InvalidFormat, 'spectrum ring has invalid descriptors.');
    }
    const writeIndex = loadSonareRingCursor(header, 0, capacity);
    const version = Atomics.load(header, 4);
    if (version === 0 && writeIndex === 0) {
      return { nextReadIndex: writeIndex, spectra: [] };
    }
    if (version !== SONARE_SPECTRUM_RING_PROTOCOL_VERSION) {
      throw codedError(
        ErrorCode.AbiMismatch,
        `spectrum ring protocol version ${version} is incompatible; expected ${SONARE_SPECTRUM_RING_PROTOCOL_VERSION}.`,
      );
    }
    const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
    const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
    const spectra: WorkletSpectrumSnapshot[] = [];
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
  });
}

export function meterRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedMeterRingWriter {
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw refuse('meterSharedBuffer is too small for the ring header.');
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
    throw codedError(ErrorCode.InvalidFormat, 'meter ring has invalid descriptors.');
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
    throw refuse('spectrumSharedBuffer is too small for the ring header.');
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
    throw refuse(`spectrumBands must be an integer in [1, ${Number.MAX_SAFE_INTEGER}]`);
  }
  if (fallbackBands !== undefined && existingBands !== 0 && existingBands !== fallbackBands) {
    throw refuse('spectrumBands does not match the shared-buffer descriptor');
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
    throw codedError(ErrorCode.InvalidFormat, 'spectrum ring has invalid descriptors.');
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
export function meterFromEngine(meter: EngineMeterTelemetry): WorkletMeterSnapshot {
  const snapshot: WorkletMeterSnapshot = {
    type: 'meter',
    targetId: meter.targetId,
    frame: meter.renderFrame,
    peakDbL: meter.peakDb[0] ?? SONARE_FLOOR_DB,
    peakDbR: meter.peakDb[1] ?? SONARE_FLOOR_DB,
    rmsDbL: meter.rmsDb[0] ?? SONARE_FLOOR_DB,
    rmsDbR: meter.rmsDb[1] ?? SONARE_FLOOR_DB,
    correlation: meter.correlation,
    truePeakDbtpL: meter.truePeakDbtp[0] ?? SONARE_FLOOR_DB,
    truePeakDbtpR: meter.truePeakDbtp[1] ?? SONARE_FLOOR_DB,
    momentaryLufs: meter.momentaryLufs,
    shortTermLufs: meter.shortTermLufs,
    integratedLufs: meter.integratedLufs,
    gainReductionDb: meter.gainReductionDb,
    inputPeakDbL: meter.inputPeakDb[0] ?? SONARE_FLOOR_DB,
    inputPeakDbR: meter.inputPeakDb[1] ?? SONARE_FLOOR_DB,
  };
  if (meter.numChannels > 2) {
    snapshot.numChannels = meter.numChannels;
    snapshot.peakDb = meter.peakDb;
    snapshot.rmsDb = meter.rmsDb;
    snapshot.truePeakDbtp = meter.truePeakDbtp;
    snapshot.inputPeakDb = meter.inputPeakDb;
  }
  return snapshot;
}

export function magnitudeToDb(value: number): number {
  return value > 1.0e-12 ? 20 * Math.log10(value) : SONARE_FLOOR_DB;
}
