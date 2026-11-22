import { ErrorCode } from '../errors.js';
import type { EngineTelemetry } from '../index.js';
import { codedError, refuse, withEntry } from '../internal/entry.js';
import {
  attachProtocolVersion,
  commitOverwriteWrite,
  loadSonareRingCursor,
  makeOverwriteSlotStates,
  overwriteRingByteLength,
  overwriteRingFirstReadable,
  type RingCapacityOptions,
  readInt64Words,
  recordOffset,
  releaseOverwriteRead,
  requirePositiveCapacity,
  requireProtocolVersion,
  resolveRingCapacity,
  ringCursorSpan,
  sonareRingCursorDistance,
  toSafeInteger,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
  validateOverwriteViews,
  writePreparedInt64Words,
} from './ring-core.js';

/** Base for splitting a frame index into two exactly-representable Float32 lanes. */
const SONARE_FRAME_LANE_BASE = 0x1000000; // 2^24

/** Low 24 bits of a frame index (exact in Float32). */
export function encodeFrameLo(frame: number): number {
  const f = Math.max(0, Math.floor(frame));
  return f % SONARE_FRAME_LANE_BASE;
}

/** High bits of a frame index above 2^24 (exact in Float32 up to ~2^48). */
export function encodeFrameHi(frame: number): number {
  const f = Math.max(0, Math.floor(frame));
  return Math.floor(f / SONARE_FRAME_LANE_BASE);
}

/** Reconstruct a frame index from its low/high Float32 lanes. */
export function decodeFrame(lo: number, hi: number): number {
  return hi * SONARE_FRAME_LANE_BASE + lo;
}
/**
 * Int32 words in an engine ring header. Telemetry rings lay them out as
 * `[writeIndex, protocolVersion, capacity, recordBytes, lost]`; command rings use slot 1 as
 * their read cursor.
 */
export const SONARE_ENGINE_RING_HEADER_INTS = 5;

/** Highest PPQ the public transport accepts; mirrors the core's `kMaxPublicPpq`. */
export const SONARE_MAX_PUBLIC_PPQ = 1e12;
/** Bytes in one telemetry record slot of the ring. */
export const SONARE_ENGINE_TELEMETRY_RECORD_BYTES = 48;
/**
 * Telemetry ring protocol version, kept in header slot 1. A ring carrying another non-zero version
 * is refused with `SonareError` code `AbiMismatch`.
 */
export const SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION = 1;
// Engine telemetry uses header [writeIndex, protocolVersion, capacity,
// recordBytes, lost]. Its record area is followed by one slot-state word per
// record and one publicationWrapped latch. Command rings continue to use
// header slot 1 as their read cursor and have no appended overwrite metadata.

/** Kind of a telemetry record: one per rendered block, or an error report. */
export enum SonareEngineTelemetryType {
  ProcessBlock = 0,
  Error = 1,
}

/**
 * Error ordinals of an `Error` telemetry record, matching the C ABI `SonareEngineTelemetryError`.
 * Ordinal 19 (`InvalidCommand`) is raised by the worklet itself for a command it could not apply;
 * the rest come from the core.
 */
export enum SonareEngineTelemetryError {
  None = 0,
  CommandQueueOverflow = 1,
  PendingCommandOverflow = 2,
  BoundaryOverflow = 3,
  TelemetryOverflow = 4,
  CaptureOverflow = 5,
  MaxBlockExceeded = 6,
  UnknownTarget = 7,
  NonRealtimeSafeParameter = 8,
  NotPrepared = 9,
  NonQueueableCommand = 10,
  AutomationBindTargetOverflow = 11,
  StaleAutomationLanes = 12,
  SmoothedParameterCapacity = 13,
  CommandBacklogDeferred = 14,
  ClipPageUnderrun = 15,
  InsertAutomationOverflow = 16,
  MidiClockOverflow = 17,
  MetronomeOverflow = 18,
  InvalidCommand = 19,
  MaxChannelsExceeded = 20,
  ParameterBaseOverflow = 21,
}

