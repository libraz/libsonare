import type { MidiBank } from '../public_types_midi_messages.js';
import { assertBoundedInteger, requestObject } from '../validation.js';
import { refuse, refuseType } from './entry.js';
import { isNonArrayObject } from './value_guards.js';

/** An integer in `[0, max]`: a non-number is a `TypeError`, anything else outside a `RangeError`. */
function programArg(value: unknown, argName: string, max: number): number {
  assertBoundedInteger(value, argName, 0, max);
  return value as number;
}

/**
 * Pack a MIDI 2.0 Program Change into its two UMP words, refusing a non-zero
 * bank MSB/LSB when `bankValid` is false (the receiver would ignore it).
 * `fnName` prefixes every refusal.
 */
export function packMidi2Program(
  group: number,
  channel: number,
  program: number,
  bankValid: boolean,
  bankMsb: number,
  bankLsb: number,
): [number, number] {
  const g = programArg(group, 'group', 15);
  const ch = programArg(channel, 'channel', 15);
  const p = programArg(program, 'program', 127);
  const msb = programArg(bankMsb, 'bank.msb', 127);
  const lsb = programArg(bankLsb, 'bank.lsb', 127);
  if (!bankValid && (msb !== 0 || lsb !== 0)) {
    throw refuse(`bank MSB and LSB must be 0 when bank-valid is false`);
  }
  const word0 = ((0x4 << 28) | (g << 24) | (0xc << 20) | (ch << 16) | (bankValid ? 1 : 0)) >>> 0;
  const word1 = ((p << 24) | (msb << 8) | lsb) >>> 0;
  return [word0, word1];
}

/**
 * Pack a Program Change whose bank is the `{ msb, lsb }` request of `requestName` (omitted or
 * null: no bank), refusing an undeclared bank key by that name.
 */
export function packMidi2ProgramRequest(
  requestName: 'pushMidiProgram' | 'pushMidiInputProgram',
  group: number,
  channel: number,
  program: number,
  bank: MidiBank | undefined,
): Uint32Array {
  const bankValid = bank !== undefined;
  if (bankValid) {
    if (!isNonArrayObject(bank)) {
      throw refuseType('bank must be a { msb, lsb } object');
    }
    requestObject(requestName, bank, 'bank', true);
  }
  return Uint32Array.from(
    packMidi2Program(group, channel, program, bankValid, bank?.msb ?? 0, bank?.lsb ?? 0),
  );
}
