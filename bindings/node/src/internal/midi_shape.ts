import { assertDeclaredKeys, refuseType } from './entry.js';
import { isAbsent } from './value_guards.js';

const typedArrayTagGetter = Object.getOwnPropertyDescriptor(
  Object.getPrototypeOf(Uint8Array.prototype),
  Symbol.toStringTag,
)?.get;

function isUint32ArrayView(value: unknown): value is Uint32Array {
  return (
    ArrayBuffer.isView(value) &&
    typedArrayTagGetter !== undefined &&
    typedArrayTagGetter.call(value) === 'Uint32Array'
  );
}

export type MidiShapeField = {
  name: string;
  shape: 'number' | 'integer' | 'boolean' | 'object';
  optional?: boolean;
};

export function assertMidiFieldShape(
  value: unknown,
  argName: string,
  shape: MidiShapeField['shape'],
  optional = false,
): void {
  if (optional && isAbsent(value)) {
    return;
  }
  const valid =
    shape === 'object'
      ? typeof value === 'object' && value !== null && !Array.isArray(value)
      : shape === 'boolean'
        ? typeof value === 'boolean'
        : typeof value === 'number';
  if (!valid) {
    throw refuseType(`${argName} must be ${shape === 'integer' ? 'an integer' : `a ${shape}`}`);
  }
}

export function assertMidiShapeFields(
  record: Record<string, unknown>,
  fields: readonly MidiShapeField[],
): void {
  for (const field of fields) {
    assertMidiFieldShape(record[field.name], field.name, field.shape, field.optional);
  }
}

export function assertMidiWordListContainerShape(value: unknown, path = 'words'): void {
  if (!isUint32ArrayView(value) && !Array.isArray(value)) {
    throw refuseType(`${path} must be a Uint32Array or a number array`);
  }
}

export function assertBankShape(bank: unknown, optional = false): void {
  if (optional && isAbsent(bank)) {
    return;
  }
  if (typeof bank !== 'object' || bank === null || Array.isArray(bank)) {
    throw refuseType('bank must be a { msb, lsb } object');
  }
  const record = bank as { msb?: unknown; lsb?: unknown };
  assertDeclaredKeys(record, ['msb', 'lsb'], 'bank');
  assertMidiFieldShape(record.msb, 'bank.msb', 'integer');
  assertMidiFieldShape(record.lsb, 'bank.lsb', 'integer');
}
