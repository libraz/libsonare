import type { ProjectMidiEvent } from '../project_types.js';
import { assertBoundedInteger, assertNibble } from '../validation.js';
import { SONARE_MAX_PUBLIC_PPQ as MAX_PUBLIC_PPQ } from '../worklet/engine-rings.js';
import { refuse } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { UINT32_MAX } from './value_guards.js';

const MIDI2_U16_MAX = 0xffff;

/** Most words one UMP packet holds. */
export const UMP_MAX_WORDS = 4;

/** Refuses a word list that holds no word or more than one packet's worth. */
export function assertUmpWordCount(words: ArrayLike<number>): void {
  requireListBudget(words, 'words');
  if (words.length < 1 || words.length > UMP_MAX_WORDS) {
    throw refuse(`words must hold 1 to ${UMP_MAX_WORDS} words`);
  }
}

/** Refuses an oversized word array before it is copied; a short one is left to the engine's own refusal. */
export function boundedUmpWords(words: number[]): number[] {
  if (words.length > UMP_MAX_WORDS) {
    assertUmpWordCount(words);
  }
  return words;
}

/** Refuses a ppq that is not a non-negative finite number. */
export function assertEventPpq(ppq: number): void {
  if (!(Number.isFinite(ppq) && ppq >= 0 && ppq <= MAX_PUBLIC_PPQ)) {
    throw refuse(`ppq must be finite and in [0, 1e12]`);
  }
}

/** Packs the first UMP word of a channel-voice packet. */
export function umpWord0(
  messageType: number,
  group: number,
  opcode: number,
  channel: number,
  byte2: number,
  byte3: number,
): number {
  return (
    ((messageType << 28) |
      (group << 24) |
      (opcode << 20) |
      (channel << 16) |
      (byte2 << 8) |
      byte3) >>>
    0
  );
}

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
  const word0 = umpWord0(0x4, g, opcode, ch, byte2, byte3);
  return { ppq, word0, word1: word1 >>> 0 };
}

export function midi2U32(value: unknown, argName: string): number {
  assertBoundedInteger(value, argName, 0, UINT32_MAX);
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
