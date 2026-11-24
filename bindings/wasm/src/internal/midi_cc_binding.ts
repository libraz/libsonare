import { assertU7 } from '../validation.js';
import {
  assertMidiCcBindingListShapes,
  assertMidiCcBindingShape,
  assertMidiEventListShapes,
  assertMidiEventShape,
} from './midi_shape.js';
import { isNonArrayObject } from './value_guards.js';

/**
 * The binding fields held as uint8_t by the C struct. Their types are checked
 * with every sibling field before any of these domain checks run.
 */
const MIDI_CC_BYTE_FIELDS = ['ccLsbNumber', 'selectorMsb', 'selectorLsb'] as const;

/** Shape-check a binding before applying the narrow byte-domain guard. */
export function assertMidiCcBindingBytes(binding: unknown, path = ''): void {
  assertMidiCcBindingShape(binding, path.replace(/\.$/, ''));
  if (!isNonArrayObject(binding)) {
    return;
  }
  for (const field of MIDI_CC_BYTE_FIELDS) {
    const value = binding[field];
    if (typeof value === 'number') {
      assertU7(value, path + field);
    }
  }
}

/** All binding types across the list precede any byte or core-domain check. */
export function assertMidiCcBindingListBytes(bindings: unknown): void {
  assertMidiCcBindingListShapes(bindings);
  for (const [index, binding] of bindings.entries()) {
    assertMidiCcBindingBytes(binding, `bindings[${index}].`);
  }
}

/** Check the event's full shape before any numeric domain is read. */
export function assertMidiCcEventPayload(event: unknown, path: string): void {
  assertMidiEventShape(event, path);
}

/** Check every event shape in a bounded learn list before core domain checks. */
export function assertMidiCcEventPayloads(events: unknown): void {
  assertMidiEventListShapes(events);
}
