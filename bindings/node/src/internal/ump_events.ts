import type { ProjectMidiEvent } from '../types.js';
import { assertBoundedInteger, assertFiniteScalar, assertNibble } from '../validation.js';
import { refuse, refuseType } from './entry.js';

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
  assertFiniteScalar(ppq, 'ppq');
  if (ppq < 0) {
    throw refuse(`ppq must be non-negative`);
  }
  const g = assertNibble(group, 'group');
  const ch = assertNibble(channel, 'channel');
  const word0 =
    ((0x4 << 28) | (g << 24) | (opcode << 20) | (ch << 16) | (byte2 << 8) | byte3) >>> 0;
  return { ppq, word0, word1: word1 >>> 0 };
}

/** Integer in `[0, max]` worded like the shared live-push rule; a non-integer is a TypeError. */
export function programArg(value: number, argName: string, max: number): number {
  if (!Number.isInteger(value)) {
    throw refuseType(`${argName} must be an integer`);
  }
  if (value < 0 || value > max) {
    throw refuse(`${argName} must be in [0, ${max}]`);
  }
  return value;
}

export function midi2U32(value: number, argName: string): number {
  assertBoundedInteger(value, argName, 0, MIDI2_U32_MAX);
  return value;
}

export function midi2U16(value: number, argName: string): number {
  assertBoundedInteger(value, argName, 0, MIDI2_U16_MAX);
  return value;
}

export function midi2I32(value: number, argName: string): number {
  assertBoundedInteger(value, argName, -0x80000000, 0x7fffffff);
  return value;
}

export function midi2Byte(value: number, argName: string): number {
  assertBoundedInteger(value, argName, 0, 0xff);
  return value;
}
