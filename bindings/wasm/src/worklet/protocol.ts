import type { EngineMeterTelemetry, EngineTelemetry } from '../index';

const ENGINE_MIXER_TARGET_BASE = 0x4d580000;
export const ENGINE_MIXER_PARAM_FADER_DB = 1;
export const ENGINE_MIXER_PARAM_PAN = 2;

export function engineMixerLaneTarget(laneIndex: number, paramKind: number): number {
  return ENGINE_MIXER_TARGET_BASE | ((laneIndex & 0xff) << 8) | (paramKind & 0xff);
}

export function engineMixerBusTarget(busIndex: number, paramKind: number): number {
  return ENGINE_MIXER_TARGET_BASE | (((0xfe - busIndex) & 0xff) << 8) | (paramKind & 0xff);
}

export function engineMixerMasterTarget(paramKind: number): number {
  return ENGINE_MIXER_TARGET_BASE | (0xff << 8) | (paramKind & 0xff);
}

export interface SonareWorkletMeterSnapshot {
  type: 'meter';
  targetId: number;
  frame: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  truePeakDbL: number;
  truePeakDbR: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
}

export interface SonareWorkletSpectrumSnapshot {
  type: 'spectrum';
  frame: number;
  bands: Float32Array;
}

export const SONARE_METER_RING_HEADER_INTS = 4;
// Record layout: [frameLo, frameHi, targetId, peakDbL, peakDbR, rmsDbL, rmsDbR,
// correlation, truePeakDbL, truePeakDbR, momentaryLufs, shortTermLufs,
// integratedLufs, gainReductionDb].
// The sample-frame index is monotonically increasing and quickly exceeds the
// 2^24 exact-integer range of a single Float32 slot (~349 s at 48 kHz), so it is
// stored split across two Float32 lanes (low 24 bits + high bits) for exact
// reconstruction. See encodeFrameLo/encodeFrameHi/decodeFrame.
export const SONARE_METER_RING_RECORD_FLOATS = 14;
export const SONARE_SPECTRUM_RING_HEADER_INTS = 5;

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
export const SONARE_ENGINE_COMMAND_RECORD_BYTES = 32;
export const SONARE_ENGINE_TELEMETRY_RECORD_BYTES = 48;

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
}

export interface SonareMeterRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
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
  capacity: number;
  bands: number;
}

export interface SonareSpectrumRingReadResult {
  nextReadIndex: number;
  spectra: SonareWorkletSpectrumSnapshot[];
}

export interface SonareEngineCommandRecord {
  type: SonareEngineCommandType | number;
  targetId?: number;
  sampleTime?: number | bigint;
  argFloat?: number;
  argInt?: number | bigint;
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
  capacity: number;
}

export interface SonareEngineTelemetryRingReadResult {
  nextReadIndex: number;
  telemetry: SonareEngineTelemetryRecord[];
}

export interface SharedMeterRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
}

export interface SharedSpectrumRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
  bands: number;
  recordFloats: number;
}

export function toDb(value: number): number {
  return value > 0 ? 20 * Math.log10(value) : Number.NEGATIVE_INFINITY;
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

export function sonareMeterRingBufferByteLength(capacity: number): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  return (
    SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareMeterRingBuffer(capacity = 128): SonareMeterRingBuffer {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const sharedBuffer = new SharedArrayBuffer(sonareMeterRingBufferByteLength(clampedCapacity));
  const ring = meterRingFromSharedBuffer(sharedBuffer, clampedCapacity);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, clampedCapacity);
  Atomics.store(ring.header, 2, SONARE_METER_RING_RECORD_FLOATS);
  Atomics.store(ring.header, 3, 0);
  return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
}

export function readSonareMeterRingBuffer(
  ring: SonareMeterRingBuffer,
  readIndex = 0,
): SonareMeterRingReadResult {
  const writeIndex = Atomics.load(ring.header, 0);
  const recordFloats = Atomics.load(ring.header, 2) || SONARE_METER_RING_RECORD_FLOATS;
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const meters: SonareWorkletMeterSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * recordFloats;
    meters.push({
      type: 'meter',
      frame: decodeFrame(ring.records[offset], ring.records[offset + 1]),
      targetId: ring.records[offset + 2],
      peakDbL: ring.records[offset + 3],
      peakDbR: ring.records[offset + 4],
      rmsDbL: ring.records[offset + 5],
      rmsDbR: ring.records[offset + 6],
      correlation: ring.records[offset + 7],
      truePeakDbL: ring.records[offset + 8],
      truePeakDbR: ring.records[offset + 9],
      momentaryLufs: ring.records[offset + 10],
      shortTermLufs: ring.records[offset + 11],
      integratedLufs: ring.records[offset + 12],
      gainReductionDb: ring.records[offset + 13],
    });
  }
  return { nextReadIndex: writeIndex, meters };
}

