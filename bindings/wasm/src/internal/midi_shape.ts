import { assertDeclaredKeys, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { isAbsent, isNonArrayObject, isPlainRecord } from './value_guards.js';

export type MidiShapeField = readonly [
  name: string,
  value: unknown,
  optional?: boolean,
  integer?: boolean,
];

const typedArrayTagGetter = Object.getOwnPropertyDescriptor(
  Object.getPrototypeOf(Uint8Array.prototype),
  Symbol.toStringTag,
)?.get;

function isUint8ArrayView(value: unknown): value is Uint8Array {
  return (
    ArrayBuffer.isView(value) &&
    typedArrayTagGetter !== undefined &&
    typedArrayTagGetter.call(value) === 'Uint8Array'
  );
}

/** Check only JS types for the whole argument list, before any domain check runs. */
export function assertMidiNumberShapes(fields: readonly MidiShapeField[]): void {
  for (const [name, value, optional = false, integer = false] of fields) {
    if (optional && isAbsent(value)) {
      continue;
    }
    if (typeof value !== 'number') {
      throw refuseType(`${name} must be ${integer ? 'an integer' : 'a number'}`);
    }
  }
}

export function assertMidiBooleanShape(value: unknown, name: string, optional = false): void {
  if (optional && isAbsent(value)) {
    return;
  }
  if (typeof value !== 'boolean') {
    throw refuseType(`${name} must be a boolean`);
  }
}

export function assertMidiStringShape(value: unknown, name: string, optional = false): void {
  if (optional && isAbsent(value)) {
    return;
  }
  if (typeof value !== 'string') {
    throw refuseType(`${name} must be a string`);
  }
}

export function assertMidiEventShape(value: unknown, path: string): void {
  if (!isNonArrayObject(value)) {
    throw refuseType(`${path} must be an object`);
  }
  assertDeclaredKeys(value, ['ppq', 'word0', 'word1', 'sysex'], path);
  const event = value as Record<string, unknown>;
  assertMidiNumberShapes([
    [`${path}.ppq`, event.ppq],
    [`${path}.word0`, event.word0],
    [`${path}.word1`, event.word1, true],
  ]);
  const sysex = event.sysex;
  if (!isAbsent(sysex) && !isUint8ArrayView(sysex)) {
    throw refuseType(`${path}.sysex must be a Uint8Array`);
  }
}

export function assertMidiEventListShapes(
  value: unknown,
  path = 'events',
): asserts value is unknown[] {
  if (!Array.isArray(value)) {
    throw refuseType(`${path} must be an array`);
  }
  const count = requireListBudget(value, path);
  for (let index = 0; index < count; index++) {
    assertMidiEventShape(value[index], `${path}[${index}]`);
  }
}

const MIDI_CC_BINDING_KEYS = [
  'ccNumber',
  'channel',
  'kind',
  'ccLsbNumber',
  'selectorMsb',
  'selectorLsb',
  'paramId',
  'minValue',
  'maxValue',
  'consume',
] as const;

export function assertMidiCcBindingShape(value: unknown, path: string): void {
  if (!isNonArrayObject(value)) {
    throw refuseType(`${path === '' ? 'binding' : path} must be an object`);
  }
  assertDeclaredKeys(value, MIDI_CC_BINDING_KEYS, path);
  const binding = value as Record<string, unknown>;
  const field = (name: string) => (path === '' ? name : `${path}.${name}`);
  assertMidiNumberShapes([
    [field('ccNumber'), binding.ccNumber, false, true],
    [field('channel'), binding.channel, true],
  ]);
  assertMidiStringShape(binding.kind, field('kind'), true);
  assertMidiNumberShapes([
    [field('ccLsbNumber'), binding.ccLsbNumber, true, true],
    [field('selectorMsb'), binding.selectorMsb, true, true],
    [field('selectorLsb'), binding.selectorLsb, true, true],
    [field('paramId'), binding.paramId],
    [field('minValue'), binding.minValue, true],
    [field('maxValue'), binding.maxValue, true],
  ]);
  assertMidiBooleanShape(binding.consume, field('consume'), true);
}

export function assertMidiCcBindingListShapes(value: unknown): asserts value is unknown[] {
  if (!Array.isArray(value)) {
    throw refuseType('bindings must be an array');
  }
  const count = requireListBudget(value, 'bindings');
  for (let index = 0; index < count; index++) {
    assertMidiCcBindingShape(value[index], `bindings[${index}]`);
  }
}

/** Type-only preflight for UMP words. The core owns count and numeric word domains. */
export function assertMidiWordListShapes(value: unknown, path = 'words'): number {
  if (!(value instanceof Uint32Array) && !Array.isArray(value)) {
    throw refuseType(`${path} must be a Uint32Array or a number array`);
  }
  const words = value as ArrayLike<unknown>;
  const count = requireListBudget(words, path);
  for (let index = 0; index < count; index++) {
    if (typeof words[index] !== 'number') {
      throw refuseType(`${path}[${index}] must be a number`);
    }
  }
  return count;
}

/** Type-only check for the WASM reader’s Uint8Array byte input. */
export function assertMidiByteShape(value: unknown, path: string): void {
  if (!isUint8ArrayView(value)) {
    throw refuseType(`${path} must be a Uint8Array`);
  }
}

export function assertMidiBankShape(value: unknown, path = 'bank', optional = true): void {
  if (optional && value === undefined) {
    return;
  }
  if (!isNonArrayObject(value)) {
    throw refuseType(`${path} must be a { msb, lsb } object`);
  }
  assertDeclaredKeys(value, ['msb', 'lsb'], path);
  const bank = value as Record<string, unknown>;
  assertMidiNumberShapes([
    [`${path}.msb`, bank.msb],
    [`${path}.lsb`, bank.lsb],
  ]);
}

export function assertMidiTrackIdShape(value: unknown, path = 'trackId'): void {
  if (typeof value !== 'number' && typeof value !== 'string') {
    throw refuseType(`${path} must be a number or string`);
  }
}

export function assertMidiTimingShape(
  positionName: string,
  position: unknown,
  options: unknown,
): void {
  assertMidiNumberShapes([[positionName, position, true]]);
  if (isAbsent(options)) {
    return;
  }
  if (!isPlainRecord(options)) {
    throw refuseType('options must be an object');
  }
  assertDeclaredKeys(options, ['contextTimeSec'], '');
  const contextTimeSec = (options as Record<string, unknown>).contextTimeSec;
  assertMidiNumberShapes([['contextTimeSec', contextTimeSec, true]]);
}
