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
  fnName: string,
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  const resolvedNFft = nFft === undefined ? 2048 : nFft;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  if (typeof resolvedNFft !== 'number' || !Number.isInteger(resolvedNFft)) {
    throw new TypeError(`${fnName}: nFft must be an integer`);
  }
  if (resolvedNFft < 2 || resolvedNFft > 2 ** 30 || resolvedNFft % 2 !== 0) {
    throw new RangeError(`${fnName}: nFft must be an even integer >= 2`);
  }
  if (typeof resolvedHopLength !== 'number' || !Number.isInteger(resolvedHopLength)) {
    throw new TypeError(`${fnName}: hopLength must be an integer`);
  }
  if (resolvedHopLength <= 0 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: hopLength must be a positive integer`);
  }
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}
