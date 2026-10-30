import type { EngineTelemetry } from '../index.js';
import {
  attachProtocolVersion,
  commitOverwriteWrite,
  loadSonareRingCursor,
  makeOverwriteSlotStates,
  overwriteRingByteLength,
  overwriteRingFirstReadable,
  readInt64Words,
  recordOffset,
  releaseOverwriteRead,
  requirePositiveCapacity,
  requireProtocolVersion,
  requireRingDimension,
  ringCursorSpan,
  sonareRingCursorDistance,
  storeSonareRingCursor,
  toSafeInteger,
  toUint32Slot,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
  validateOverwriteViews,
  writeInt64Words,
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
export const SONARE_ENGINE_RING_HEADER_INTS = 5;
export const SONARE_ENGINE_COMMAND_RECORD_BYTES = 40;

/** Highest PPQ the public transport accepts; mirrors the core's `kMaxPublicPpq`. */
export const SONARE_MAX_PUBLIC_PPQ = 1e12;
export const SONARE_ENGINE_TELEMETRY_RECORD_BYTES = 48;
export const SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION = 1;
// Engine telemetry uses header [writeIndex, protocolVersion, capacity,
// recordBytes, lost]. Its record area is followed by one slot-state word per
// record and one publicationWrapped latch. Command rings continue to use
// header slot 1 as their read cursor and have no appended overwrite metadata.

export enum SonareEngineCommandType {
  SetParam = 0,
  SetParamSmoothed = 1,
  TransportPlay = 2,
  TransportStop = 3,
  TransportSeekSample = 4,
  TransportSeekPpq = 5,
  SetTempoMap = 6,
  SetLoop = 7,
  SwapGraph = 8,
  SwapAutomation = 9,
  SetSoloMute = 10,
  AddClip = 11,
  RemoveClip = 12,
  ArmRecord = 13,
  Punch = 14,
  SetMetronome = 15,
  SetMarker = 16,
  SeekMarker = 17,
  // Queueable lane monitor-mode transition. The worklet command vocabulary
  // deliberately leaves 18..25 unused; this is the stable ABI-v3 ordinal.
  SetTrackMonitorMode = 26,
  // Queueable master-meter integrated-loudness reset. Numeric value 28 mirrors
  // rt::CommandType::kResetMasterMeterIntegrated; 27 remains the appended MIDI
  // UMP slot command in the native vocabulary.
  ResetMasterLoudnessMeter = 28,
  // Queueable mixer/effect processing-state reset. Numeric value 29 mirrors
  // rt::CommandType::kResetProcessorState.
  ResetProcessorState = 29,
}

export enum SonareEngineTelemetryType {
  ProcessBlock = 0,
  Error = 1,
}

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

export interface SonareEngineCommandRecord {
  type: SonareEngineCommandType | number;
  targetId?: number;
  sampleTime?: number | bigint;
  argFloat?: number;
  argInt?: number | bigint;
  /**
   * A second full-precision double, carried in the 8-byte lane `argInt` uses by
   * the command types that need one (`SetLoop`, for its end PPQ); those types
   * carry no `argInt`.
   */
  argFloat2?: number;
  /**
   * Set by the node on the SAB ring: the sequence number of the last control
   * sync posted before this command, which the worklet applies first.
   */
  syncSequence?: number;
}

export interface SonareEngineTelemetryRecord {
  type: SonareEngineTelemetryType | number;
  error: SonareEngineTelemetryError | number;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}

export interface SonareEngineCommandRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  view: DataView;
  capacity: number;
}

export interface SonareEngineTelemetryRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  view: DataView;
  slotStates: Int32Array;
  publicationWrapped: Int32Array;
  capacity: number;
}

export interface SonareEngineTelemetryRingReadResult {
  nextReadIndex: number;
  telemetry: SonareEngineTelemetryRecord[];
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
    throw new Error('engine telemetry ring has an invalid publicationWrapped view.');
  }
  const value = Atomics.load(publicationWrapped, 0);
  if (value !== 0 && value !== 1) {
    throw new Error('engine telemetry ring has an invalid publicationWrapped value.');
  }
}

export function sonareEngineCommandRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return (
    SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_ENGINE_COMMAND_RECORD_BYTES
  );
}

