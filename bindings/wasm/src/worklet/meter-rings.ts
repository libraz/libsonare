import { ErrorCode } from '../errors.js';
import type { EngineMeterTelemetry } from '../index.js';
import { codedError, refuse, withEntry } from '../internal/entry.js';
import { measured } from '../internal/metering_native.js';
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
  /** Inter-channel correlation in `[-1, 1]`. Absent when not measured (a channel has zero energy). */
  correlation?: number;
  /** Momentary loudness in LUFS. `-Infinity` for silence; absent until the window has filled or when not measured. */
  momentaryLufs?: number;
  /** Short-term loudness in LUFS. `-Infinity` for silence; absent until the window has filled or when not measured. */
  shortTermLufs?: number;
  /** Integrated loudness in LUFS. Absent before the first gating block or when not measured; `-Infinity` while nothing passes the gate. */
  integratedLufs?: number;
  /** Gain reduction in dB. Absent when not measured (the mixer worklet runs no dynamics metering). */
  gainReductionDb?: number;
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
   * Number of metered planes in this snapshot, equal to the length of each per-plane array.
   */
  numChannels: number;
  /** Per-plane peak in dBFS (length `numChannels`); `undefined` when not measured. */
  peakDb: (number | undefined)[];
  /** Per-plane RMS in dBFS (length `numChannels`); `undefined` when not measured. */
  rmsDb: (number | undefined)[];
  /** Per-plane inter-sample (true) peak in dBTP (length `numChannels`); `undefined` when not measured. */
  truePeakDbtp: (number | undefined)[];
  /** Per-plane pre-trim input peak in dBFS (length `numChannels`); `undefined` when not measured. */
  inputPeakDb: (number | undefined)[];
}

/** One spectrum reading delivered by the worklet. */
export interface WorkletSpectrumSnapshot {
  /** Message discriminator, always `'spectrum'`. */
  type: 'spectrum';
  /** Sample-frame index of the reading. */
  frame: number;
  /** Band levels of the spectrum, in dB. `-Infinity` for a band of zero power; NaN for a band no bin reaches. */
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
const METER_SCALAR_PLANE_OFFSETS = [3, 5, 8, 14] as const;
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
        const reportedChannels = records[offset + SONARE_METER_RING_CHANNEL_COUNT_OFFSET];
        if (
          !Number.isFinite(reportedChannels) ||
          !Number.isInteger(reportedChannels) ||
          reportedChannels < 0
        ) {
          throw codedError(
            ErrorCode.InvalidFormat,
            'meter ring record has an invalid channel count.',
          );
        }
        const numChannels =
          reportedChannels === 0 ? 2 : Math.min(reportedChannels, SONARE_METER_RING_MAX_CHANNELS);
        const plane = (kind: 0 | 1 | 2 | 3): (number | undefined)[] =>
          Array.from({ length: numChannels }, (_, channel) => {
            const value =
              numChannels > 2
                ? records[
                    offset +
                      SONARE_METER_RING_PLANES_OFFSET +
                      kind * SONARE_METER_RING_MAX_CHANNELS +
                      channel
                  ]
                : records[offset + METER_SCALAR_PLANE_OFFSETS[kind] + channel];
            return measured(value);
          });
        const meter: WorkletMeterSnapshot = {
          type: 'meter',
          frame: decodeFrame(records[offset], records[offset + 1]),
          targetId: records[offset + 2],
          numChannels,
          peakDb: plane(0),
          rmsDb: plane(1),
          truePeakDbtp: plane(2),
          inputPeakDb: plane(3),
          ...omitNaN({
            correlation: records[offset + 7],
            momentaryLufs: records[offset + 10],
            shortTermLufs: records[offset + 11],
            integratedLufs: records[offset + 12],
            gainReductionDb: records[offset + 13],
          }),
        };
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

/** Keeps the entries whose value is not NaN: a scalar that was not measured is an absent member. */
export function omitNaN<K extends string>(fields: Record<K, number>): Partial<Record<K, number>> {
  const out: Partial<Record<K, number>> = {};
  for (const key of Object.keys(fields) as K[]) {
    if (!Number.isNaN(fields[key])) {
      out[key] = fields[key];
    }
  }
  return out;
}

// The arrays hold numChannels planes; an unmeasured plane is undefined.
export function meterFromEngine(meter: EngineMeterTelemetry): WorkletMeterSnapshot {
  const snapshot: WorkletMeterSnapshot = {
    type: 'meter',
    targetId: meter.targetId,
    frame: meter.renderFrame,
    ...omitNaN({
      correlation: meter.correlation ?? Number.NaN,
      momentaryLufs: meter.momentaryLufs ?? Number.NaN,
      shortTermLufs: meter.shortTermLufs ?? Number.NaN,
      integratedLufs: meter.integratedLufs ?? Number.NaN,
    }),
    gainReductionDb: meter.gainReductionDb,
    numChannels: meter.numChannels,
    peakDb: meter.peakDb,
    rmsDb: meter.rmsDb,
    truePeakDbtp: meter.truePeakDbtp,
    inputPeakDb: meter.inputPeakDb,
  };
  return snapshot;
}

export function magnitudeToDb(value: number): number {
  return value > 0 ? 20 * Math.log10(value) : Number.NEGATIVE_INFINITY;
}
