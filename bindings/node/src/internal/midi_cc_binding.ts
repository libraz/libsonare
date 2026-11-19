import { assertU7 } from '../validation.js';

/** The binding fields that carry a MIDI data byte; the C struct holds them as `uint8_t`. */
const MIDI_CC_BYTE_FIELDS = ['ccNumber', 'ccLsbNumber', 'selectorMsb', 'selectorLsb'] as const;

/**
 * Refuses a binding's data byte outside [0, 127], which the C struct would hold
 * and the MIDI domain cannot carry. A wrong type stays the native reader's refusal.
 */
export function assertMidiCcBindingBytes(binding: unknown, path = ''): void {
  if (typeof binding !== 'object' || binding === null) {
    return;
  }
  for (const field of MIDI_CC_BYTE_FIELDS) {
    const value = (binding as Record<string, unknown>)[field];
    if (typeof value === 'number') {
      assertU7(value, `${path}${field}`);
    }
  }
}

/** {@link assertMidiCcBindingBytes} over a `bindings` list, naming each by its index. */
export function assertMidiCcBindingListBytes(bindings: unknown): void {
  if (Array.isArray(bindings)) {
    for (const [index, binding] of bindings.entries()) {
      assertMidiCcBindingBytes(binding, `bindings[${index}].`);
    }
  }
}
