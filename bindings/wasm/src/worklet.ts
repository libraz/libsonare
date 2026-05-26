import type {
  EngineAutomationPoint,
  EngineClip,
  EngineMarker,
  EngineMetronomeConfig,
  EngineMeterTelemetry,
  EngineParameterInfo,
  EngineTelemetry,
  MixerRealtimeBuffer,
} from './index';
import { engineCapabilities, Mixer, RealtimeEngine } from './index';
import type { AutomationCurve } from './public_types';
import type { SonareRtModule } from './sonare-rt';

export interface SonareWorkletProcessorOptions {
  sceneJson: string;
  sampleRate?: number;
  blockSize?: number;
  stripCount?: number;
  meterIntervalFrames?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  spectrumIntervalFrames?: number;
  spectrumBands?: number;
  spectrumSharedBuffer?: SharedArrayBuffer;
  spectrumRingCapacity?: number;
}

export interface SonareRealtimeEngineWorkletProcessorOptions {
  runtimeTarget?: 'embind' | 'sonare-rt';
  rtModuleUrl?: string;
  rtWasmBinary?: ArrayBuffer | Uint8Array;
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
  meterIntervalFrames?: number;
  commandSharedBuffer?: SharedArrayBuffer;
  commandRingCapacity?: number;
  telemetrySharedBuffer?: SharedArrayBuffer;
  telemetryRingCapacity?: number;
}

export interface SonareRealtimeEngineNodeCapabilities {
  mode: 'sab' | 'postMessage';
  runtimeTarget: 'embind' | 'sonare-rt';
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  abiCompatible?: boolean;
  degradedReason?: string;
}

export interface SonareRealtimeEngineNodeOptions
  extends SonareRealtimeEngineWorkletProcessorOptions {
  processorName?: string;
  moduleUrl?: string | URL;
  rtModuleUrl?: string;
  mode?: 'auto' | 'sab' | 'postMessage';
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  requireAbiCompatible?: boolean;
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

export interface SonareRtRealtimeEngineRuntimeOptions {
  module: SonareRtModule;
  memory: WebAssembly.Memory;
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
  commandSharedBuffer?: SharedArrayBuffer;
  commandRingCapacity?: number;
  telemetrySharedBuffer?: SharedArrayBuffer;
  telemetryRingCapacity?: number;
}

export interface SonareEngineOptions extends SonareRealtimeEngineNodeOptions {
  offlineEngine?: RealtimeEngine;
  offlineBlockSize?: number;
  offlineChannelCount?: number;
}

export interface SonareEngineTransportFacade {
  play(sampleTime?: number): boolean;
  stop(sampleTime?: number): boolean;
  seekPpq(ppq: number, sampleTime?: number): boolean;
  seekSeconds(seconds: number, sampleTime?: number): boolean;
  setTempo(bpm: number): void;
  setLoop(startPpq: number, endPpq: number, enabled?: boolean): boolean;
}

type SuspendableAudioContext = BaseAudioContext & {
  suspend?: () => Promise<void>;
  resume?: () => Promise<void>;
};

type WorkletInput = readonly (readonly Float32Array[])[];
type WorkletOutput = Float32Array[][];

export interface SonareWorkletScheduleInsertAutomationMessage {
  type: 'scheduleInsertAutomation';
  stripIndex: number;
  insertIndex: number;
  paramId: number;
  value: number;
  samplePos?: number;
  curve?: AutomationCurve;
}

export interface SonareWorkletSetMeterIntervalMessage {
  type: 'setMeterInterval';
  frames: number;
}

export interface SonareWorkletDestroyMessage {
  type: 'destroy';
}

export type SonareWorkletMessage =
  | SonareWorkletScheduleInsertAutomationMessage
  | SonareWorkletSetMeterIntervalMessage
  | SonareWorkletDestroyMessage;

export interface SonareWorkletMeterSnapshot {
  type: 'meter';
  frame: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
}

export interface SonareWorkletSpectrumSnapshot {
  type: 'spectrum';
  frame: number;
  bands: Float32Array;
}

export type SonareWorkletTransportMessage =
  | SonareWorkletMeterSnapshot
  | SonareWorkletSpectrumSnapshot
  | SonareEngineTelemetryRecord;

export const SONARE_METER_RING_HEADER_INTS = 4;
export const SONARE_METER_RING_RECORD_FLOATS = 6;
export const SONARE_SPECTRUM_RING_HEADER_INTS = 5;
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

interface WorkletTransport {
  postMessage?: (message: SonareWorkletTransportMessage) => void;
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
  onSpectrum?: (spectrum: SonareWorkletSpectrumSnapshot) => void;
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

interface SharedMeterRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
}

interface SharedSpectrumRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
  bands: number;
  recordFloats: number;
}

interface SharedEngineRing {
  header: Int32Array;
  view: DataView;
  capacity: number;
}

interface WorkletPort {
  postMessage?: (message: unknown) => void;
  onmessage?: (event: { data: unknown }) => void;
  addEventListener?: (type: 'message', listener: (event: { data: unknown }) => void) => void;
  start?: () => void;
}

function toDb(value: number): number {
  return value > 0 ? 20 * Math.log10(value) : Number.NEGATIVE_INFINITY;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function isWorkletMessage(value: unknown): value is SonareWorkletMessage {
  if (!isRecord(value) || typeof value.type !== 'string') {
    return false;
  }
  return (
    value.type === 'scheduleInsertAutomation' ||
    value.type === 'setMeterInterval' ||
    value.type === 'destroy'
  );
}

function isEngineCommandRecord(value: unknown): value is SonareEngineCommandRecord {
  return isRecord(value) && typeof value.type === 'number';
}

function isEngineTelemetryRecord(value: unknown): value is SonareEngineTelemetryRecord {
  return (
    isRecord(value) &&
    typeof value.type === 'number' &&
    typeof value.error === 'number' &&
    typeof value.renderFrame === 'number' &&
    typeof value.timelineSample === 'number' &&
    typeof value.audibleTimelineSample === 'number' &&
    typeof value.graphLatencySamplesQ8 === 'number' &&
    typeof value.value === 'number'
  );
}

function isMeterSnapshot(value: unknown): value is SonareWorkletMeterSnapshot {
  return (
    isRecord(value) &&
    value.type === 'meter' &&
    typeof value.frame === 'number' &&
    typeof value.peakDbL === 'number' &&
    typeof value.peakDbR === 'number' &&
    typeof value.rmsDbL === 'number' &&
    typeof value.rmsDbR === 'number' &&
    typeof value.correlation === 'number'
  );
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
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const meters: SonareWorkletMeterSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    meters.push({
      type: 'meter',
      frame: ring.records[offset],
      peakDbL: ring.records[offset + 1],
      peakDbR: ring.records[offset + 2],
      rmsDbL: ring.records[offset + 3],
      rmsDbR: ring.records[offset + 4],
      correlation: ring.records[offset + 5],
    });
  }
  return { nextReadIndex: writeIndex, meters };
}

