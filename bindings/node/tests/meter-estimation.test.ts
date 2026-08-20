/**
 * Meter estimation over a caller-supplied beat series, plus the beat-level
 * observations `analyze()` reports.
 *
 * The scoring reads only per-beat accents, so these tests drive it with a
 * synthetic beat series rather than audio: an accent pattern with a known
 * period is what the estimator is supposed to resolve, and building it directly
 * keeps a failure attributable to the estimator instead of to beat tracking.
 */

import { describe, expect, it } from 'vitest';
import { analyze, estimateMeter } from '../src/index.js';

const SR = 22050;

/**
 * Build a beat series whose every `numerator`-th beat is accented.
 *
 * @param numerator Beats per measure.
 * @param measures Number of measures to render.
 * @param phase Beat index the first accent lands on.
 * @param beatSec Beat period in seconds.
 * @returns Parallel beat times and accent strengths.
 */
function beatSeries(
  numerator: number,
  measures: number,
  phase = 0,
  beatSec = 0.5,
): { beatTimes: Float32Array; beatStrengths: Float32Array } {
  const count = numerator * measures;
  const beatTimes = new Float32Array(count);
  const beatStrengths = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    beatTimes[i] = i * beatSec;
    beatStrengths[i] = (((i - phase) % numerator) + numerator) % numerator === 0 ? 1 : 0.3;
  }
  return { beatTimes, beatStrengths };
}

/**
 * Percussive click track with the first beat of every measure accented, the
 * accent pattern the analysis pipeline's own meter pass scores.
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

/** One entry past the C-ABI capacity, every value inside the valid [2, 32] range. */
function overLongList(): number[] {
  return Array.from({ length: 17 }, (_, i) => 2 + (i % 30));
}

