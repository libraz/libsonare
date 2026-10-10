import { assertNibble, assertU7 } from './validation.js';

/**
 * Pack a MIDI 2.0 Program Change into its two UMP words, refusing a non-zero
 * bank MSB/LSB when `bankValid` is false (the receiver would ignore it).
 * `fnName` prefixes every refusal.
 */
export function packMidi2Program(
  fnName: string,
  group: number,
  channel: number,
  program: number,
  bankValid: boolean,
  bankMsb: number,
  bankLsb: number,
): [number, number] {
  const g = assertNibble(fnName, group, 'group');
  const ch = assertNibble(fnName, channel, 'channel');
  const p = assertU7(fnName, program, 'program');
  const msb = assertU7(fnName, bankMsb, 'bankMsb');
  const lsb = assertU7(fnName, bankLsb, 'bankLsb');
  if (!bankValid && (msb !== 0 || lsb !== 0)) {
    throw new RangeError(`${fnName}: bank MSB and LSB must be 0 when bank-valid is false`);
  }
  const word0 = ((0x4 << 28) | (g << 24) | (0xc << 20) | (ch << 16) | (bankValid ? 1 : 0)) >>> 0;
  const word1 = ((p << 24) | (msb << 8) | lsb) >>> 0;
  return [word0, word1];
}