/** One engine telemetry record as stored in the ring; mirrors the C ABI `SonareEngineTelemetry`. */
export interface EngineTelemetryRecord {
  /** Record kind, a {@link SonareEngineTelemetryType} ordinal. */
  type: SonareEngineTelemetryType | number;
  /** Error ordinal, a {@link SonareEngineTelemetryError}; `None` (0) on a block record. */
  error: SonareEngineTelemetryError | number;
  /** Render frame the record refers to, in frames since the engine started rendering. */
  renderFrame: number;
  /** Transport position at the end of the block, in samples. */
  timelineSample: number;
  /** `timelineSample` corrected for the graph latency: the position being heard, in samples. */
  audibleTimelineSample: number;
  /** Graph processing latency, in samples. */
  graphLatencySamples: number;
  /** Frames in the block for a block record; for an error record a count or id that depends on `error`. */
  value: number;
}

/** Shared-memory overwrite ring of engine telemetry records; create one with `createEngineTelemetryRingBuffer`. */
export interface EngineTelemetryRingBuffer {
  /** Backing shared memory. */
  sharedBuffer: SharedArrayBuffer;
  /** Header words `[writeIndex, protocolVersion, capacity, recordBytes, lost]`. */
  header: Int32Array;
  /** Record area, `capacity` slots of 48 bytes each. */
  view: DataView;
  /** One publish-state word per record slot, guarding readers against a record still being written. */
  slotStates: Int32Array;
  /** One-word latch set once the ring has overwritten a record that was already published. */
  publicationWrapped: Int32Array;
  /** Number of record slots. */
  capacity: number;
}

/** Telemetry records drained from a ring and the index to resume from. */
export interface EngineTelemetryRingReadResult {
  /** Index to pass as `readIndex` on the next read. */
  nextReadIndex: number;
  /** Records read, oldest first. */
  telemetry: EngineTelemetryRecord[];
}

function makeEngineTelemetryPublicationWrapped(
  sharedBuffer: SharedArrayBuffer,
  capacity: number,
): Int32Array {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const recordBytes = SONARE_ENGINE_TELEMETRY_RECORD_BYTES;
  const stateOffset = headerBytes + recordBytes * capacity;
  return new Int32Array(sharedBuffer, stateOffset + capacity * Int32Array.BYTES_PER_ELEMENT, 1);
}

function validateEngineTelemetryPublicationWrapped(
  sharedBuffer: SharedArrayBuffer,
  publicationWrapped: Int32Array,
  capacity: number,
): void {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const stateOffset =
    headerBytes +
    SONARE_ENGINE_TELEMETRY_RECORD_BYTES * capacity +
    capacity * Int32Array.BYTES_PER_ELEMENT;
  if (
    publicationWrapped.buffer !== sharedBuffer ||
    publicationWrapped.byteOffset !== stateOffset ||
    publicationWrapped.byteLength !== Int32Array.BYTES_PER_ELEMENT ||
    publicationWrapped.length !== 1
  ) {
    throw codedError(
      ErrorCode.InvalidFormat,
      'engine telemetry ring has an invalid publicationWrapped view.',
    );
  }
  const value = Atomics.load(publicationWrapped, 0);
  if (value !== 0 && value !== 1) {
    throw codedError(
      ErrorCode.InvalidFormat,
      'engine telemetry ring has an invalid publicationWrapped value.',
    );
  }
}

/** Byte length of the ring. `capacity` is required; RangeError outside the domain, TypeError for an unknown key. */
export function engineTelemetryRingBufferByteLength(
  options: Required<RingCapacityOptions>,
): number {
  return withEntry('engineTelemetryRingBufferByteLength', () => {
    const clampedCapacity = resolveRingCapacity(options, undefined);
    return (
      overwriteRingByteLength(
        SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
        SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
        clampedCapacity,
      ) + Int32Array.BYTES_PER_ELEMENT
    );
  });
}

