import { assertPositiveInteger } from '../validation.js';
import { refuse, refuseType } from './entry.js';

/**
 * The facade's FFT-size/hop defaults and the resynthesis domains.
 *
 * This module is deliberately internal: sibling facade modules import it, but
 * nothing re-exports it, so it stays out of the package entry point and out of
 * the C-ABI parity surface (which is defined by the index's re-export closure).
 */

/**
 * Resolves the `nFft` / `hopLength` pair every STFT-backed entry point takes.
 *
 * Only the defaults are resolved here: the native reader refuses a value outside
 * the core's domain, so the bound is stated once for every surface.
 */
export function resolveFftOptions(
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  return {
    nFft: (nFft === undefined ? 2048 : nFft) as number,
    hopLength: (hopLength === undefined ? 512 : hopLength) as number,
  };
}

/** The largest even C `int`, the top of a resynthesizing effect's `nFft` domain. */
const MAX_EFFECT_N_FFT = 2147483646;

/** A hop of 0 is the automatic hop native runs as a quarter of `nFft`. */
const AUTO_HOP = 0;

/**
 * The same pair for an effect that resynthesizes audio: the symmetric Hann
 * synthesis window is all zeros at two points, so the smallest size is 4. An
 * omitted hop reaches native as 0, the automatic hop of a quarter of `nFft`.
 */
export function resolveEffectFftOptions(
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  const size = nFft === undefined ? 2048 : nFft;
  if (typeof size !== 'number') {
    throw refuseType('nFft must be an integer');
  }
  if (!Number.isInteger(size) || size < 4 || size > MAX_EFFECT_N_FFT || size % 2 !== 0) {
    throw refuse(`nFft must be an even integer in [4, ${MAX_EFFECT_N_FFT}]`);
  }
  return { nFft: size, hopLength: resolveResynthesisHop(hopLength) };
}

/**
 * The hop of a resynthesizing call as native reads it: omitted is
 * 0, the automatic hop of a quarter of `nFft`; a given hop is a positive integer.
 */
export function resolveResynthesisHop(hopLength: unknown): number {
  if (hopLength === undefined) {
    return AUTO_HOP;
  }
  assertPositiveInteger(hopLength, 'hopLength');
  return hopLength;
}
