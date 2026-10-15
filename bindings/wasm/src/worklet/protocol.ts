import type { EngineMeterTelemetryWide, EngineTelemetry } from '../index.js';

const ENGINE_MIXER_TARGET_BASE = 0x4d580000;
export const ENGINE_MIXER_PARAM_FADER_DB = 1;
export const ENGINE_MIXER_PARAM_PAN = 2;

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
// Clip-page request ring header: [writeIndex, readIndex, capacity,
// recordUint32s, dropped]. Records are [clipId, pageIndex]. This is an SPSC
// queue: the AudioWorklet is the sole writer and the main thread is the sole
// reader. Unlike the telemetry rings, it must not overwrite unread requests:
// the producer increments `dropped` when the fixed queue is full.
export const SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS = 5;
export const SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S = 2;
// External MIDI records: destination ID, render frame (low 32 bits), packed
// MIDI-1 bytes (little-endian), byte count, render frame (high bits). SPSC
// worklet -> main-thread ring.
export const SONARE_EXTERNAL_MIDI_RING_HEADER_INTS = 5;
export const SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S = 5;

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

export interface SharedScopeRingWriter {
  header: Int32Array;
  records: Float32Array;
  slotStates: Int32Array;
  capacity: number;
  bands: number;
  maxPoints: number;
  recordFloats: number;
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

export interface SonareClipPageRequest {
  clipId: number;
  pageIndex: number;
}

export interface SonareClipPageRequestRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SharedClipPageRequestRingWriter {
  header: Int32Array;
  records: Uint32Array;
  capacity: number;
}

export interface SonareClipPageRequestRingReadResult {
  requests: SonareClipPageRequest[];
  /** Cumulative uint32 drop count for native and SAB-ring overflows. */
  dropped: number;
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

/**
 * Magnitude to dB with the shared finite floor.
 *
 * `-Infinity` is not a usable value for a meter reading: it propagates NaN
 * through the usual `(db + 60) / 60 * height` bar geometry, serializes to
 * `null` through `JSON.stringify`, and poisons `Math.min`/`Math.max`
 * aggregation. The native meters have always floored at `kFloorDb`, so a
 * producer emitting `-Infinity` also made the two worklets disagree about
 * silence.
 */
export function toDb(value: number): number {
  return value > 0 ? Math.max(20 * Math.log10(value), SONARE_FLOOR_DB) : SONARE_FLOOR_DB;
}

const RING_CURSOR_RANGE = 0x1_0000_0000;

// Overwrite records use one atomic state word per slot. The high bit excludes
// writers while the remaining bits count independent readers. The count is
// intentionally bounded below 2^31-1: allowing it to wrap would recreate the
// writer sentinel and admit a concurrent producer.
const OVERWRITE_WRITER_BIT = 0x8000_0000;
const OVERWRITE_READER_MASK = 0x7fff_ffff;

/**
 * SAB ring cursors are free-running record counts kept in an Int32Array header
 * slot and read back as uint32. They wrap at the largest multiple of the
 * capacity that fits in 32 bits, so slot order stays continuous across the wrap
 * for any capacity. Every cursor load, store and distance goes through here.
 */
function ringCursorSpan(capacity: number): number {
  return capacity * Math.floor(RING_CURSOR_RANGE / capacity);
}

/** Loads the cursor held in header `slot` of a ring with `capacity` records. */
export function loadSonareRingCursor(header: Int32Array, slot: number, capacity: number): number {
  return (Atomics.load(header, slot) >>> 0) % ringCursorSpan(capacity);
}

/** Publishes `cursor` advanced by `count` records into header `slot`. */
export function storeSonareRingCursor(
  header: Int32Array,
  slot: number,
  cursor: number,
  capacity: number,
  count = 1,
): void {
  Atomics.store(header, slot, ((cursor + count) % ringCursorSpan(capacity)) | 0);
}

/** Number of records from cursor `from` up to cursor `to`. */
export function sonareRingCursorDistance(from: number, to: number, capacity: number): number {
  const span = ringCursorSpan(capacity);
  return (((to - from) % span) + span) % span;
}

/** Record index of `cursor` within the ring. */
export function sonareRingCursorSlot(cursor: number, capacity: number): number {
  return cursor % capacity;
}

/**
 * Claims the current overwrite slot once. A failed claim drops the candidate
 * publication immediately; producers never wait for a reader.
 */
export function tryBeginOverwriteWrite(
  header: Int32Array,
  slotStates: Int32Array,
  capacity: number,
): number | undefined {
  const cursor = loadSonareRingCursor(header, 0, capacity);
  const slot = sonareRingCursorSlot(cursor, capacity);
  return Atomics.compareExchange(slotStates, slot, 0, OVERWRITE_WRITER_BIT) === 0
    ? cursor
    : undefined;
}

/** Publishes the cursor and releases only the writer bit for one slot. */
export function commitOverwriteWrite(
  header: Int32Array,
  slotStates: Int32Array,
  capacity: number,
  cursor: number,
): void {
  const slot = sonareRingCursorSlot(cursor, capacity);
  storeSonareRingCursor(header, 0, cursor, capacity);
  Atomics.and(slotStates, slot, OVERWRITE_READER_MASK);
}

/**
 * Takes one of two immediate reader attempts. Reader counts commute with one
 * another, so independent readers do not consume each other's retry budget.
 */
export function tryAcquireOverwriteRead(
  slotStates: Int32Array,
  capacity: number,
  cursor: number,
): boolean {
  const slot = sonareRingCursorSlot(cursor, capacity);
  for (let attempt = 0; attempt < 2; attempt++) {
    const previous = Atomics.add(slotStates, slot, 1) >>> 0;
    if (previous === OVERWRITE_READER_MASK) {
      Atomics.sub(slotStates, slot, 1);
      throw new RangeError('overwrite-ring reader count overflow');
    }
    if ((previous & OVERWRITE_WRITER_BIT) === 0) {
      return true;
    }
    Atomics.sub(slotStates, slot, 1);
  }
  return false;
}

/** Releases exactly one successful reader acquisition. */
export function releaseOverwriteRead(
  slotStates: Int32Array,
  capacity: number,
  cursor: number,
): void {
  Atomics.sub(slotStates, sonareRingCursorSlot(cursor, capacity), 1);
}

/**
 * Cursor of the oldest record an overwrite reader still holds: `readIndex`
 * when it is within the last `capacity` records before `writeIndex`, else the
 * oldest record not yet overwritten.
 */
function overwriteRingFirstReadable(
  readIndex: number,
  writeIndex: number,
  capacity: number,
): number {
  const lag = sonareRingCursorDistance(readIndex, writeIndex, capacity);
  return lag <= capacity
    ? readIndex
    : (writeIndex - capacity + ringCursorSpan(capacity)) % ringCursorSpan(capacity);
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function overwriteRingByteLength(
  headerBytes: number,
  recordBytes: number,
  capacity: number,
): number {
  return headerBytes + capacity * recordBytes + capacity * Int32Array.BYTES_PER_ELEMENT;
}

/**
 * A ring dimension a caller passes when creating or sizing a ring: refused when
 * it is not a number (TypeError) or not a whole number in [minimum, maximum]
 * (RangeError), never floored or clamped into one.
 */
function requireRingDimension(
  value: unknown,
  name: string,
  minimum: number,
  maximum = Number.MAX_SAFE_INTEGER,
): number {
  if (typeof value !== 'number') {
    throw new TypeError(`${name} must be a number`);
  }
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new RangeError(`${name} must be an integer in [${minimum}, ${maximum}]`);
  }
  return value;
}

function requirePositiveCapacity(
  existingCapacity: number,
  fallbackCapacity: number | undefined,
  name: string,
): number {
  if (
    fallbackCapacity !== undefined &&
    (!Number.isSafeInteger(fallbackCapacity) || fallbackCapacity < 1)
  ) {
    throw new RangeError(`${name} must be a positive integer`);
  }
  if (existingCapacity !== 0 && (!Number.isSafeInteger(existingCapacity) || existingCapacity < 1)) {
    throw new RangeError(`${name} must be a positive integer`);
  }
  const capacity = existingCapacity || fallbackCapacity;
  if (capacity === undefined || capacity < 1) {
    throw new RangeError(`${name} must be a positive integer`);
  }
  if (fallbackCapacity !== undefined && existingCapacity !== 0 && capacity !== fallbackCapacity) {
    throw new RangeError(`${name} does not match the shared-buffer descriptor`);
  }
  return capacity;
}

function requireProtocolVersion(
  header: Int32Array,
  versionSlot: number,
  expectedVersion: number,
  name: string,
): void {
  const version = Atomics.load(header, versionSlot);
  if (version !== 0 && version !== expectedVersion) {
    throw new Error(
      `${name} protocol version ${version} is incompatible; expected ${expectedVersion}.`,
    );
  }
}

function attachProtocolVersion(
  header: Int32Array,
  versionSlot: number,
  expectedVersion: number,
  name: string,
): void {
  if (Atomics.load(header, versionSlot) === expectedVersion) {
    return;
  }
  const previous = Atomics.compareExchange(header, versionSlot, 0, expectedVersion);
  if (previous !== 0 && previous !== expectedVersion) {
    throw new Error(
      `${name} protocol version ${previous} is incompatible; expected ${expectedVersion}.`,
    );
  }
}

function requireNoUnpublishedRecords(
  header: Int32Array,
  versionSlot: number,
  capacity: number,
  name: string,
): void {
  if (Atomics.load(header, versionSlot) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw new Error(`${name} has records without a compatible producer.`);
  }
}

function validateOverwriteViews(
  sharedBuffer: SharedArrayBuffer,
  header: Int32Array,
  records: Float32Array | DataView,
  slotStates: Int32Array,
  headerBytes: number,
  recordBytes: number,
  capacity: number,
  name: string,
  expectedTrailingBytes = 0,
): void {
  if (!Number.isSafeInteger(capacity) || capacity < 1) {
    throw new RangeError(`${name} capacity must be a positive integer.`);
  }
  const stateOffset = headerBytes + recordBytes * capacity;
  if (
    sharedBuffer.byteLength !==
      overwriteRingByteLength(headerBytes, recordBytes, capacity) + expectedTrailingBytes ||
    header.buffer !== sharedBuffer ||
    header.byteOffset !== 0 ||
    header.byteLength !== headerBytes ||
    records.buffer !== sharedBuffer ||
    records.byteOffset !== headerBytes ||
    records.byteLength !== recordBytes * capacity ||
    slotStates.buffer !== sharedBuffer ||
    slotStates.byteOffset !== stateOffset ||
    slotStates.length !== capacity
  ) {
    throw new Error(`${name} shared buffer has an invalid layout.`);
  }
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

function makeOverwriteSlotStates(
  sharedBuffer: SharedArrayBuffer,
  headerBytes: number,
  recordBytes: number,
  capacity: number,
): Int32Array {
  return new Int32Array(sharedBuffer, headerBytes + recordBytes * capacity, capacity);
}

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

export function sonareClipPageRequestRingBufferByteLength(capacity: number): number {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  return (
    SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S * Uint32Array.BYTES_PER_ELEMENT
  );
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

export function createSonareClipPageRequestRingBuffer(
  capacity = 128,
): SonareClipPageRequestRingBuffer {
  const clampedCapacity = requireRingDimension(capacity, 'capacity', 1);
  const sharedBuffer = new SharedArrayBuffer(
    sonareClipPageRequestRingBufferByteLength(clampedCapacity),
  );
  const ring = clipPageRequestRingFromSharedBuffer(sharedBuffer, clampedCapacity);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, 0);
  Atomics.store(ring.header, 2, ring.capacity);
  Atomics.store(ring.header, 3, SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S);
  Atomics.store(ring.header, 4, 0);
  return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
}

/**
 * Writes one request without allocating. Invalid uint32 coordinates and a full
 * ring are counted as drops so the main thread can surface a bounded-loss
 * diagnostic without receiving a worklet postMessage.
 */
export function pushSonareClipPageRequestRingBuffer(
  ring: SharedClipPageRequestRingWriter,
  clipId: number,
  pageIndex: number,
): boolean {
  if (
    !Number.isSafeInteger(clipId) ||
    clipId < 0 ||
    clipId > 0xffff_ffff ||
    !Number.isSafeInteger(pageIndex) ||
    pageIndex < 0 ||
    pageIndex > 0xffff_ffff
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
    sonareRingCursorSlot(writeIndex, ring.capacity) * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S;
  Atomics.store(ring.records, offset, clipId);
  Atomics.store(ring.records, offset + 1, pageIndex);
  // Publish only after both scalar fields have been written.
  storeSonareRingCursor(ring.header, 0, writeIndex, ring.capacity);
  return true;
}

/** Drains the SPSC request ring on the main thread. Object creation is here, never in process(). */
export function readSonareClipPageRequestRingBuffer(
  ring: SonareClipPageRequestRingBuffer,
): SonareClipPageRequestRingReadResult {
  const readIndex = loadSonareRingCursor(ring.header, 1, ring.capacity);
  const writeIndex = loadSonareRingCursor(ring.header, 0, ring.capacity);
  const pending = Math.min(
    ring.capacity,
    sonareRingCursorDistance(readIndex, writeIndex, ring.capacity),
  );
  const requests: SonareClipPageRequest[] = [];
  for (let i = 0; i < pending; i++) {
    const cursor = (readIndex + i) % ringCursorSpan(ring.capacity);
    const offset =
      sonareRingCursorSlot(cursor, ring.capacity) * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S;
    requests.push({
      clipId: Atomics.load(ring.records, offset) >>> 0,
      pageIndex: Atomics.load(ring.records, offset + 1) >>> 0,
    });
  }
  storeSonareRingCursor(ring.header, 1, readIndex, ring.capacity, pending);
  return { requests, dropped: Atomics.load(ring.header, 4) >>> 0 };
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

export function clipPageRequestRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedClipPageRequestRingWriter {
  const headerBytes = SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 2);
  const capacity = requirePositiveCapacity(
    existingCapacity,
    fallbackCapacity,
    'clipPageRequestRingCapacity',
  );
  const minBytes = sonareClipPageRequestRingBufferByteLength(capacity);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('clipPageRequestSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 2, capacity);
  Atomics.store(header, 3, SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S);
  return {
    header,
    records: new Uint32Array(
      sharedBuffer,
      headerBytes,
      capacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
    ),
    capacity,
  };
}

function recordOffset(cursor: number, capacity: number, recordBytes: number): number {
  return sonareRingCursorSlot(cursor, capacity) * recordBytes;
}

/**
 * The domain of a uint32 record slot: no fraction, no sign, no overflow.
 *
 * `setUint32` folds anything outside it into a legal value, so a negative id
 * becomes a large one and 2**32 becomes 0. An absent value is out of domain;
 * a slot with a default resolves it before asking.
 */
export function isUint32Slot(value: number | undefined): boolean {
  return (
    typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= 0xffff_ffff
  );
}

function toUint32Slot(value: number | undefined, fallback: number, name: string): number {
  const resolved = value ?? fallback;
  if (!isUint32Slot(resolved)) {
    throw new RangeError(`${name} must be an integer within [0, 4294967295]`);
  }
  return resolved;
}

export function toSafeInteger(value: number | bigint | undefined, fallback: number): number {
  const resolved = typeof value === 'bigint' ? Number(value) : value;
  if (resolved === undefined) {
    return fallback;
  }
  if (!Number.isSafeInteger(resolved)) {
    throw new RangeError('64-bit ring values must be safe integers');
  }
  return resolved;
}

/** Store a signed 64-bit safe integer as two 32-bit words without BigInt. */
export function writeInt64Words(view: DataView, offset: number, value: number): void {
  const integer = toSafeInteger(value, 0);
  writePreparedInt64Words(view, offset, integer);
}

/** Store a previously validated safe integer; all operations are primitive. */
export function writePreparedInt64Words(view: DataView, offset: number, integer: number): void {
  view.setUint32(offset, integer >>> 0, true);
  view.setInt32(offset + 4, Math.floor(integer / 0x100000000), true);
}

/** Reconstruct a signed 64-bit safe integer stored by {@link writeInt64Words}. */
export function readInt64Words(view: DataView, offset: number): number {
  return view.getInt32(offset + 4, true) * 0x100000000 + view.getUint32(offset, true);
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
  return value > 1.0e-12 ? 20 * Math.log10(value) : -120;
}