export function sonareSpectrumRingBufferByteLength(capacity: number, bands = 16): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const clampedBands = Math.max(1, Math.floor(bands));
  return (
    SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * (2 + clampedBands) * Float32Array.BYTES_PER_ELEMENT
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
  const recordFloats = Atomics.load(ring.header, 2) || 2 + ring.bands;
  const bands = Atomics.load(ring.header, 3) || ring.bands;
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const spectra: SonareWorkletSpectrumSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * recordFloats;
    const values = new Float32Array(bands);
    values.set(ring.records.subarray(offset + 2, offset + 2 + bands));
    spectra.push({ type: 'spectrum', frame: ring.records[offset], bands: values });
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

function meterRingFromSharedBuffer(
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

function spectrumRingFromSharedBuffer(
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
  const recordFloats = 2 + bands;
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

function engineRingFromSharedBuffer(
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

function toBigInt64(value: number | bigint | undefined, fallback: bigint): bigint {
  if (typeof value === 'bigint') return value;
  if (typeof value === 'number') return BigInt(Math.trunc(value));
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
  view.setFloat32(offset + 16, command.argFloat ?? 0, true);
  view.setUint32(offset + 20, 0, true);
  view.setBigInt64(offset + 24, toBigInt64(command.argInt, 0n), true);
}

function readEngineCommandRecord(view: DataView, offset: number): SonareEngineCommandRecord {
  return {
    type: view.getUint32(offset, true),
    targetId: view.getUint32(offset + 4, true),
    sampleTime: Number(view.getBigInt64(offset + 8, true)),
    argFloat: view.getFloat32(offset + 16, true),
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

function telemetryFromEngine(telemetry: EngineTelemetry): SonareEngineTelemetryRecord {
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

function meterFromEngine(meter: EngineMeterTelemetry): SonareWorkletMeterSnapshot {
  return {
    type: 'meter',
    frame: meter.renderFrame,
    peakDbL: meter.peakDbL,
    peakDbR: meter.peakDbR,
    rmsDbL: meter.rmsDbL,
    rmsDbR: meter.rmsDbR,
    correlation: meter.correlation,
  };
}

function magnitudeToDb(value: number): number {
  return value > 1.0e-12 ? 20 * Math.log10(value) : -120;
}

/**
 * AudioWorklet-style mixer bridge backed by the package's single `sonare.wasm`.
 *
 * The WASM module must already be initialized via `init()` before constructing
 * this bridge. Each AudioWorklet input is treated as one stereo strip:
 * `inputs[strip][0]` is left and `inputs[strip][1]` is right. Missing channels
 * are replaced with preallocated silence.
 */
export class SonareWorkletProcessor {
  readonly sampleRate: number;
  readonly blockSize: number;
  private mixer: Mixer;
  private realtime: MixerRealtimeBuffer;
  private closed = false;
  private processedFrames = 0;
  private lastMeterFrame = 0;
  private meterIntervalFrames: number;
  private spectrumIntervalFrames: number;
  private lastSpectrumFrame = 0;
  private transport?: WorkletTransport;
  private meterRing?: SharedMeterRingWriter;
  private spectrumRing?: SharedSpectrumRingWriter;
  private spectrumBands: Float32Array;

  constructor(options: SonareWorkletProcessorOptions, transport?: WorkletTransport) {
    if (!options.sceneJson) {
      throw new Error('sceneJson is required.');
    }
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.spectrumIntervalFrames = Math.max(0, Math.floor(options.spectrumIntervalFrames ?? 0));
    this.transport = transport;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.meterRing = options.meterSharedBuffer
      ? meterRingFromSharedBuffer(options.meterSharedBuffer, options.meterRingCapacity)
      : undefined;
    this.spectrumRing = options.spectrumSharedBuffer
      ? spectrumRingFromSharedBuffer(
          options.spectrumSharedBuffer,
          options.spectrumRingCapacity,
          options.spectrumBands,
        )
      : undefined;
    const spectrumBandCount = this.spectrumRing?.bands ?? Math.max(1, options.spectrumBands ?? 16);
    this.spectrumBands = new Float32Array(spectrumBandCount);
    this.mixer = Mixer.fromSceneJson(options.sceneJson, this.sampleRate, this.blockSize);
    this.mixer.compile();
    const sceneStripCount = this.mixer.stripCount();
    const stripCount = options.stripCount ?? sceneStripCount;
    if (stripCount !== sceneStripCount) {
      throw new Error('stripCount must match the scene strip count.');
    }
    this.realtime = this.mixer.createRealtimeBuffer();
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const leftOut = output?.[0];
    const rightOut = output?.[1];
    if (!leftOut) {
      return true;
    }
    const frames = leftOut.length;
    if (frames !== this.blockSize) {
      return false;
    }

    for (let strip = 0; strip < this.realtime.leftInputs.length; strip++) {
      const input = inputs[strip];
      const left = input?.[0];
      const right = input?.[1];
      const leftTarget = this.realtime.leftInputs[strip];
      const rightTarget = this.realtime.rightInputs[strip];
      if (left && left.length === frames) {
        leftTarget.set(left);
        if (right && right.length === frames) {
          rightTarget.set(right);
        } else {
          rightTarget.set(left);
        }
      } else {
        leftTarget.fill(0);
        rightTarget.fill(0);
      }
    }

    this.realtime.process(frames);
    leftOut.set(this.realtime.outLeft);
    if (rightOut) {
      rightOut.set(this.realtime.outRight);
    }
    this.processedFrames += frames;
    this.publishMeter(this.realtime.outLeft, this.realtime.outRight);
    this.publishSpectrum(this.realtime.outLeft, this.realtime.outRight);
    return true;
  }

  receiveMessage(message: SonareWorkletMessage): void {
    if (this.closed) {
      return;
    }
    if (message.type === 'destroy') {
      this.destroy();
      return;
    }
    if (message.type === 'setMeterInterval') {
      this.meterIntervalFrames = Math.max(0, Math.floor(message.frames));
      return;
    }
    if (message.type === 'scheduleInsertAutomation') {
      this.mixer.scheduleInsertAutomation(
        message.stripIndex,
        message.insertIndex,
        message.paramId,
        message.samplePos ?? this.processedFrames,
        message.value,
        message.curve ?? 'linear',
      );
    }
  }

  destroy(): void {
    if (!this.closed) {
      this.mixer.delete();
      this.closed = true;
    }
  }

  private publishMeter(left: Float32Array, right: Float32Array): void {
    if (!this.transport || this.meterIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastMeterFrame < this.meterIntervalFrames) {
      return;
    }
    this.lastMeterFrame = this.processedFrames;

    let peakL = 0;
    let peakR = 0;
    let sumL = 0;
    let sumR = 0;
    let sumLR = 0;
    for (let i = 0; i < left.length; i++) {
      const l = left[i] ?? 0;
      const r = right[i] ?? 0;
      const absL = Math.abs(l);
      const absR = Math.abs(r);
      if (absL > peakL) {
        peakL = absL;
      }
      if (absR > peakR) {
        peakR = absR;
      }
      sumL += l * l;
      sumR += r * r;
      sumLR += l * r;
    }
    const rmsL = Math.sqrt(sumL / Math.max(1, left.length));
    const rmsR = Math.sqrt(sumR / Math.max(1, right.length));
    const denominator = Math.sqrt(sumL * sumR);
    const meter: SonareWorkletMeterSnapshot = {
      type: 'meter',
      frame: this.processedFrames,
      peakDbL: toDb(peakL),
      peakDbR: toDb(peakR),
      rmsDbL: toDb(rmsL),
      rmsDbR: toDb(rmsR),
      correlation: denominator > 0 ? sumLR / denominator : 0,
    };
    this.transport.onMeter?.(meter);
    if (this.meterRing) {
      this.writeMeterRing(meter);
    } else {
      this.transport.postMessage?.(meter);
    }
  }

  private writeMeterRing(meter: SonareWorkletMeterSnapshot): void {
    const ring = this.meterRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    ring.records[offset] = meter.frame;
    ring.records[offset + 1] = meter.peakDbL;
    ring.records[offset + 2] = meter.peakDbR;
    ring.records[offset + 3] = meter.rmsDbL;
    ring.records[offset + 4] = meter.rmsDbR;
    ring.records[offset + 5] = meter.correlation;
    Atomics.store(ring.header, 0, writeIndex + 1);
    if (writeIndex + 1 > ring.capacity) {
      Atomics.store(ring.header, 3, writeIndex + 1 - ring.capacity);
    }
  }

  private publishSpectrum(left: Float32Array, right: Float32Array): void {
    if (this.spectrumIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastSpectrumFrame < this.spectrumIntervalFrames) {
      return;
    }
    this.lastSpectrumFrame = this.processedFrames;
    this.computeSpectrum(left, right);
    if (this.spectrumRing) {
      this.writeSpectrumRing(this.processedFrames, this.spectrumBands);
      return;
    }
    const spectrum: SonareWorkletSpectrumSnapshot = {
      type: 'spectrum',
      frame: this.processedFrames,
      bands: new Float32Array(this.spectrumBands),
    };
    this.transport?.onSpectrum?.(spectrum);
    this.transport?.postMessage?.(spectrum);
  }

  private computeSpectrum(left: Float32Array, right: Float32Array): void {
    const n = Math.max(1, Math.min(left.length, right.length));
    for (let band = 0; band < this.spectrumBands.length; band++) {
      const bin = band + 1;
      let real = 0;
      let imag = 0;
      for (let i = 0; i < n; i++) {
        const sample = 0.5 * ((left[i] ?? 0) + (right[i] ?? 0));
        const phase = (-2 * Math.PI * bin * i) / n;
        real += sample * Math.cos(phase);
        imag += sample * Math.sin(phase);
      }
      this.spectrumBands[band] = magnitudeToDb((2 * Math.hypot(real, imag)) / n);
    }
  }

  private writeSpectrumRing(frame: number, bands: Float32Array): void {
    const ring = this.spectrumRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * ring.recordFloats;
    ring.records[offset] = frame;
    ring.records[offset + 1] = bands.length;
    ring.records.set(bands.subarray(0, ring.bands), offset + 2);
    Atomics.store(ring.header, 0, writeIndex + 1);
    if (writeIndex + 1 > ring.capacity) {
      Atomics.store(ring.header, 4, writeIndex + 1 - ring.capacity);
    }
  }
}

/**
 * AudioWorklet-style bridge for the DAW realtime engine facade.
 *
 * The default mode uses the existing `sonare.wasm` embind facade. The
 * `sonare-rt` target is exposed as a selectable runtime target for hosts that
 * load the dedicated Emscripten AudioWorklet module.
 */
export class SonareRealtimeEngineWorkletProcessor {
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly channelCount: number;
  readonly runtimeTarget: 'embind' | 'sonare-rt';
  private engine: RealtimeEngine;
  private closed = false;
  private commandRing?: SonareEngineCommandRingBuffer;
  private telemetryRing?: SonareEngineTelemetryRingBuffer;
  private transport?: WorkletTransport;
  private meterIntervalFrames: number;
  private lastMeterFrame = Number.NEGATIVE_INFINITY;

  constructor(
    options: SonareRealtimeEngineWorkletProcessorOptions = {},
    transport?: WorkletTransport,
  ) {
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    this.runtimeTarget = options.runtimeTarget ?? 'embind';
    if (this.runtimeTarget === 'sonare-rt') {
      throw new Error(
        'sonare-rt runtime is provided by the dedicated Emscripten AudioWorklet module; use SonareRealtimeEngineNode.create({ runtimeTarget: "sonare-rt", moduleUrl: ... }) to load it.',
      );
    }
    this.transport = transport;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.commandRing = options.commandSharedBuffer
      ? this.commandRingFromSharedBuffer(options.commandSharedBuffer, options.commandRingCapacity)
      : undefined;
    this.telemetryRing = options.telemetrySharedBuffer
      ? this.telemetryRingFromSharedBuffer(
          options.telemetrySharedBuffer,
          options.telemetryRingCapacity,
        )
      : undefined;
    this.engine = new RealtimeEngine(this.sampleRate, this.blockSize);
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const firstOutput = output?.[0];
    if (!firstOutput) {
      return true;
    }
    const frames = firstOutput.length;
    if (frames > this.blockSize) {
      for (const channel of output ?? []) {
        channel.fill(0);
      }
      this.publishTelemetry();
      return true;
    }

    this.drainCommands();

    const channels: Float32Array[] = [];
    const input = inputs[0];
    for (let ch = 0; ch < this.channelCount; ch++) {
      const source = input?.[ch];
      const channel = new Float32Array(frames);
      if (source && source.length === frames) {
        channel.set(source);
      }
      channels.push(channel);
    }

    const processed = this.engine.process(channels);
    for (let ch = 0; ch < output.length; ch++) {
      const target = output[ch];
      const source = processed[ch] ?? processed[0];
      if (source) {
        target.set(source.subarray(0, target.length));
      } else {
        target.fill(0);
      }
    }
    this.publishTelemetry();
    this.publishMeters();
    return true;
  }

  receiveCommand(command: SonareEngineCommandRecord): void {
    if (!this.closed) {
      this.applyCommand(command);
    }
  }

  destroy(): void {
    if (!this.closed) {
      this.engine.destroy();
      this.closed = true;
    }
  }

  private drainCommands(): void {
    if (!this.commandRing) {
      return;
    }
    for (let i = 0; i < 64; i++) {
      const command = popSonareEngineCommandRingBuffer(this.commandRing);
      if (!command) {
        return;
      }
      this.applyCommand(command);
    }
  }

  private applyCommand(command: SonareEngineCommandRecord): void {
    const sampleTime = Number(command.sampleTime ?? -1);
    switch (command.type) {
      case SonareEngineCommandType.TransportPlay:
        this.engine.play(sampleTime);
        break;
      case SonareEngineCommandType.TransportStop:
        this.engine.stop(sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekSample:
        this.engine.seekSample(Number(command.argInt ?? 0), sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekPpq:
        this.engine.seekPpq(Number(command.argFloat ?? 0), sampleTime);
        break;
      case SonareEngineCommandType.SetTempoMap:
        this.engine.setTempo(Number(command.argFloat ?? 120));
        break;
      case SonareEngineCommandType.SetLoop:
        this.engine.setLoop(
          Number(command.argFloat ?? 0),
          Number(command.argInt ?? 0) / 1_000_000,
          command.targetId !== 0,
        );
        break;
      case SonareEngineCommandType.ArmRecord:
        this.engine.armCapture(Boolean(command.argInt));
        break;
      case SonareEngineCommandType.Punch:
        this.engine.setCapturePunch(
          Number(command.argInt ?? 0),
          Math.max(0, Math.round(Number(command.argFloat ?? 0) * this.sampleRate)),
          true,
        );
        break;
      case SonareEngineCommandType.SetMetronome:
        this.engine.setMetronome({
          enabled: Boolean(command.argInt),
          beatGain: 0.25,
          accentGain: 0.75,
          clickSamples: 64,
        });
        break;
      default:
        this.publishTelemetryRecord({
          type: SonareEngineTelemetryType.Error,
          error: SonareEngineTelemetryError.UnknownTarget,
          renderFrame: 0,
          timelineSample: 0,
          audibleTimelineSample: 0,
          graphLatencySamplesQ8: 0,
          value: Number(command.type),
        });
        break;
    }
  }

  private publishTelemetry(): void {
    for (const item of this.engine.drainTelemetry(64)) {
      this.publishTelemetryRecord(telemetryFromEngine(item));
    }
  }

  private publishTelemetryRecord(record: SonareEngineTelemetryRecord): void {
    if (this.telemetryRing) {
      writeSonareEngineTelemetryRingBuffer(this.telemetryRing, record);
      return;
    }
    this.transport?.postMessage?.(record);
  }

  private publishMeters(): void {
    if (!this.transport || this.meterIntervalFrames <= 0) {
      return;
    }
    for (const item of this.engine.drainMeterTelemetry(64)) {
      const meter = meterFromEngine(item);
      if (meter.frame - this.lastMeterFrame < this.meterIntervalFrames) {
        continue;
      }
      this.lastMeterFrame = meter.frame;
      this.transport.onMeter?.(meter);
      this.transport.postMessage?.(meter);
    }
  }

  private commandRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineCommandRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_COMMAND_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }

  private telemetryRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineTelemetryRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }
}

export class SonareRtRealtimeEngineRuntime {
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly channelCount: number;
  private readonly module: SonareRtModule;
  private readonly memory: WebAssembly.Memory;
  private readonly engine: number;
  private readonly channelPointerTable: number;
  private readonly channelBuffers: number[];
  private readonly telemetryIntsPtr: number;
  private readonly telemetryFramesPtr: number;
  private readonly commandRing?: SonareEngineCommandRingBuffer;
  private readonly telemetryRing?: SonareEngineTelemetryRingBuffer;
  private closed = false;

  constructor(options: SonareRtRealtimeEngineRuntimeOptions) {
    this.module = options.module;
    this.memory = options.memory;
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    this.commandRing = options.commandSharedBuffer
      ? this.commandRingFromSharedBuffer(options.commandSharedBuffer, options.commandRingCapacity)
      : undefined;
    this.telemetryRing = options.telemetrySharedBuffer
      ? this.telemetryRingFromSharedBuffer(
          options.telemetrySharedBuffer,
          options.telemetryRingCapacity,
        )
      : undefined;

    this.engine = this.module._sonare_rt_engine_create();
    if (this.engine <= 0) {
      throw new Error('failed to create sonare-rt engine');
    }
    if (
      this.module._sonare_rt_engine_prepare(
        this.engine,
        this.sampleRate,
        this.blockSize,
        1024,
        1024,
      ) !== 1
    ) {
      this.module._sonare_rt_engine_destroy(this.engine);
      throw new Error('failed to prepare sonare-rt engine');
    }
    this.channelPointerTable = this.module._malloc(
      this.channelCount * Uint32Array.BYTES_PER_ELEMENT,
    );
    this.channelBuffers = [];
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.channelBuffers.push(
        this.module._malloc(this.blockSize * Float32Array.BYTES_PER_ELEMENT),
      );
    }
    this.telemetryIntsPtr = this.module._malloc(64 * 4 * Int32Array.BYTES_PER_ELEMENT);
    this.telemetryFramesPtr = this.module._malloc(64 * 3 * Float64Array.BYTES_PER_ELEMENT);
    this.writeChannelPointers();
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const firstOutput = output?.[0];
    if (!firstOutput) {
      return true;
    }
    const frames = firstOutput.length;
    if (frames > this.blockSize) {
      for (const channel of output) {
        channel.fill(0);
      }
      return true;
    }

    this.drainCommands();
    const heap = new Float32Array(this.memory.buffer);
    const input = inputs[0];
    for (let ch = 0; ch < this.channelCount; ch++) {
      const ptr = this.channelBuffers[ch] ?? this.channelBuffers[0];
      const offset = ptr >> 2;
      const source = input?.[ch];
      if (source && source.length === frames) {
        heap.set(source, offset);
      } else {
        heap.fill(0, offset, offset + frames);
      }
    }

    this.module._sonare_rt_engine_process(
      this.engine,
      this.channelPointerTable,
      this.channelCount,
      frames,
    );

    for (let ch = 0; ch < output.length; ch++) {
      const target = output[ch];
      const ptr = this.channelBuffers[ch] ?? this.channelBuffers[0];
      target.set(heap.subarray(ptr >> 2, (ptr >> 2) + target.length));
    }
    this.publishTelemetry();
    return true;
  }

  destroy(): void {
    if (this.closed) {
      return;
    }
    this.module._free(this.telemetryFramesPtr);
    this.module._free(this.telemetryIntsPtr);
    for (const ptr of this.channelBuffers) {
      this.module._free(ptr);
    }
    this.module._free(this.channelPointerTable);
    this.module._sonare_rt_engine_destroy(this.engine);
    this.closed = true;
  }

  private writeChannelPointers(): void {
    const pointers = new Uint32Array(this.memory.buffer);
    const offset = this.channelPointerTable >> 2;
    for (let ch = 0; ch < this.channelBuffers.length; ch++) {
      pointers[offset + ch] = this.channelBuffers[ch];
    }
  }

  private drainCommands(): void {
    if (!this.commandRing) {
      return;
    }
    for (let i = 0; i < 64; i++) {
      const command = popSonareEngineCommandRingBuffer(this.commandRing);
      if (!command) {
        return;
      }
      this.applyCommand(command);
    }
  }

  private applyCommand(command: SonareEngineCommandRecord): void {
    const sampleTime = toBigInt64(command.sampleTime, -1n);
    switch (command.type) {
      case SonareEngineCommandType.TransportPlay:
        this.module._sonare_rt_engine_play(this.engine, sampleTime);
        break;
      case SonareEngineCommandType.TransportStop:
        this.module._sonare_rt_engine_stop(this.engine, sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekSample:
        this.module._sonare_rt_engine_seek_sample(
          this.engine,
          toBigInt64(command.argInt, 0n),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.TransportSeekPpq:
        this.module._sonare_rt_engine_seek_ppq(
          this.engine,
          Number(command.argFloat ?? 0),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.SetTempoMap:
        this.module._sonare_rt_engine_set_tempo(this.engine, Number(command.argFloat ?? 120));
        break;
      case SonareEngineCommandType.SetLoop:
        this.module._sonare_rt_engine_set_loop(
          this.engine,
          Number(command.argFloat ?? 0),
          Number(command.argInt ?? 0) / 1_000_000,
          command.targetId ? 1 : 0,
        );
        break;
      case SonareEngineCommandType.ArmRecord:
        this.module._sonare_rt_engine_set_capture_armed(this.engine, command.argInt ? 1 : 0);
        break;
      case SonareEngineCommandType.Punch:
        this.module._sonare_rt_engine_set_capture_punch(
          this.engine,
          toBigInt64(command.argInt, 0n),
          BigInt(Math.trunc(Number(command.argFloat ?? 0) * this.sampleRate)),
          1,
        );
        break;
      case SonareEngineCommandType.SetMetronome:
        this.module._sonare_rt_engine_set_metronome_enabled(
          this.engine,
          command.argInt ? 1 : 0,
          0.25,
          0.75,
          64,
        );
        break;
      case SonareEngineCommandType.SeekMarker:
        this.module._sonare_rt_engine_seek_marker(
          this.engine,
          Math.trunc(command.targetId ?? 0),
          sampleTime,
        );
        break;
      default:
        if (this.telemetryRing) {
          writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
            type: SonareEngineTelemetryType.Error,
            error: SonareEngineTelemetryError.UnknownTarget,
            renderFrame: 0,
            timelineSample: 0,
            audibleTimelineSample: 0,
            graphLatencySamplesQ8: 0,
            value: Number(command.type),
          });
        }
        break;
    }
  }

  private publishTelemetry(): void {
    if (!this.telemetryRing) {
      this.module._sonare_rt_engine_drain_telemetry(
        this.engine,
        this.telemetryIntsPtr,
        this.telemetryFramesPtr,
        64,
      );
      return;
    }
    const count = this.module._sonare_rt_engine_drain_telemetry(
      this.engine,
      this.telemetryIntsPtr,
      this.telemetryFramesPtr,
      64,
    );
    const ints = new Int32Array(this.memory.buffer);
    const frames = new Float64Array(this.memory.buffer);
    const intBase = this.telemetryIntsPtr >> 2;
    const frameBase = this.telemetryFramesPtr >> 3;
    for (let i = 0; i < count; i++) {
      writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
        type: ints[intBase + i * 4],
        error: ints[intBase + i * 4 + 1],
        renderFrame: frames[frameBase + i * 3],
        timelineSample: frames[frameBase + i * 3 + 1],
        audibleTimelineSample: frames[frameBase + i * 3 + 2],
        graphLatencySamplesQ8: ints[intBase + i * 4 + 2],
        value: ints[intBase + i * 4 + 3],
      });
    }
  }

  private commandRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineCommandRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_COMMAND_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }

  private telemetryRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineTelemetryRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }
}

export class SonareRealtimeEngineNode {
  readonly node: AudioWorkletNode;
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  readonly commandRing?: SonareEngineCommandRingBuffer;
  readonly telemetryRing?: SonareEngineTelemetryRingBuffer;
  readonly ready: Promise<void>;
  private telemetryReadIndex = 0;
  private telemetryListeners = new Set<(telemetry: SonareEngineTelemetryRecord) => void>();
  private meterListeners = new Set<(meter: SonareWorkletMeterSnapshot) => void>();
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private destroyed = false;

  private constructor(
    node: AudioWorkletNode,
    capabilities: SonareRealtimeEngineNodeCapabilities,
    commandRing?: SonareEngineCommandRingBuffer,
    telemetryRing?: SonareEngineTelemetryRingBuffer,
  ) {
    this.node = node;
    this.capabilities = capabilities;
    this.commandRing = commandRing;
    this.telemetryRing = telemetryRing;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    if (capabilities.runtimeTarget !== 'sonare-rt') {
      this.resolveReady();
    }
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      if (isEngineTelemetryRecord(event.data)) {
        this.emitTelemetry(event.data);
      } else if (isMeterSnapshot(event.data)) {
        this.emitMeter(event.data);
      } else if (isRecord(event.data) && event.data.type === 'ready') {
        this.resolveReady();
      } else if (isRecord(event.data) && event.data.type === 'error') {
        this.rejectReady(new Error(String(event.data.message ?? 'AudioWorklet error')));
      }
    };
  }

  static async create(
    context: BaseAudioContext,
    options: SonareRealtimeEngineNodeOptions = {},
  ): Promise<SonareRealtimeEngineNode> {
    const runtimeTarget = options.runtimeTarget ?? 'embind';
    const processorName = options.processorName ?? 'sonare-realtime-engine-processor';
    const moduleUrl = options.moduleUrl;
    if (moduleUrl && context.audioWorklet?.addModule) {
      await context.audioWorklet.addModule(moduleUrl);
    }
    const detectedCapabilities =
      options.engineAbiVersion !== undefined
        ? {
            engineAbiVersion: options.engineAbiVersion,
            expectedEngineAbiVersion: options.expectedEngineAbiVersion ?? options.engineAbiVersion,
            abiCompatible:
              options.engineAbiVersion ===
              (options.expectedEngineAbiVersion ?? options.engineAbiVersion),
          }
        : runtimeTarget === 'embind'
          ? engineCapabilities()
          : undefined;
    if (options.requireAbiCompatible !== false && detectedCapabilities?.abiCompatible === false) {
      throw new Error(
        `Engine ABI mismatch: wasm=${detectedCapabilities.engineAbiVersion}, expected=${detectedCapabilities.expectedEngineAbiVersion}`,
      );
    }
    const sharedArrayBuffer = typeof globalThis.SharedArrayBuffer === 'function';
    const atomics = typeof globalThis.Atomics === 'object';
    const audioWorklet = typeof AudioWorkletNode !== 'undefined' || !!options.nodeFactory;
    const degradedReason =
      options.mode !== 'postMessage' && (!sharedArrayBuffer || !atomics)
        ? 'SharedArrayBuffer or Atomics unavailable; using postMessage transport.'
        : undefined;
    const mode =
      options.mode === 'postMessage' || !sharedArrayBuffer || !atomics ? 'postMessage' : 'sab';
    if (options.mode === 'sab' && mode !== 'sab') {
      throw new Error(
        'SharedArrayBuffer mode requested but SharedArrayBuffer/Atomics are unavailable.',
      );
    }

    const commandRing =
      mode === 'sab'
        ? createSonareEngineCommandRingBuffer(options.commandRingCapacity ?? 128)
        : undefined;
    const telemetryRing =
      mode === 'sab'
        ? createSonareEngineTelemetryRingBuffer(options.telemetryRingCapacity ?? 128)
        : undefined;
    const channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    const processorOptions: SonareRealtimeEngineWorkletProcessorOptions = {
      runtimeTarget,
      rtModuleUrl: options.rtModuleUrl,
      rtWasmBinary: options.rtWasmBinary,
      sampleRate: options.sampleRate ?? context.sampleRate,
      blockSize: options.blockSize,
      channelCount,
      commandSharedBuffer: commandRing?.sharedBuffer,
      commandRingCapacity: commandRing?.capacity,
      telemetrySharedBuffer: telemetryRing?.sharedBuffer,
      telemetryRingCapacity: telemetryRing?.capacity,
    };
    const factory =
      options.nodeFactory ??
      ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
        new AudioWorkletNode(ctx, name, nodeOptions));
    const node = factory(context, processorName, {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [channelCount],
      processorOptions,
    });
    return new SonareRealtimeEngineNode(
      node,
      {
        mode,
        runtimeTarget,
        sharedArrayBuffer,
        atomics,
        audioWorklet,
        engineAbiVersion: detectedCapabilities?.engineAbiVersion,
        expectedEngineAbiVersion: detectedCapabilities?.expectedEngineAbiVersion,
        abiCompatible: detectedCapabilities?.abiCompatible,
        degradedReason,
      },
      commandRing,
      telemetryRing,
    );
  }

  play(sampleTime = -1): boolean {
    return this.sendCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime });
  }

  stop(sampleTime = -1): boolean {
    return this.sendCommand({ type: SonareEngineCommandType.TransportStop, sampleTime });
  }

  seekSample(timelineSample: number, sampleTime = -1): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekSample,
      sampleTime,
      argInt: timelineSample,
    });
  }

  seekPpq(ppq: number, sampleTime = -1): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekPpq,
      sampleTime,
      argFloat: ppq,
    });
  }

  sendCommand(command: SonareEngineCommandRecord): boolean {
    if (this.destroyed) {
      return false;
    }
    if (this.commandRing) {
      return pushSonareEngineCommandRingBuffer(this.commandRing, command);
    }
    this.node.port.postMessage(command);
    return true;
  }

  pollTelemetry(): SonareEngineTelemetryRecord[] {
    if (!this.telemetryRing) {
      return [];
    }
    const read = readSonareEngineTelemetryRingBuffer(this.telemetryRing, this.telemetryReadIndex);
    this.telemetryReadIndex = read.nextReadIndex;
    for (const telemetry of read.telemetry) {
      this.emitTelemetry(telemetry);
    }
    return read.telemetry;
  }

  onTelemetry(callback: (telemetry: SonareEngineTelemetryRecord) => void): () => void {
    this.telemetryListeners.add(callback);
    return () => {
      this.telemetryListeners.delete(callback);
    };
  }

  onMeter(callback: (meter: SonareWorkletMeterSnapshot) => void): () => void {
    this.meterListeners.add(callback);
    return () => {
      this.meterListeners.delete(callback);
    };
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.node.port.postMessage({ type: SonareEngineCommandType.TransportStop, sampleTime: -1 });
    this.node.disconnect();
    this.telemetryListeners.clear();
    this.meterListeners.clear();
  }

  private emitTelemetry(telemetry: SonareEngineTelemetryRecord): void {
    for (const listener of this.telemetryListeners) {
      listener(telemetry);
    }
  }

  private emitMeter(meter: SonareWorkletMeterSnapshot): void {
    for (const listener of this.meterListeners) {
      listener(meter);
    }
  }
}