/** Creates a shared engine telemetry ring (default `capacity` 128). RangeError outside the domain, TypeError for an unknown key. */
export function createEngineTelemetryRingBuffer(
  options?: RingCapacityOptions,
): EngineTelemetryRingBuffer {
  return withEntry('createEngineTelemetryRingBuffer', () => {
    const clampedCapacity = resolveRingCapacity(options, 128);
    const sharedBuffer = new SharedArrayBuffer(
      engineTelemetryRingBufferByteLength({ capacity: clampedCapacity }),
    );
    const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
    const header = new Int32Array(sharedBuffer, 0, SONARE_ENGINE_RING_HEADER_INTS);
    const view = new DataView(
      sharedBuffer,
      headerBytes,
      clampedCapacity * SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
    );
    const slotStates = makeOverwriteSlotStates(
      sharedBuffer,
      headerBytes,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      clampedCapacity,
    );
    const publicationWrapped = makeEngineTelemetryPublicationWrapped(sharedBuffer, clampedCapacity);
    Atomics.store(header, 0, 0);
    Atomics.store(header, 1, 0);
    Atomics.store(header, 2, clampedCapacity);
    Atomics.store(header, 3, SONARE_ENGINE_TELEMETRY_RECORD_BYTES);
    Atomics.store(header, 4, 0);
    slotStates.fill(0);
    Atomics.store(publicationWrapped, 0, 0);
    return {
      sharedBuffer,
      header,
      view,
      slotStates,
      publicationWrapped,
      capacity: clampedCapacity,
    };
  });
}

export function writeSonareEngineTelemetryRingBuffer(
  ring: EngineTelemetryRingBuffer,
  telemetry: EngineTelemetryRecord,
): void {
  // Capture the complete mapping once. In particular, do not validate one
  // getter-backed mapping and then let a later helper read another mapping.
  const sharedBuffer = ring.sharedBuffer;
  const header = ring.header;
  const view = ring.view;
  const slotStates = ring.slotStates;
  const publicationWrapped = ring.publicationWrapped;
  const capacity = ring.capacity;
  ensureEngineTelemetryRingAttached(
    sharedBuffer,
    header,
    view,
    slotStates,
    publicationWrapped,
    capacity,
  );
  // Read and coerce every source value before selecting a slot. Accessor
  // re-entry and all checked 64-bit conversions therefore finish while the
  // previous publication is still intact.
  const type = +telemetry.type;
  const error = +telemetry.error;
  const renderFrame = toSafeInteger(telemetry.renderFrame, 0);
  const timelineSample = toSafeInteger(telemetry.timelineSample, 0);
  const audibleTimelineSample = toSafeInteger(telemetry.audibleTimelineSample, 0);
  const graphLatencySamplesQ8 = Math.round(+telemetry.graphLatencySamples * 256);
  const value = +telemetry.value;
  const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
  if (writeIndex === undefined) {
    Atomics.add(header, 4, 1);
    return;
  }
  const offset = recordOffset(writeIndex, capacity, SONARE_ENGINE_TELEMETRY_RECORD_BYTES);
  writePreparedEngineTelemetryRecord(
    view,
    offset,
    type,
    error,
    renderFrame,
    timelineSample,
    audibleTimelineSample,
    graphLatencySamplesQ8,
    value,
  );
  commitSonareEngineTelemetryRingWrite(
    header,
    slotStates,
    publicationWrapped,
    capacity,
    writeIndex,
  );
}

/**
 * Commits telemetry and counts both collision drops and physical overwrites.
 * `publicationWrapped` is the separate monotonic latch: it prevents prior
 * collision drops from being mistaken for a physical-wrap history bit.
 */
export function commitSonareEngineTelemetryRingWrite(
  ring: Pick<
    EngineTelemetryRingBuffer,
    'header' | 'slotStates' | 'publicationWrapped' | 'capacity'
  >,
  writeIndex: number,
): void;
export function commitSonareEngineTelemetryRingWrite(
  header: Int32Array,
  slotStates: Int32Array,
  publicationWrapped: Int32Array,
  capacity: number,
  writeIndex: number,
): void;
export function commitSonareEngineTelemetryRingWrite(
  ringOrHeader:
    | Pick<EngineTelemetryRingBuffer, 'header' | 'slotStates' | 'publicationWrapped' | 'capacity'>
    | Int32Array,
  slotStatesOrWriteIndex: Int32Array | number,
  publicationWrappedArgument?: Int32Array,
  capacityArgument?: number,
  writeIndexArgument?: number,
): void {
  const scalarForm = writeIndexArgument !== undefined;
  const header = scalarForm
    ? (ringOrHeader as Int32Array)
    : (ringOrHeader as Pick<EngineTelemetryRingBuffer, 'header'>).header;
  const slotStates = scalarForm
    ? (slotStatesOrWriteIndex as Int32Array)
    : (ringOrHeader as Pick<EngineTelemetryRingBuffer, 'slotStates'>).slotStates;
  const publicationWrapped = scalarForm
    ? (publicationWrappedArgument as Int32Array)
    : (ringOrHeader as Pick<EngineTelemetryRingBuffer, 'publicationWrapped'>).publicationWrapped;
  const capacity = scalarForm
    ? (capacityArgument as number)
    : (ringOrHeader as Pick<EngineTelemetryRingBuffer, 'capacity'>).capacity;
  const writeIndex = scalarForm
    ? (writeIndexArgument as number)
    : (slotStatesOrWriteIndex as number);
  const hadPhysicalOverwrite = Atomics.load(publicationWrapped, 0) !== 0;
  const overwritesPublishedRecord = hadPhysicalOverwrite || writeIndex >= capacity;
  if (!hadPhysicalOverwrite && writeIndex >= capacity) {
    Atomics.store(publicationWrapped, 0, 1);
  }
  commitOverwriteWrite(header, slotStates, capacity, writeIndex);
  if (overwritesPublishedRecord) {
    Atomics.add(header, 4, 1);
  }
}

