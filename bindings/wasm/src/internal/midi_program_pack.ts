import { refuse, refuseType } from './entry.js';

/** Integer in `[0, max]`, worded like the shared live-push rule; a non-integer is a TypeError. */
function programArg(value: number, argName: string, max: number): number {
  if (!Number.isInteger(value)) {
    throw refuseType(`${argName} must be an integer`);
  }
  if (value < 0 || value > max) {
    throw refuse(`${argName} must be in [0, ${max}]`);
  }
  return value;
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
  const msb = programArg(bankMsb, 'bank_msb', 127);
  const lsb = programArg(bankLsb, 'bank_lsb', 127);
  if (!bankValid && (msb !== 0 || lsb !== 0)) {
    throw refuse(`bank MSB and LSB must be 0 when bank-valid is false`);
  }
  const word0 = ((0x4 << 28) | (g << 24) | (0xc << 20) | (ch << 16) | (bankValid ? 1 : 0)) >>> 0;
  const word1 = ((p << 24) | (msb << 8) | lsb) >>> 0;
  return [word0, word1];
}