export function sonareEngineTelemetryRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return (
    overwriteRingByteLength(
      SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      clampedCapacity,
    ) + Int32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareEngineCommandRingBuffer(capacity = 128): SonareEngineCommandRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareEngineCommandRingBufferByteLength(clampedCapacity),
  );
  const ring = engineRingFromSharedBuffer(
    sharedBuffer,
    SONARE_ENGINE_COMMAND_RECORD_BYTES,
    clampedCapacity,
  );
  return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
}

export function createSonareEngineTelemetryRingBuffer(
  capacity = 128,
): SonareEngineTelemetryRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareEngineTelemetryRingBufferByteLength(clampedCapacity),
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
}

export function pushSonareEngineCommandRingBuffer(
  ring: SonareEngineCommandRingBuffer,
  command: SonareEngineCommandRecord,
): boolean {
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  if (sonareRingCursorDistance(readIndex, writeIndex, ring.capacity) >= ring.capacity) {
    Atomics.add(ring.header, 4, 1);
    return false;
  }
  writeEngineCommandRecord(
    ring.view,
    recordOffset(writeIndex, ring.capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES),
    command,
  );
  storeSonareRingCursor(ring.header, 0, writeIndex, ring.capacity);
  return true;
}

export function popSonareEngineCommandRingBuffer(
  ring: SonareEngineCommandRingBuffer,
): SonareEngineCommandRecord | null {
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  if (readIndex === writeIndex) {
    return null;
  }
  const command = readEngineCommandRecord(
    ring.view,
    recordOffset(readIndex, ring.capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES),
  );
  storeSonareRingCursor(ring.header, 1, readIndex, ring.capacity);
  return command;
}

/** Whether one more command fits the ring now. */
export function sonareEngineCommandRingHasRoom(ring: SonareEngineCommandRingBuffer): boolean {
  return pendingSonareEngineCommandCount(ring) < ring.capacity;
}

/** Commands published to the ring and not yet popped. */
export function pendingSonareEngineCommandCount(ring: SonareEngineCommandRingBuffer): number {
  return Math.min(
    ring.capacity,
    sonareRingCursorDistance(
      loadSonareRingCursor(ring.header, 1, ring.capacity),
      loadSonareRingCursor(ring.header, 0, ring.capacity),
      ring.capacity,
    ),
  );
}

export function writeSonareEngineTelemetryRingBuffer(
  ring: SonareEngineTelemetryRingBuffer,
  telemetry: SonareEngineTelemetryRecord,
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
  const graphLatencySamplesQ8 = +telemetry.graphLatencySamplesQ8;
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
    SonareEngineTelemetryRingBuffer,
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
    | Pick<
        SonareEngineTelemetryRingBuffer,
        'header' | 'slotStates' | 'publicationWrapped' | 'capacity'
      >
    | Int32Array,
  slotStatesOrWriteIndex: Int32Array | number,
  publicationWrappedArgument?: Int32Array,
  capacityArgument?: number,
  writeIndexArgument?: number,
): void {
  const scalarForm = writeIndexArgument !== undefined;
  const header = scalarForm
    ? (ringOrHeader as Int32Array)
    : (ringOrHeader as Pick<SonareEngineTelemetryRingBuffer, 'header'>).header;
  const slotStates = scalarForm
    ? (slotStatesOrWriteIndex as Int32Array)
    : (ringOrHeader as Pick<SonareEngineTelemetryRingBuffer, 'slotStates'>).slotStates;
  const publicationWrapped = scalarForm
    ? (publicationWrappedArgument as Int32Array)
    : (ringOrHeader as Pick<SonareEngineTelemetryRingBuffer, 'publicationWrapped'>)
        .publicationWrapped;
  const capacity = scalarForm
    ? (capacityArgument as number)
    : (ringOrHeader as Pick<SonareEngineTelemetryRingBuffer, 'capacity'>).capacity;
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

export function readSonareEngineTelemetryRingBuffer(
  ring: SonareEngineTelemetryRingBuffer,
  readIndex = 0,
): SonareEngineTelemetryRingReadResult {
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
    throw new Error('engine telemetry ring has invalid descriptors.');
  }
  const writeIndex = loadSonareRingCursor(header, 0, capacity);
  const version = Atomics.load(header, 1);
  if (version === 0 && writeIndex === 0) {
    return { nextReadIndex: writeIndex, telemetry: [] };
  }
  if (version !== SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION) {
    throw new Error(
      `engine telemetry ring protocol version ${version} is incompatible; expected ${SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION}.`,
    );
  }
  const firstReadable = overwriteRingFirstReadable(readIndex, writeIndex, capacity);
  const count = sonareRingCursorDistance(firstReadable, writeIndex, capacity);
  const telemetry: SonareEngineTelemetryRecord[] = [];
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
    throw new Error('engine SharedArrayBuffer is too small for the requested ring capacity.');
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
): SonareEngineTelemetryRingBuffer {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  if (sharedBuffer.byteLength < headerBytes) {
    throw new Error('telemetrySharedBuffer is too small for the ring header.');
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
    throw new Error('engine telemetry ring has invalid descriptors.');
  }
  requireProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
  if (Atomics.load(header, 1) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw new Error('engine telemetry ring has records without a compatible producer.');
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
    throw new Error('engine telemetry ring has invalid descriptors.');
  }
  requireProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
  if (Atomics.load(header, 1) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw new Error('engine telemetry ring has records without a compatible producer.');
  }
  attachProtocolVersion(
    header,
    1,
    SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
    'engine telemetry ring',
  );
}