export class SonareEngine {
  readonly node: AudioWorkletNode;
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  readonly transport: SonareEngineTransportFacade;
  private readonly realtimeNode: SonareRealtimeEngineNode;
  private readonly offlineEngine: RealtimeEngine;
  private readonly context: SuspendableAudioContext;
  private readonly sampleRate: number;
  private readonly offlineBlockSize: number;
  private readonly offlineChannelCount: number;
  private readonly automationLanes = new Map<number, EngineAutomationPoint[]>();
  private readonly clips = new Map<number, EngineClip>();
  private readonly markers = new Map<number, EngineMarker>();
  private nextClipId = 1;
  private nextMarkerId = 1;
  private destroyed = false;

  private constructor(
    context: BaseAudioContext,
    realtimeNode: SonareRealtimeEngineNode,
    offlineEngine: RealtimeEngine,
    sampleRate: number,
    offlineBlockSize: number,
    offlineChannelCount: number,
  ) {
    this.context = context;
    this.realtimeNode = realtimeNode;
    this.offlineEngine = offlineEngine;
    this.node = realtimeNode.node;
    this.capabilities = realtimeNode.capabilities;
    this.sampleRate = sampleRate;
    this.offlineBlockSize = offlineBlockSize;
    this.offlineChannelCount = offlineChannelCount;
    this.transport = {
      play: (sampleTime = -1) => this.realtimeNode.play(sampleTime),
      stop: (sampleTime = -1) => this.realtimeNode.stop(sampleTime),
      seekPpq: (ppq, sampleTime = -1) => {
        this.offlineEngine.seekPpq(ppq, sampleTime);
        return this.realtimeNode.seekPpq(ppq, sampleTime);
      },
      seekSeconds: (seconds, sampleTime = -1) => {
        const timelineSample = Math.max(0, Math.round(seconds * this.sampleRate));
        this.offlineEngine.seekSample(timelineSample, sampleTime);
        return this.realtimeNode.seekSample(timelineSample, sampleTime);
      },
      setTempo: (bpm) => this.setTempo(bpm),
      setLoop: (startPpq, endPpq, enabled = true) => this.setLoop(startPpq, endPpq, enabled),
    };
  }

