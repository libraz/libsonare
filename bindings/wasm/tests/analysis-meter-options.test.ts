/**
 * Meter/tempo analysis option tests for the WASM binding.
 *
 * Covers the downbeat fields on the unified `analyze()` result and the
 * adaptive-tempo / meter-candidate options: their defaults, the validation the
 * core applies to them, and that a widened candidate set actually reaches the
 * meter estimator.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { analyze, init } from '../src/index';

const SR = 22050;

/**
 * Builds a percussive click track with the first beat of every measure
 * accented, which is the accent pattern the meter estimator scores.
 *
 * @param numerator - Beats per measure.
 * @param measures - Number of measures to render.
 * @param weakAmp - Amplitude of the unaccented beats relative to the downbeat.
 * @param beatSec - Beat period in seconds.
 * @returns The generated mono click track.
 */
function clickTrack(
  numerator: number,
  measures: number,
  weakAmp = 0.28,
  beatSec = 0.4,
): Float32Array {
  const beats = numerator * measures;
  const out = new Float32Array(Math.round(SR * beatSec * beats));
  const clickLen = Math.round(SR * 0.06);
  // Deterministic LCG noise burst: broadband so the onset detector sees every
  // click, and reproducible so the meter result does not vary run to run.
  let seed = 12345;
  const noise = new Float32Array(clickLen);
  for (let i = 0; i < clickLen; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    const white = (seed / 0x3fffffff - 1) * Math.exp(-i / (SR * 0.012));
    noise[i] = white;
  }
  for (let b = 0; b < beats; b++) {
    const amp = b % numerator === 0 ? 1.0 : weakAmp;
    const start = Math.round(b * beatSec * SR);
    for (let i = 0; i < clickLen && start + i < out.length; i++) {
      out[start + i] += 0.6 * amp * noise[i];
    }
  }
  return out;
}

describe('WASM analyze() downbeats and meter options', () => {
  const samples = clickTrack(4, 8);

  beforeAll(async () => {
    await init();
  });

  describe('downbeat fields', () => {
    it('exposes downbeatIndices as indices into beats plus a downbeatPhase', () => {
      const result = analyze(samples, SR);

      expect(Array.isArray(result.downbeatIndices)).toBe(true);
      for (const index of result.downbeatIndices) {
        expect(Number.isInteger(index)).toBe(true);
        expect(index).toBeGreaterThanOrEqual(0);
        expect(index).toBeLessThan(result.beats.length);
      }
      expect(Number.isInteger(result.downbeatPhase)).toBe(true);
      expect(result.downbeatPhase).toBeGreaterThanOrEqual(0);
      expect(result.downbeatPhase).toBeLessThan(result.timeSignature.numerator);
    });
  });

  describe('option defaults', () => {
    it('matches the core defaults when the options are omitted', () => {
      const implicit = analyze(samples, SR);
      const explicit = analyze(samples, SR, {
        adaptiveTempo: false,
        tempoUpdateIntervalBeats: 8,
        meterCandidateNumerators: [3, 4, 6],
        meterDenominator: 4,
      });

      expect(explicit).toEqual(implicit);
    });

    it('accepts adaptiveTempo: true', () => {
      const result = analyze(samples, SR, { adaptiveTempo: true });
      expect(Number.isFinite(result.bpm)).toBe(true);
      expect(result.beats.length).toBeGreaterThan(0);
    });
  });

  describe('option validation', () => {
    it('rejects an empty meterCandidateNumerators list', () => {
      expect(() => analyze(samples, SR, { meterCandidateNumerators: [] })).toThrow();
    });

    it('rejects more than 16 meterCandidateNumerators entries', () => {
      const seventeen = Array.from({ length: 17 }, (_, i) => 2 + (i % 30));
      expect(() => analyze(samples, SR, { meterCandidateNumerators: seventeen })).toThrow();
    });

    it('rejects a meterCandidateNumerators entry below 2', () => {
      expect(() => analyze(samples, SR, { meterCandidateNumerators: [1] })).toThrow();
    });

    it('rejects a meterCandidateNumerators entry above 32', () => {
      expect(() => analyze(samples, SR, { meterCandidateNumerators: [33] })).toThrow();
    });

    it('rejects a non-finite meterCandidateNumerators entry', () => {
      expect(() => analyze(samples, SR, { meterCandidateNumerators: [Number.NaN] })).toThrow();
    });

    it('rejects a meterDenominator that is not a power of two', () => {
      expect(() => analyze(samples, SR, { meterDenominator: 3 })).toThrow();
    });

    it('rejects a non-positive tempoUpdateIntervalBeats', () => {
      expect(() => analyze(samples, SR, { tempoUpdateIntervalBeats: 0 })).toThrow();
    });
  });

  describe('meterCandidateNumerators reaches the estimator', () => {
    it('resolves a 5-beat accent pattern to 5 only when 5 is a candidate', () => {
      const fiveBeat = clickTrack(5, 7);
      const widened = analyze(fiveBeat, SR, { meterCandidateNumerators: [5, 7] });
      const defaults = analyze(fiveBeat, SR);

      expect(widened.timeSignature.numerator).toBe(5);
      expect(defaults.timeSignature.numerator).not.toBe(5);
    });

    it('resolves a 7-beat accent pattern to 7 only when 7 is a candidate', () => {
      // A 7-beat pattern needs more measures and a deeper accent than the
      // 5-beat one: over five measures the 35 beats are also divisible by 5, so
      // a 5-beat comb fits the accents and wins. Seven measures plus a deeper
      // accent leave the 7-beat period as the only periodicity that survives.
      const sevenBeat = clickTrack(7, 7, 0.15);
      const widened = analyze(sevenBeat, SR, { meterCandidateNumerators: [5, 7] });
      const defaults = analyze(sevenBeat, SR);

      expect(widened.timeSignature.numerator).toBe(7);
      expect(defaults.timeSignature.numerator).not.toBe(7);
    });
  });
});
