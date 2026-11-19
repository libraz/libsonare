import { assertBoundedInteger } from '../validation.js';
import { refuse } from './entry.js';

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