describe('estimateMeter()', () => {
  it('resolves a 4-beat accent pattern with the default candidate set', () => {
    const result = estimateMeter(beatSeries(4, 8));

    expect(result.timeSignature.numerator).toBe(4);
    expect(result.timeSignature.denominator).toBe(4);
    expect(result.timeSignature.confidence).toBeGreaterThan(0);
  });

  it('accepts a plain number[] beat series', () => {
    const { beatTimes, beatStrengths } = beatSeries(4, 8);
    const fromArrays = estimateMeter({
      beatTimes: [...beatTimes],
      beatStrengths: [...beatStrengths],
    });

    expect(fromArrays).toEqual(estimateMeter({ beatTimes, beatStrengths }));
  });

  describe('candidateNumerators reaches the estimator', () => {
    it('resolves a 5-beat accent pattern to 5 only when 5 is a candidate', () => {
      const series = beatSeries(5, 8);
      const widened = estimateMeter({ ...series, candidateNumerators: [3, 4, 5, 6, 7] });
      const defaults = estimateMeter(series);

      expect(widened.timeSignature.numerator).toBe(5);
      expect(defaults.timeSignature.numerator).not.toBe(5);
    });

    it('resolves a 7-beat accent pattern to 7 only when 7 is a candidate', () => {
      const series = beatSeries(7, 8);
      const widened = estimateMeter({ ...series, candidateNumerators: [3, 4, 5, 6, 7] });
      const defaults = estimateMeter(series);

      expect(widened.timeSignature.numerator).toBe(7);
      expect(defaults.timeSignature.numerator).not.toBe(7);
    });

    it('scores candidateScores in the requested order, candidates by support', () => {
      const candidateNumerators = [3, 4, 5];
      const result = estimateMeter({ ...beatSeries(4, 8), candidateNumerators });

      expect(result.candidateScores).toHaveLength(candidateNumerators.length);
      // The two lists do not index alike: candidates leads with the reported
      // meter, so it is matched on numerator rather than by position.
      expect(result.candidates[0]?.numerator).toBe(result.timeSignature.numerator);
      expect(
        result.candidates.map((candidate) => candidate.numerator).sort((a, b) => a - b),
      ).toEqual(candidateNumerators);
    });
  });

  it('reports the requested denominator', () => {
    const result = estimateMeter({ ...beatSeries(4, 8), denominator: 8 });

    expect(result.timeSignature.denominator).toBe(8);
    for (const candidate of result.candidates) {
      expect(candidate.denominator).toBe(8);
    }
  });

  it('keeps downbeatPhase inside the reported measure', () => {
    for (const [numerator, phase] of [
      [4, 0],
      [4, 1],
      [4, 3],
      [3, 2],
      [5, 4],
      [7, 6],
    ]) {
      const result = estimateMeter(beatSeries(numerator, 8, phase));
      expect(Number.isInteger(result.downbeatPhase)).toBe(true);
      expect(result.downbeatPhase).toBeGreaterThanOrEqual(0);
      expect(result.downbeatPhase).toBeLessThan(result.timeSignature.numerator);
    }
  });

  it('recovers the accent phase of a shifted 4-beat pattern', () => {
    for (const phase of [0, 1, 2, 3]) {
      expect(estimateMeter(beatSeries(4, 8, phase)).downbeatPhase).toBe(phase);
    }
  });

  describe('input validation', () => {
    it('rejects an empty beat series', () => {
      // The rejection is the core's, not the binding's: an empty series would
      // otherwise be answered with a fixed low-confidence 4/4 that reads like a
      // detection result. The wording is asserted unanchored because each
      // surface decorates the core message differently, but it keeps the field
      // name: a bare "must not be empty" also matches the empty candidate-list
      // guard and would let this pass for the wrong reason.
      expect(() => estimateMeter({ beatTimes: [], beatStrengths: [] })).toThrow(
        /beatTimes must not be empty/,
      );
      expect(() =>
        estimateMeter({ beatTimes: new Float32Array(0), beatStrengths: new Float32Array(0) }),
      ).toThrow(/beatTimes must not be empty/);
    });

    it('still answers a single-beat series with the low-confidence default', () => {
      // The line is drawn at empty, not at "too short to score": one beat is
      // answerable, so it returns the default rather than throwing.
      const result = estimateMeter({ beatTimes: [0], beatStrengths: [1] });

      expect(result.timeSignature.numerator).toBe(4);
      expect(result.timeSignature.denominator).toBe(4);
      expect(result.timeSignature.confidence).toBeLessThan(
        estimateMeter(beatSeries(4, 8)).timeSignature.confidence,
      );
    });

    it('rejects mismatched beatTimes and beatStrengths lengths', () => {
      const { beatTimes, beatStrengths } = beatSeries(4, 8);
      expect(() =>
        estimateMeter({ beatTimes, beatStrengths: beatStrengths.subarray(0, 5) }),
      ).toThrow(/same length/);
    });

    it('rejects an empty candidateNumerators list', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [] })).toThrow();
    });

    it('rejects more than 16 candidateNumerators entries with a RangeError', () => {
      expect(() =>
        estimateMeter({ ...beatSeries(4, 8), candidateNumerators: overLongList() }),
      ).toThrow(RangeError);
      expect(() =>
        estimateMeter({ ...beatSeries(4, 8), candidateNumerators: overLongList() }),
      ).toThrow(/at most 16 entries/);
    });

    it('rejects a candidateNumerators entry outside [2, 32]', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [1] })).toThrow();
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [33] })).toThrow();
    });

    it('rejects a denominator that is not a power of two', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), denominator: 3 })).toThrow();
    });

    it('rejects beatTimes that decrease', () => {
      const { beatTimes, beatStrengths } = beatSeries(4, 8);
      beatTimes[5] = beatTimes[4] - 1;
      expect(() => estimateMeter({ beatTimes, beatStrengths })).toThrow();
    });
  });
});

describe('analyze() beat observations', () => {
  const result = analyze(clickTrack(4, 8), SR);

  it('reports every non-empty stream beat-indexed', () => {
    const { onsetStrength, lowFrequencyEnergy, chordChange } = result.beatObservations;

    expect(result.beats.length).toBeGreaterThan(0);
    for (const stream of [onsetStrength, lowFrequencyEnergy, chordChange]) {
      expect(Array.isArray(stream)).toBe(true);
      if (stream.length > 0) {
        expect(stream).toHaveLength(result.beats.length);
      }
    }
  });

  it('reports an onsetStrength distinct from beats[].strength', () => {
    // The two are different quantities — a window around the beat versus the
    // single envelope frame nearest it — so an element-wise match would mean
    // one of them is not what it is documented to be.
    const { onsetStrength } = result.beatObservations;
    expect(onsetStrength.length).toBeGreaterThan(0);
    expect(onsetStrength).not.toEqual(result.beats.map((beat) => beat.strength));
  });

  it('feeds estimateMeter without re-running analysis', () => {
    const estimate = estimateMeter({
      beatTimes: result.beats.map((beat) => beat.time),
      beatStrengths: result.beatObservations.onsetStrength,
    });

    expect(estimate.timeSignature.numerator).toBeGreaterThan(1);
    expect(estimate.downbeatPhase).toBeLessThan(estimate.timeSignature.numerator);
  });
});