  static async create(
    context: BaseAudioContext,
    options: SonareEngineOptions = {},
  ): Promise<SonareEngine> {
    const sampleRate = options.sampleRate ?? context.sampleRate;
    const blockSize = options.offlineBlockSize ?? options.blockSize ?? 128;
    const channelCount = Math.max(
      1,
      Math.floor(options.offlineChannelCount ?? options.channelCount ?? 2),
    );
    const realtimeNode = await SonareRealtimeEngineNode.create(context, options);
    const offlineEngine = options.offlineEngine ?? new RealtimeEngine(sampleRate, blockSize);
    return new SonareEngine(
      context,
      realtimeNode,
      offlineEngine,
      sampleRate,
      blockSize,
      channelCount,
    );
  }

  async suspend(): Promise<void> {
    if (this.destroyed) {
      return;
    }
    await this.context.suspend?.();
  }

  async resume(): Promise<void> {
    if (this.destroyed) {
      return;
    }
    await this.context.resume?.();
  }

  setTempo(bpm: number): void {
    this.offlineEngine.setTempo(bpm);
    this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetTempoMap,
      sampleTime: -1,
      argFloat: bpm,
    });
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): boolean {
    this.offlineEngine.setLoop(startPpq, endPpq, enabled);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetLoop,
      targetId: enabled ? 1 : 0,
      sampleTime: -1,
      argFloat: startPpq,
      argInt: Math.round(endPpq * 1_000_000),
    });
  }

  setParam(nodeId: string, param: string | number, value: number): boolean {
    void nodeId;
    void param;
    void value;
    return false;
  }

  scheduleParam(
    nodeId: string,
    param: string | number,
    ppq: number,
    value: number,
    curve: number | 'linear' | 'exponential' = 'linear',
  ): void {
    const paramId = this.resolveParamId(nodeId, param);
    const lane = this.automationLanes.get(paramId) ?? [];
    lane.push({ ppq, value, curveToNext: this.curveCode(curve) });
    lane.sort((a, b) => a.ppq - b.ppq);
    this.automationLanes.set(paramId, lane);
    this.offlineEngine.setAutomationLane(paramId, lane);
  }

  addAutomationPoint(
    laneId: string | number,
    ppq: number,
    value: number,
    curve: number | 'linear' | 'exponential' = 'linear',
  ): void {
    this.scheduleParam('', laneId, ppq, value, curve);
  }

  listParameters(): EngineParameterInfo[] {
    const parameters: EngineParameterInfo[] = [];
    for (let index = 0; index < this.offlineEngine.parameterCount(); index++) {
      parameters.push(this.offlineEngine.parameterInfoByIndex(index));
    }
    return parameters;
  }

  setSoloMute(target: string | number, solo: boolean, mute: boolean): boolean {
    void target;
    void solo;
    void mute;
    return false;
  }

  addClip(
    trackId: string | number,
    buffer: Float32Array[],
    startPpq: number,
    opts: Partial<Omit<EngineClip, 'channels' | 'startPpq'>> = {},
  ): number {
    const id = opts.id ?? this.nextClipId++;
    const clip: EngineClip = {
      ...opts,
      id,
      channels: buffer,
      startPpq,
    };
    this.clips.set(id, clip);
    this.syncClips();
    void trackId;
    return id;
  }

  removeClip(clipId: number): void {
    this.clips.delete(clipId);
    this.syncClips();
  }

  armRecord(trackId: string | number, enabled: boolean): boolean {
    this.offlineEngine.armCapture(enabled);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.ArmRecord,
      targetId: this.resolveTargetId(trackId),
      sampleTime: -1,
      argInt: enabled ? 1 : 0,
    });
  }

  punch(inPpq: number, outPpq: number): boolean {
    const inSample = this.ppqToApproxSample(inPpq);
    const outSample = this.ppqToApproxSample(outPpq);
    this.offlineEngine.setCapturePunch(inSample, outSample, true);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.Punch,
      sampleTime: -1,
      argInt: inSample,
      argFloat: outPpq,
    });
  }

  setMetronome(opts: EngineMetronomeConfig): void {
    this.offlineEngine.setMetronome(opts);
    this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetMetronome,
      sampleTime: -1,
      argInt: opts.enabled ? 1 : 0,
    });
  }

  addMarker(ppq: number, name = ''): number {
    const id = this.nextMarkerId++;
    this.markers.set(id, { id, ppq, name });
    this.syncMarkers();
    return id;
  }

  seekMarker(markerId: number): boolean {
    this.offlineEngine.seekMarker(markerId);
    return false;
  }

  async renderOffline(totalFrames: number): Promise<Float32Array[]> {
    const frames = Math.max(0, Math.floor(totalFrames));
    const inputs: Float32Array[] = [];
    for (let ch = 0; ch < this.offlineChannelCount; ch++) {
      inputs.push(new Float32Array(frames));
    }
    return this.offlineEngine.renderOffline(inputs, this.offlineBlockSize);
  }

  onMeter(callback: (meter: SonareWorkletMeterSnapshot) => void): () => void {
    return this.realtimeNode.onMeter(callback);
  }

  onTelemetry(callback: (telemetry: SonareEngineTelemetryRecord) => void): () => void {
    return this.realtimeNode.onTelemetry(callback);
  }

  pollTelemetry(): SonareEngineTelemetryRecord[] {
    return this.realtimeNode.pollTelemetry();
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.transport.stop();
    this.realtimeNode.pollTelemetry();
    this.realtimeNode.destroy();
    this.offlineEngine.destroy();
  }

  private syncClips(): void {
    this.offlineEngine.setClips(Array.from(this.clips.values()));
  }

  private syncMarkers(): void {
    this.offlineEngine.setMarkers(Array.from(this.markers.values()).sort((a, b) => a.ppq - b.ppq));
  }

  private resolveParamId(nodeId: string, param: string | number): number {
    if (typeof param === 'number') {
      return param;
    }
    const byName = this.listParameters().find((info) => info.name === param);
    if (byName) {
      return byName.id;
    }
    return this.resolveTargetId(param || nodeId);
  }

  private resolveTargetId(target: string | number): number {
    if (typeof target === 'number') {
      return target;
    }
    const parsed = Number.parseInt(target, 10);
    return Number.isFinite(parsed) ? parsed : 0;
  }

  private curveCode(curve: number | 'linear' | 'exponential'): number {
    if (typeof curve === 'number') {
      return curve;
    }
    return curve === 'exponential' ? 1 : 0;
  }

  private ppqToApproxSample(ppq: number): number {
    return Math.max(0, Math.round(((ppq * 60) / 120) * this.sampleRate));
  }
}

