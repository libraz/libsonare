import { assertEvenIntegerAtLeast, assertPositiveInteger } from '../validation.js';
import { refuse, refuseType } from './entry.js';

/**
 * The facade's single FFT-size/hop domain.
 *
 * This module is deliberately internal: sibling facade modules import it, but
 * nothing re-exports it, so it stays out of the package entry point and out of
 * the C-ABI parity surface (which is defined by the index's re-export closure).
 */

/**
 * Resolves and validates the `nFft` / `hopLength` pair every STFT-backed entry
 * point takes.
 *
 * The core FFT is mixed-radix, so any even size transforms exactly; only the
 * real one-sided spectrum's `n_fft / 2 + 1` bin layout needs the evenness. A
 * power-of-two restriction rejects sizes the C ABI and the native CLI accept,
 * which is what a second copy of this rule used to do: `hpss({ nFft: 1536 })`
 * worked while `hpssWithResidual({ nFft: 1536 })` threw, with the message
 * asserting a constraint this project had explicitly written down as untrue.
 * One implementation, so the two cannot disagree again.
 */
export function resolveFftOptions(
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  return resolveWithMinimum(nFft, hopLength, 2);
}

/** The largest even C `int`, the top of a resynthesizing effect's `nFft` domain. */
const MAX_EFFECT_N_FFT = 2147483646;

/** `SONARE_STFT_HOP_AUTO`: the hop native runs as a quarter of `nFft`. */
const STFT_HOP_AUTO = -1;

/**
 * The same pair for an effect that resynthesizes audio: the symmetric Hann
 * synthesis window is all zeros at two points, so the smallest size is 4. An
 * omitted hop reaches native as `SONARE_STFT_HOP_AUTO`, a quarter of `nFft`.
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
 * `SONARE_STFT_HOP_AUTO`, a quarter of `nFft`; a given hop is a positive integer.
 */
export function resolveResynthesisHop(hopLength: unknown): number {
  if (hopLength === undefined) {
    return STFT_HOP_AUTO;
  }
  assertPositiveInteger(hopLength, 'hopLength');
  return hopLength;
}

function resolveWithMinimum(
  nFft: unknown,
  hopLength: unknown,
  minNFft: number,
): { nFft: number; hopLength: number } {
  const resolvedNFft = nFft === undefined ? 2048 : nFft;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  assertEvenIntegerAtLeast(resolvedNFft, 'nFft', minNFft, 2 ** 30);
  assertPositiveInteger(resolvedHopLength, 'hopLength');
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}
