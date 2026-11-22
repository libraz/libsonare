import type { ProjectMidiEvent } from '../types.js';
import { assertBoundedInteger, assertEventPpq, assertNibble } from '../validation.js';

const MIDI2_U16_MAX = 0xffff;
const MIDI2_U32_MAX = 0xffffffff;

// UMP MIDI-2.0 channel-voice packet (message type 0x4). Canonical layout is
// sonare::midi::make_midi2_* (C-ABI sonare_midi2_*); the golden vectors in
// midi2-ump.test.ts lock this copy against it.
export function midi2Event(
  ppq: number,
  group: number,
  opcode: number,
  channel: number,
  byte2: number,
  byte3: number,
  word1: number,
): ProjectMidiEvent {
  assertEventPpq(ppq);
  const g = assertNibble(group, 'group');
  const ch = assertNibble(channel, 'channel');
  const word0 =
    ((0x4 << 28) | (g << 24) | (opcode << 20) | (ch << 16) | (byte2 << 8) | byte3) >>> 0;
  return { ppq, word0, word1: word1 >>> 0 };
}

/** An integer in `[0, max]`: a non-number is a `TypeError`, anything else outside a `RangeError`. */
export function programArg(value: unknown, argName: string, max: number): number {
  assertBoundedInteger(value, argName, 0, max);
  return value as number;
}

export function midi2U32(value: unknown, argName: string): number {
  assertBoundedInteger(value, argName, 0, MIDI2_U32_MAX);
  return value as number;
}

export function midi2U16(value: unknown, argName: string): number {
  assertBoundedInteger(value, argName, 0, MIDI2_U16_MAX);
  return value as number;
}

export function midi2I32(value: unknown, argName: string): number {
  assertBoundedInteger(value, argName, -0x80000000, 0x7fffffff);
  return value as number;
}

export function midi2Byte(value: unknown, argName: string): number {
  assertBoundedInteger(value, argName, 0, 0xff);
  return value as number;
}
