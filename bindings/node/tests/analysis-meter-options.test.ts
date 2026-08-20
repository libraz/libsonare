/**
 * Meter/tempo analysis option tests for the Node binding.
 *
 * Covers the downbeat fields on the `analyze()` result and the adaptive-tempo /
 * meter-candidate options: their defaults, the validation applied to them (in
 * the addon for the list length and element types, in the core for everything
 * else), that a widened candidate set actually reaches the meter estimator, and
 * that the positional and request-object call forms agree on all of it.
 */

import { describe, expect, it } from 'vitest';
import type { MusicAnalyzeOptions } from '../src/analysis.js';
import { analyze, analyzeAsync } from '../src/index.js';

const SR = 22050;

/**
 * Build a percussive click track with the first beat of every measure
 * accented, which is the accent pattern the meter estimator scores.
 *
 * @param numerator Beats per measure.
 * @param measures Number of measures to render.
 * @param beatSec Beat period in seconds.
 * @returns The generated mono click track.
 */
function clickTrack(numerator: number, measures: number, beatSec = 0.4): Float32Array {
  const beats = numerator * measures;
  const out = new Float32Array(Math.round(SR * beatSec * beats));
  const clickLen = Math.round(SR * 0.06);
  // Deterministic LCG noise burst: broadband so the onset detector sees every
  // click, and reproducible so the meter result does not vary run to run.
  let seed = 12345;
  const noise = new Float32Array(clickLen);
  for (let i = 0; i < clickLen; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    noise[i] = (seed / 0x3fffffff - 1) * Math.exp(-i / (SR * 0.012));
  }
  for (let b = 0; b < beats; b++) {
    const amp = b % numerator === 0 ? 1.0 : 0.28;
    const start = Math.round(b * beatSec * SR);
    for (let i = 0; i < clickLen && start + i < out.length; i++) {
      out[start + i] += 0.6 * amp * noise[i];
    }
  }
  return out;
}

const samples = clickTrack(4, 8);
const fiveBeat = clickTrack(5, 7);

describe('analyze() downbeats and meter options', () => {
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
      expect(() => analyze(samples, SR, { meterCandidateNumerators: overLongList() })).toThrow(
        /at most 16 entries/,
      );
    });

    it('rejects a non-numeric meterCandidateNumerators entry', () => {
      const malformed = [3, 'four'] as unknown as number[];
      expect(() => analyze(samples, SR, { meterCandidateNumerators: malformed })).toThrow(
        /meterCandidateNumerators\[1\] must be a number/,
      );
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

    it('rejects the same options through analyzeAsync', async () => {
      await expect(
        analyzeAsync(samples, SR, { meterCandidateNumerators: overLongList() }),
      ).rejects.toThrow(/at most 16 entries/);
      await expect(analyzeAsync(samples, SR, { tempoUpdateIntervalBeats: 0 })).rejects.toThrow();
    });
  });

  describe('meterCandidateNumerators reaches the estimator', () => {
    it('resolves a 5-beat accent pattern to 5 only when 5 is a candidate', () => {
      const widened = analyze(fiveBeat, SR, { meterCandidateNumerators: [5, 7] });
      const defaults = analyze(fiveBeat, SR);

      expect(widened.timeSignature.numerator).toBe(5);
      expect(defaults.timeSignature.numerator).not.toBe(5);
    });

    it('resolves a 7-beat accent pattern to 7 only when 7 is a candidate', () => {
      // Seven measures, not five: 35 beats are also divisible by 5, and the
      // estimator prefers 5 over 7 on that shorter track even with both scored.
      const sevenBeat = clickTrack(7, 7);
      const widened = analyze(sevenBeat, SR, { meterCandidateNumerators: [5, 7] });
      const defaults = analyze(sevenBeat, SR);

      expect(widened.timeSignature.numerator).toBe(7);
      expect(defaults.timeSignature.numerator).not.toBe(7);
    });
  });

  // Both call shapes normalize through one path, so the new options must reach
  // the analyzer and fail identically either way.
  describe('positional and request-object call forms', () => {
    const options: MusicAnalyzeOptions = {
      adaptiveTempo: true,
      tempoUpdateIntervalBeats: 4,
      meterCandidateNumerators: [5, 7],
      meterDenominator: 8,
    };

    it('produces the same result either way', () => {
      const positional = analyze(fiveBeat, SR, options);
      expect(analyze({ samples: fiveBeat, sampleRate: SR, ...options })).toEqual(positional);
      // The bag is not inert: without it the same signal resolves differently,
      // so the equivalence above is over a non-default configuration.
      expect(positional).not.toEqual(analyze(fiveBeat, SR));
    });

    it('fails identically either way', () => {
      for (const bad of [
        { meterCandidateNumerators: [] },
        { meterCandidateNumerators: overLongList() },
        { meterCandidateNumerators: [1] },
        { meterCandidateNumerators: [33] },
        { meterDenominator: 3 },
        { tempoUpdateIntervalBeats: 0 },
      ] as MusicAnalyzeOptions[]) {
        const positional = captureThrow(() => analyze(fiveBeat, SR, bad));
        const request = captureThrow(() => analyze({ samples: fiveBeat, sampleRate: SR, ...bad }));
        expect(positional.threw, JSON.stringify(bad)).toBe(true);
        expect(request.threw, JSON.stringify(bad)).toBe(true);
        expect(request.message).toBe(positional.message);
      }
    });
  });
});

/** One entry past the C-ABI capacity, every value inside the valid [2, 32] range. */
function overLongList(): number[] {
  return Array.from({ length: 17 }, (_, i) => 2 + (i % 30));
}

function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
  try {
    fn();
    return { threw: false, message: '' };
  } catch (error) {
    return { threw: true, message: error instanceof Error ? error.message : String(error) };
  }
}