/**
 * Drains the records published since `readIndex`, oldest first. A reader that fell more than
 * `capacity` records behind resumes at the oldest record still held.
 *
 * @param ring Ring to read.
 * @param readIndex Index returned as `nextReadIndex` by the previous read. Default 0.
 * @throws SonareError `InvalidFormat` for views or descriptors that do not match the ring layout,
 * and `AbiMismatch` for a different protocol version.
 */
export function readEngineTelemetryRingBuffer(
  ring: EngineTelemetryRingBuffer,
  readIndex = 0,
): EngineTelemetryRingReadResult {
  return withEntry('readEngineTelemetryRingBuffer', () => {
    const { sharedBuffer, header, view, slotStates, publicationWrapped, capacity } = ring;
    const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
    validateOverwriteViews(
      sharedBuffer,
      header,
      view,
      slotStates,
      headerBytes,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      capacity,
      'engine telemetry ring',
      Int32Array.BYTES_PER_ELEMENT,
    );
    validateEngineTelemetryPublicationWrapped(sharedBuffer, publicationWrapped, capacity);
    if (
      Atomics.load(header, 2) !== capacity ||
      Atomics.load(header, 3) !== SONARE_ENGINE_TELEMETRY_RECORD_BYTES
    ) {
      throw codedError(ErrorCode.InvalidFormat, 'engine telemetry ring has invalid descriptors.');
    }
    const writeIndex = loadSonareRingCursor(header, 0, capacity);
    const version = Atomics.load(header, 1);
    if (version === 0 && writeIndex === 0) {
      return { nextReadIndex: writeIndex, telemetry: [] };
    }
    if (version !== SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION) {
      throw codedError(
        ErrorCode.AbiMismatch,
        `engine telemetry ring protocol version ${version} is incompatible; expected ${SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION}.`,
      );
    }
    const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
    const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
    const telemetry: EngineTelemetryRecord[] = [];
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
        telemetry.push(
          readEngineTelemetryRecord(
            view,
            recordOffset(cursor, capacity, SONARE_ENGINE_TELEMETRY_RECORD_BYTES),
          ),
        );
      } finally {
        releaseOverwriteRead(slotStates, capacity, cursor);
      }
    }
    return { nextReadIndex, telemetry };
  });
}

export function engineRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  recordBytes: number,
  fallbackCapacity?: number,
): { header: Int32Array; view: DataView; capacity: number } {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_ENGINE_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'commandRingCapacity',
  );
  const minBytes = headerBytes + capacity * recordBytes;
  if (sharedBuffer.byteLength < minBytes) {
    throw refuse('engine SharedArrayBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 2, capacity);
  Atomics.store(header, 3, recordBytes);
  return {
    header,
    view: new DataView(sharedBuffer, headerBytes, capacity * recordBytes),
    capacity,
  };
}

/** Maps and acknowledges the overwrite telemetry layout (engine slot 1 is version). */
export function engineTelemetryRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): EngineTelemetryRingBuffer {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw refuse('telemetrySharedBuffer is too small for the ring header.');
  }
  const header = new Int32Array(sharedBuffer, 0, SONARE_ENGINE_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'telemetryRingCapacity',
  );
  const view = new DataView(
    sharedBuffer,
    headerBytes,
    capacity * SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  );
  const slotStates = makeOverwriteSlotStates(
    sharedBuffer,
    headerBytes,
    SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
    capacity,
  );
  const publicationWrapped = makeEngineTelemetryPublicationWrapped(sharedBuffer, capacity);
  validateOverwriteViews(
    sharedBuffer,
    header,
    view,
    slotStates,
    headerBytes,
    SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
    capacity,
    'engine telemetry ring',
    Int32Array.BYTES_PER_ELEMENT,
  );
  validateEngineTelemetryPublicationWrapped(sharedBuffer, publicationWrapped, capacity);
  if (
    Atomics.load(header, 2) !== capacity ||
    Atomics.load(header, 3) !== SONARE_ENGINE_TELEMETRY_RECORD_BYTES
  ) {
    throw codedError(ErrorCode.InvalidFormat, 'engine telemetry ring has invalid descriptors.');
  }
  requireProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
  if (Atomics.load(header, 1) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw codedError(
      ErrorCode.InvalidFormat,
      'engine telemetry ring has records without a compatible producer.',
    );
  }
  attachProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
  return { sharedBuffer, header, view, slotStates, publicationWrapped, capacity };
}

