import {
  engineRingFromSharedBuffer,
  SONARE_ENGINE_RING_HEADER_INTS,
} from '../worklet/engine-rings.js';
import {
  loadSonareRingCursor,
  type RingCapacityOptions,
  readInt64Words,
  recordOffset,
  resolveRingCapacity,
  sonareRingCursorDistance,
  storeSonareRingCursor,
  toSafeInteger,
  toUint32Slot,
  writeInt64Words,
} from '../worklet/ring-core.js';

export const SONARE_ENGINE_COMMAND_RECORD_BYTES = 40;

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

export interface SonareEngineCommandRecord {
  type: SonareEngineCommandType | number;
  targetId?: number;
  /** Absolute render frame the command applies at; `-1` or absent: the next block head. */
  renderFrame?: number | bigint;
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

export interface SonareEngineCommandRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  view: DataView;
  capacity: number;
}

/** Byte length of the ring. `capacity` is required; RangeError outside the domain, TypeError for an unknown key. */
export function sonareEngineCommandRingBufferByteLength(options: { capacity: number }): number {
  const clampedCapacity = resolveRingCapacity(
    'sonareEngineCommandRingBufferByteLength',
    options,
    undefined,
  );
  return (
    SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_ENGINE_COMMAND_RECORD_BYTES
  );
}

/** Creates a shared engine command ring (default `capacity` 128). RangeError outside the domain, TypeError for an unknown key. */
export function createSonareEngineCommandRingBuffer(
  options?: RingCapacityOptions,
): SonareEngineCommandRingBuffer {
  const clampedCapacity = resolveRingCapacity('createSonareEngineCommandRingBuffer', options, 128);
  const sharedBuffer = new SharedArrayBuffer(
    sonareEngineCommandRingBufferByteLength({ capacity: clampedCapacity }),
  );
  const ring = engineRingFromSharedBuffer(
    sharedBuffer,
    SONARE_ENGINE_COMMAND_RECORD_BYTES,
    clampedCapacity,
  );
  return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
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

function writeEngineCommandRecord(
  view: DataView,
  offset: number,
  command: SonareEngineCommandRecord,
): void {
  // Both slots are uint32 and the readers are unsigned, so the writer holds
  // them to that domain; the command vocabulary itself is the node's check.
  view.setUint32(offset, toUint32Slot(command.type, 0, 'type'), true);
  view.setUint32(offset + 4, toUint32Slot(command.targetId, 0, 'targetId'), true);
  writeInt64Words(view, offset + 8, toSafeInteger(command.renderFrame, -1));
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
    renderFrame: readInt64Words(view, offset + 8),
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