export function sonareSpectrumRingBufferByteLength(capacity: number, bands = 16): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const clampedBands = Math.max(1, Math.floor(bands));
  // Record layout: [frameLo, frameHi, bandCount, band0, band1, ...]. frame is
  // split across two Float32 lanes for exact reconstruction beyond 2^24.
  return (
    SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * (3 + clampedBands) * Float32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareSpectrumRingBuffer(
  capacity = 128,
  bands = 16,
): SonareSpectrumRingBuffer {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const clampedBands = Math.max(1, Math.floor(bands));
  const sharedBuffer = new SharedArrayBuffer(
    sonareSpectrumRingBufferByteLength(clampedCapacity, clampedBands),
  );
  const ring = spectrumRingFromSharedBuffer(sharedBuffer, clampedCapacity, clampedBands);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, clampedCapacity);
  Atomics.store(ring.header, 2, ring.recordFloats);
  Atomics.store(ring.header, 3, clampedBands);
  Atomics.store(ring.header, 4, 0);
  return {
    sharedBuffer,
    header: ring.header,
    records: ring.records,
    capacity: ring.capacity,
    bands: ring.bands,
  };
}

export function readSonareSpectrumRingBuffer(
  ring: SonareSpectrumRingBuffer,
  readIndex = 0,
): SonareSpectrumRingReadResult {
  const writeIndex = Atomics.load(ring.header, 0);
  const recordFloats = Atomics.load(ring.header, 2) || 3 + ring.bands;
  const bands = Atomics.load(ring.header, 3) || ring.bands;
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const spectra: SonareWorkletSpectrumSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * recordFloats;
    const values = new Float32Array(bands);
    values.set(ring.records.subarray(offset + 3, offset + 3 + bands));
    spectra.push({
      type: 'spectrum',
      frame: decodeFrame(ring.records[offset], ring.records[offset + 1]),
      bands: values,
    });
  }
  return { nextReadIndex: writeIndex, spectra };
}

export function sonareEngineCommandRingBufferByteLength(capacity: number): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  return (
    SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_ENGINE_COMMAND_RECORD_BYTES
  );
}

export function sonareEngineTelemetryRingBufferByteLength(capacity: number): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  return (
    SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_ENGINE_TELEMETRY_RECORD_BYTES
  );
}

export function createSonareEngineCommandRingBuffer(capacity = 128): SonareEngineCommandRingBuffer {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
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
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const sharedBuffer = new SharedArrayBuffer(
    sonareEngineTelemetryRingBufferByteLength(clampedCapacity),
  );
  const ring = engineRingFromSharedBuffer(
    sharedBuffer,
    SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
    clampedCapacity,
  );
  return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
}

export function pushSonareEngineCommandRingBuffer(
  ring: SonareEngineCommandRingBuffer,
  command: SonareEngineCommandRecord,
): boolean {
  const writeIndex = Atomics.load(ring.header, 0);
  const readIndex = Atomics.load(ring.header, 1);
  if (writeIndex - readIndex >= ring.capacity) {
    Atomics.add(ring.header, 4, 1);
    return false;
  }
  writeEngineCommandRecord(
    ring.view,
    recordOffset(writeIndex, ring.capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES),
    command,
  );
  Atomics.store(ring.header, 0, writeIndex + 1);
  return true;
}

export function popSonareEngineCommandRingBuffer(
  ring: SonareEngineCommandRingBuffer,
): SonareEngineCommandRecord | null {
  const readIndex = Atomics.load(ring.header, 1);
  const writeIndex = Atomics.load(ring.header, 0);
  if (readIndex >= writeIndex) {
    return null;
  }
  const command = readEngineCommandRecord(
    ring.view,
    recordOffset(readIndex, ring.capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES),
  );
  Atomics.store(ring.header, 1, readIndex + 1);
  return command;
}

export function writeSonareEngineTelemetryRingBuffer(
  ring: SonareEngineTelemetryRingBuffer,
  telemetry: SonareEngineTelemetryRecord,
): void {
  const writeIndex = Atomics.load(ring.header, 0);
  writeEngineTelemetryRecord(
    ring.view,
    recordOffset(writeIndex, ring.capacity, SONARE_ENGINE_TELEMETRY_RECORD_BYTES),
    telemetry,
  );
  Atomics.store(ring.header, 0, writeIndex + 1);
  if (writeIndex + 1 > ring.capacity) {
    Atomics.store(ring.header, 4, writeIndex + 1 - ring.capacity);
  }
}

export function readSonareEngineTelemetryRingBuffer(
  ring: SonareEngineTelemetryRingBuffer,
  readIndex = 0,
): SonareEngineTelemetryRingReadResult {
  const writeIndex = Atomics.load(ring.header, 0);
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const telemetry: SonareEngineTelemetryRecord[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    telemetry.push(
      readEngineTelemetryRecord(
        ring.view,
        recordOffset(index, ring.capacity, SONARE_ENGINE_TELEMETRY_RECORD_BYTES),
      ),
    );
  }
  return { nextReadIndex: writeIndex, telemetry };
}