function ensureEngineTelemetryRingAttached(
  sharedBuffer: SharedArrayBuffer,
  header: Int32Array,
  view: DataView,
  slotStates: Int32Array,
  publicationWrapped: Int32Array,
  capacity: number,
): void {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  validateOverwriteViews(
    sharedBuffer,
    header,
    view,
    slotStates,
    headerBytes,
    SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
    capacity,
    'engine telemetry ring',
    Int32Array.BYTES_PER_ELEMENT,
  );
  validateEngineTelemetryPublicationWrapped(sharedBuffer, publicationWrapped, capacity);
  if (
    Atomics.load(header, 2) !== capacity ||
    Atomics.load(header, 3) !== SONARE_ENGINE_TELEMETRY_RECORD_BYTES
  ) {
    throw codedError(ErrorCode.InvalidFormat, 'engine telemetry ring has invalid descriptors.');
  }
  requireProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
  if (Atomics.load(header, 1) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw codedError(
      ErrorCode.InvalidFormat,
      'engine telemetry ring has records without a compatible producer.',
    );
  }
  attachProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
}

function writePreparedEngineTelemetryRecord(
  view: DataView,
  offset: number,
  type: number,
  error: number,
  renderFrame: number,
  timelineSample: number,
  audibleTimelineSample: number,
  graphLatencySamplesQ8: number,
  value: number,
): void {
  view.setUint32(offset, type, true);
  view.setUint32(offset + 4, error, true);
  writePreparedInt64Words(view, offset + 8, renderFrame);
  writePreparedInt64Words(view, offset + 16, timelineSample);
  writePreparedInt64Words(view, offset + 24, audibleTimelineSample);
  view.setInt32(offset + 32, graphLatencySamplesQ8, true);
  view.setUint32(offset + 36, value, true);
  view.setUint32(offset + 40, 0, true);
  view.setUint32(offset + 44, 0, true);
}

function readEngineTelemetryRecord(view: DataView, offset: number): EngineTelemetryRecord {
  return {
    type: view.getUint32(offset, true),
    error: view.getUint32(offset + 4, true),
    renderFrame: readInt64Words(view, offset + 8),
    timelineSample: readInt64Words(view, offset + 16),
    audibleTimelineSample: readInt64Words(view, offset + 24),
    graphLatencySamples: view.getInt32(offset + 32, true) / 256,
    value: view.getUint32(offset + 36, true),
  };
}

function upperFirst(name: string): string {
  return name.charAt(0).toUpperCase() + name.slice(1);
}

/** Converts a drained {@link EngineTelemetry} back to the wire record (ordinals). */
export function telemetryFromEngine(telemetry: EngineTelemetry): EngineTelemetryRecord {
  return {
    type: SonareEngineTelemetryType[
      upperFirst(telemetry.type) as keyof typeof SonareEngineTelemetryType
    ],
    error:
      SonareEngineTelemetryError[
        upperFirst(telemetry.error) as keyof typeof SonareEngineTelemetryError
      ],
    renderFrame: telemetry.renderFrame,
    timelineSample: telemetry.timelineSample,
    audibleTimelineSample: telemetry.audibleTimelineSample,
    graphLatencySamples: telemetry.graphLatencySamples,
    value: telemetry.value,
  };
}