function writeEngineCommandRecord(
  view: DataView,
  offset: number,
  command: SonareEngineCommandRecord,
): void {
  // Both slots are uint32 and the readers are unsigned, so the writer holds
  // them to that domain; the command vocabulary itself is the node's check.
  view.setUint32(offset, toUint32Slot(command.type, 0, 'type'), true);
  view.setUint32(offset + 4, toUint32Slot(command.targetId, 0, 'targetId'), true);
  writeInt64Words(view, offset + 8, toSafeInteger(command.sampleTime, -1));
  // argFloat occupies a full 8-byte Float64 slot (replacing the old Float32 +
  // 4-byte pad) so PPQ scalars carried here keep full double precision over the
  // SAB transport, matching the engine's double-precision seek/loop contract.
  view.setFloat64(offset + 16, command.argFloat ?? 0, true);
  if (command.type === SonareEngineCommandType.SetLoop) {
    view.setFloat64(offset + 24, command.argFloat2 ?? 0, true);
  } else {
    writeInt64Words(view, offset + 24, toSafeInteger(command.argInt, 0));
  }
  view.setUint32(offset + 32, toUint32Slot(command.syncSequence, 0, 'syncSequence'), true);
  view.setUint32(offset + 36, 0, true);
}

function readEngineCommandRecord(view: DataView, offset: number): SonareEngineCommandRecord {
  const type = view.getUint32(offset, true);
  const record: SonareEngineCommandRecord = {
    type,
    targetId: view.getUint32(offset + 4, true),
    sampleTime: readInt64Words(view, offset + 8),
    argFloat: view.getFloat64(offset + 16, true),
  };
  // The shared 8-byte lane holds a double only for the command types that write one there.
  if (type === SonareEngineCommandType.SetLoop) {
    record.argFloat2 = view.getFloat64(offset + 24, true);
  } else {
    record.argInt = readInt64Words(view, offset + 24);
  }
  record.syncSequence = view.getUint32(offset + 32, true);
  return record;
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

function readEngineTelemetryRecord(view: DataView, offset: number): SonareEngineTelemetryRecord {
  return {
    type: view.getUint32(offset, true),
    error: view.getUint32(offset + 4, true),
    renderFrame: readInt64Words(view, offset + 8),
    timelineSample: readInt64Words(view, offset + 16),
    audibleTimelineSample: readInt64Words(view, offset + 24),
    graphLatencySamplesQ8: view.getInt32(offset + 32, true),
    value: view.getUint32(offset + 36, true),
  };
}

export function telemetryFromEngine(telemetry: EngineTelemetry): SonareEngineTelemetryRecord {
  return {
    type: telemetry.type,
    error: telemetry.error,
    renderFrame: telemetry.renderFrame,
    timelineSample: telemetry.timelineSample,
    audibleTimelineSample: telemetry.audibleTimelineSample,
    graphLatencySamplesQ8: telemetry.graphLatencySamplesQ8,
    value: telemetry.value,
  };
}