export function meterRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedMeterRingWriter {
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_METER_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const capacity = Math.max(1, Math.floor(existingCapacity || fallbackCapacity || 1));
  const minBytes = sonareMeterRingBufferByteLength(capacity);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('meterSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 1, capacity);
  Atomics.store(header, 2, SONARE_METER_RING_RECORD_FLOATS);
  return {
    header,
    records: new Float32Array(
      sharedBuffer,
      headerBytes,
      capacity * SONARE_METER_RING_RECORD_FLOATS,
    ),
    capacity,
  };
}

export function spectrumRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
  fallbackBands?: number,
): SharedSpectrumRingWriter {
  const headerBytes = SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_SPECTRUM_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const existingBands = Atomics.load(header, 3);
  const capacity = Math.max(1, Math.floor(existingCapacity || fallbackCapacity || 1));
  const bands = Math.max(1, Math.floor(existingBands || fallbackBands || 16));
  const recordFloats = 3 + bands;
  const minBytes = sonareSpectrumRingBufferByteLength(capacity, bands);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('spectrumSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 1, capacity);
  Atomics.store(header, 2, recordFloats);
  Atomics.store(header, 3, bands);
  return {
    header,
    records: new Float32Array(sharedBuffer, headerBytes, capacity * recordFloats),
    capacity,
    bands,
    recordFloats,
  };
}

export function engineRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  recordBytes: number,
  fallbackCapacity?: number,
): { header: Int32Array; view: DataView; capacity: number } {
  const headerBytes = SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_ENGINE_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = Math.max(1, Math.floor(existingCapacity || fallbackCapacity || 1));
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

function recordOffset(index: number, capacity: number, recordBytes: number): number {
  return (index % capacity) * recordBytes;
}

export function toBigInt64(value: number | bigint | undefined, fallback: bigint): bigint {
  if (typeof value === 'bigint') {
    return value;
  }
  if (typeof value === 'number') {
    return BigInt(Math.trunc(value));
  }
  return fallback;
}

function writeEngineCommandRecord(
  view: DataView,
  offset: number,
  command: SonareEngineCommandRecord,
): void {
  view.setUint32(offset, command.type, true);
  view.setUint32(offset + 4, command.targetId ?? 0, true);
  view.setBigInt64(offset + 8, toBigInt64(command.sampleTime, -1n), true);
  // argFloat occupies a full 8-byte Float64 slot (replacing the old Float32 +
  // 4-byte pad) so PPQ scalars carried here keep full double precision over the
  // SAB transport, matching the engine's double-precision seek/loop contract.
  view.setFloat64(offset + 16, command.argFloat ?? 0, true);
  view.setBigInt64(offset + 24, toBigInt64(command.argInt, 0n), true);
}

function readEngineCommandRecord(view: DataView, offset: number): SonareEngineCommandRecord {
  return {
    type: view.getUint32(offset, true),
    targetId: view.getUint32(offset + 4, true),
    sampleTime: Number(view.getBigInt64(offset + 8, true)),
    argFloat: view.getFloat64(offset + 16, true),
    argInt: Number(view.getBigInt64(offset + 24, true)),
  };
}

function writeEngineTelemetryRecord(
  view: DataView,
  offset: number,
  telemetry: SonareEngineTelemetryRecord,
): void {
  view.setUint32(offset, telemetry.type, true);
  view.setUint32(offset + 4, telemetry.error, true);
  view.setBigInt64(offset + 8, BigInt(Math.trunc(telemetry.renderFrame)), true);
  view.setBigInt64(offset + 16, BigInt(Math.trunc(telemetry.timelineSample)), true);
  view.setBigInt64(offset + 24, BigInt(Math.trunc(telemetry.audibleTimelineSample)), true);
  view.setInt32(offset + 32, telemetry.graphLatencySamplesQ8, true);
  view.setUint32(offset + 36, telemetry.value, true);
  view.setBigInt64(offset + 40, 0n, true);
}

function readEngineTelemetryRecord(view: DataView, offset: number): SonareEngineTelemetryRecord {
  return {
    type: view.getUint32(offset, true),
    error: view.getUint32(offset + 4, true),
    renderFrame: Number(view.getBigInt64(offset + 8, true)),
    timelineSample: Number(view.getBigInt64(offset + 16, true)),
    audibleTimelineSample: Number(view.getBigInt64(offset + 24, true)),
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

export function meterFromEngine(meter: EngineMeterTelemetry): SonareWorkletMeterSnapshot {
  return {
    type: 'meter',
    targetId: meter.targetId,
    frame: meter.renderFrame,
    peakDbL: meter.peakDbL,
    peakDbR: meter.peakDbR,
    rmsDbL: meter.rmsDbL,
    rmsDbR: meter.rmsDbR,
    correlation: meter.correlation,
    truePeakDbL: meter.truePeakDbL,
    truePeakDbR: meter.truePeakDbR,
    momentaryLufs: meter.momentaryLufs,
    shortTermLufs: meter.shortTermLufs,
    integratedLufs: meter.integratedLufs,
    gainReductionDb: meter.gainReductionDb,
  };
}

export function magnitudeToDb(value: number): number {
  return value > 1.0e-12 ? 20 * Math.log10(value) : -120;
}
