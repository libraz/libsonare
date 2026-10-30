export const RING_CURSOR_RANGE = 0x1_0000_0000;

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
export function ringCursorSpan(capacity: number): number {
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
export function overwriteRingFirstReadable(
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

export function overwriteRingByteLength(
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
export function requireRingDimension(
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

export function requirePositiveCapacity(
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

export function requireProtocolVersion(
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

export function attachProtocolVersion(
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

export function requireNoUnpublishedRecords(
  header: Int32Array,
  versionSlot: number,
  capacity: number,
  name: string,
): void {
  if (Atomics.load(header, versionSlot) === 0 && loadSonareRingCursor(header, 0, capacity) !== 0) {
    throw new Error(`${name} has records without a compatible producer.`);
  }
}

export function validateOverwriteViews(
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

export function makeOverwriteSlotStates(
  sharedBuffer: SharedArrayBuffer,
  headerBytes: number,
  recordBytes: number,
  capacity: number,
): Int32Array {
  return new Int32Array(sharedBuffer, headerBytes + recordBytes * capacity, capacity);
}

export function recordOffset(cursor: number, capacity: number, recordBytes: number): number {
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

export function toUint32Slot(value: number | undefined, fallback: number, name: string): number {
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
