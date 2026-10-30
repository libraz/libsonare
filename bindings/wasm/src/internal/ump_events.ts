import type { ProjectMidiEvent } from '../project_types.js';
import { assertBoundedInteger, assertNibble } from '../validation.js';

const MIDI2_U16_MAX = 0xffff;
const MIDI2_U32_MAX = 0xffffffff;

// UMP MIDI-2.0 channel-voice packet (message type 0x4). Canonical layout is
// sonare::midi::make_midi2_* (C-ABI sonare_midi2_*); the golden vectors in
// midi2-ump.test.ts lock this copy against it.
export function midi2Event(
  fnName: string,
  ppq: number,
  group: number,
  opcode: number,
  channel: number,
  byte2: number,
  byte3: number,
  word1: number,
): ProjectMidiEvent {
  if (!Number.isFinite(ppq) || ppq < 0) {
    throw new RangeError(`${fnName}: ppq must be a non-negative finite number`);
  }
  const g = assertNibble(fnName, group, 'group');
  const ch = assertNibble(fnName, channel, 'channel');
  const word0 =
    ((0x4 << 28) | (g << 24) | (opcode << 20) | (ch << 16) | (byte2 << 8) | byte3) >>> 0;
  return { ppq, data0: word0, data1: word1 >>> 0 };
}

export function midi2U32(fnName: string, value: number, argName: string): number {
  assertBoundedInteger(fnName, value, argName, 0, MIDI2_U32_MAX);
  return value;
}

export function midi2U16(fnName: string, value: number, argName: string): number {
  assertBoundedInteger(fnName, value, argName, 0, MIDI2_U16_MAX);
  return value;
}

export function midi2I32(fnName: string, value: number, argName: string): number {
  assertBoundedInteger(fnName, value, argName, -0x80000000, 0x7fffffff);
  return value;
}

export function midi2Byte(fnName: string, value: number, argName: string): number {
  assertBoundedInteger(fnName, value, argName, 0, 0xff);
  return value;
}