export function registerSonareWorkletProcessor(name = 'sonare-worklet-processor'): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareWorkletProcessor extends Base {
    private bridge: SonareWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareWorkletProcessorOptions }) {
      super();
      const port = this.port;
      this.bridge = new SonareWorkletProcessor(options?.processorOptions ?? { sceneJson: '' }, {
        postMessage: (message) => port?.postMessage?.(message),
      });
      const onMessage = (event: { data: unknown }) => {
        if (isWorkletMessage(event.data)) {
          this.bridge.receiveMessage(event.data);
        }
      };
      if (port?.addEventListener) {
        port.addEventListener('message', onMessage);
        port.start?.();
      } else if (port) {
        port.onmessage = onMessage;
      }
    }

    process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
      return this.bridge.process(inputs, outputs);
    }
  }
  scope.registerProcessor(name, RegisteredSonareWorkletProcessor);
}

export function registerSonareRealtimeEngineWorkletProcessor(
  name = 'sonare-realtime-engine-processor',
): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareRealtimeEngineWorkletProcessor extends Base {
    private bridge?: SonareRealtimeEngineWorkletProcessor;
    private rtBridge?: SonareRtRealtimeEngineRuntime;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareRealtimeEngineWorkletProcessorOptions }) {
      super();
      const port = this.port;
      const processorOptions = options?.processorOptions ?? {};
      if (processorOptions.runtimeTarget === 'sonare-rt') {
        void this.initializeSonareRt(processorOptions, port);
      } else {
        this.bridge = new SonareRealtimeEngineWorkletProcessor(processorOptions, {
          postMessage: (message) => port?.postMessage?.(message),
          onMeter: (meter) => port?.postMessage?.(meter),
        });
      }
      const onMessage = (event: { data: unknown }) => {
        if (isEngineCommandRecord(event.data)) {
          this.bridge?.receiveCommand(event.data);
        }
      };
      if (port?.addEventListener) {
        port.addEventListener('message', onMessage);
        port.start?.();
      } else if (port) {
        port.onmessage = onMessage;
      }
    }

    process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
      if (this.rtBridge) {
        return this.rtBridge.process(inputs, outputs);
      }
      if (this.bridge) {
        return this.bridge.process(inputs, outputs);
      }
      const output = outputs[0];
      for (const channel of output ?? []) {
        channel.fill(0);
      }
      return true;
    }

    private async initializeSonareRt(
      options: SonareRealtimeEngineWorkletProcessorOptions,
      port?: WorkletPort,
    ): Promise<void> {
      try {
        if (!options.rtModuleUrl) {
          throw new Error('rtModuleUrl is required for sonare-rt AudioWorklet runtime.');
        }
        const memory = new WebAssembly.Memory({ initial: 1024, maximum: 1024, shared: true });
        const globalFactory = (
          globalThis as typeof globalThis & {
            SonareRtModuleFactory?: (options?: {
              wasmMemory?: WebAssembly.Memory;
              wasmBinary?: ArrayBuffer | Uint8Array;
              locateFile?: (path: string) => string;
            }) => Promise<SonareRtModule>;
          }
        ).SonareRtModuleFactory;
        const moduleFactory = globalFactory
          ? { default: globalFactory }
          : ((await import(options.rtModuleUrl)) as {
              default: (options?: {
                wasmMemory?: WebAssembly.Memory;
                wasmBinary?: ArrayBuffer | Uint8Array;
                locateFile?: (path: string) => string;
              }) => Promise<SonareRtModule>;
            });
        const module = await moduleFactory.default({
          wasmMemory: memory,
          wasmBinary: options.rtWasmBinary,
          locateFile: (path) => options.rtModuleUrl!.replace(/[^/]*$/, path),
        });
        this.rtBridge = new SonareRtRealtimeEngineRuntime({
          module,
          memory,
          sampleRate: options.sampleRate,
          blockSize: options.blockSize,
          channelCount: options.channelCount,
          commandSharedBuffer: options.commandSharedBuffer,
          commandRingCapacity: options.commandRingCapacity,
          telemetrySharedBuffer: options.telemetrySharedBuffer,
          telemetryRingCapacity: options.telemetryRingCapacity,
        });
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'sonare-rt' });
      } catch (error) {
        port?.postMessage?.({
          type: 'error',
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }
  }
  scope.registerProcessor(name, RegisteredSonareRealtimeEngineWorkletProcessor);
}
