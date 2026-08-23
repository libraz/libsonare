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

  it('reads accent contrast rather than absolute level', () => {
    // `beatObservations.onsetStrength`, the documented strength source, is a
    // windowed aggregate in the onset envelope's own units and runs well above
    // 1 on ordinary material. Scoring divides the series by its own maximum, so
    // a caller has no scale to supply; saturating it instead would flatten
    // every beat to one value and tie every candidate.
    const { beatTimes, beatStrengths } = beatSeries(4, 8);
    const scaled = Float32Array.from(beatStrengths, (value) => value * 20);

    const unit = estimateMeter({ beatTimes, beatStrengths });
    const loud = estimateMeter({ beatTimes, beatStrengths: scaled });

    expect(loud).toEqual(unit);
    expect(loud.timeSignature.numerator).toBe(4);
    expect(new Set(loud.candidateScores).size).toBe(loud.candidateScores.length);
  });

  it('scores an unmetred series below the no-meter level', () => {
    // The scores are standardized against what a numerator reaches on beats
    // carrying no meter, so zero is that level and a flat series lands under
    // it. The wider the numerator the further under: it searched more phases
    // and found nothing in any of them, which is the width advantage the
    // standardization removes.
    const beatTimes = Float32Array.from({ length: 48 }, (_, i) => i * 0.5);
    const beatStrengths = new Float32Array(48).fill(0.5);

    const result = estimateMeter({ beatTimes, beatStrengths, candidateNumerators: [3, 4, 13] });

    expect(result.candidateScores.every((score) => score < 0)).toBe(true);
    expect(result.candidateScores).toEqual([...result.candidateScores].sort((a, b) => b - a));
    expect(result.timeSignature.confidence).toBeLessThan(0.6);
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
    const { onsetStrength } = result.beatObservations;
    // The stream arrives in the envelope's units rather than pre-scaled, which
    // is the case the estimator has to handle for the documented source to be
    // usable at all.
    expect(Math.max(...onsetStrength)).toBeGreaterThan(1);

    const estimate = estimateMeter({
      beatTimes: result.beats.map((beat) => beat.time),
      beatStrengths: onsetStrength,
    });

    expect(estimate.timeSignature.numerator).toBeGreaterThan(1);
    expect(estimate.downbeatPhase).toBeLessThan(estimate.timeSignature.numerator);
    // Every candidate scoring alike is the signature of a saturated series, so
    // the scores have to separate for this to be a search and not a fallback.
    expect(new Set(estimate.candidateScores).size).toBe(estimate.candidateScores.length);
  });
});

/**
 * Build a beat series whose bars divide exactly as `grouping` says.
 *
 * The downbeat is loudest, every following group starts on a middling beat and
 * the rest are quiet, which is the accent shape an additive meter is written
 * for.
 *
 * @param grouping Beats per accent group within one bar.
 * @param measures Number of measures to render.
 * @returns Parallel beat times and accent strengths.
 */
function groupedSeries(
  grouping: number[],
  measures = 12,
): { beatTimes: Float32Array; beatStrengths: Float32Array } {
  const accents = new Set<number>();
  let position = 0;
  for (const part of grouping.slice(0, -1)) {
    position += part;
    accents.add(position);
  }

  const numerator = grouping.reduce((sum, part) => sum + part, 0);
  const count = numerator * measures;
  const beatTimes = new Float32Array(count);
  const beatStrengths = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    const pos = i % numerator;
    beatTimes[i] = i * 0.5;
    beatStrengths[i] = pos === 0 ? 1 : accents.has(pos) ? 0.65 : 0.35;
  }
  return { beatTimes, beatStrengths };
}

describe('estimateMeter() grouping', () => {
  it('reports how the bar divides, not just how many beats it holds', () => {
    // These layouts share a numerator and an accent count and differ only in
    // where the accents fall, so the numerator alone cannot tell them apart.
    const candidateNumerators = [3, 4, 5, 6, 7, 9, 11, 13];
    for (const grouping of [
      [3, 2, 2],
      [2, 3, 2],
      [2, 2, 3],
      [3, 2],
      [2, 3],
      [2, 2, 2, 3],
      [3, 3, 3, 2, 2],
    ]) {
      const result = estimateMeter({ ...groupedSeries(grouping), candidateNumerators });
      expect(result.timeSignature.numerator).toBe(grouping.reduce((sum, p) => sum + p, 0));
      expect(result.grouping).toEqual(grouping);
    }
  });

  it('always reports a grouping that sums to the numerator', () => {
    // The invariant every consumer of the field relies on, checked across the
    // whole candidate range so it covers the numerators too wide to divide.
    for (let numerator = 2; numerator <= 32; numerator++) {
      const result = estimateMeter({
        ...beatSeries(numerator, 6),
        candidateNumerators: [numerator],
      });
      expect(result.grouping.length).toBeGreaterThan(0);
      expect(result.grouping.reduce((sum, part) => sum + part, 0)).toBe(
        result.timeSignature.numerator,
      );
      expect(result.grouping.every((part) => Number.isInteger(part) && part > 0)).toBe(true);
    }
  });

  it('leaves a bar undivided when nothing divided it', () => {
    // Below the search floor no scoring ran at all, so the bar comes back whole
    // rather than carrying the 2+2 a four would have been given.
    const { beatTimes, beatStrengths } = beatSeries(4, 8);
    const result = estimateMeter({
      beatTimes: beatTimes.slice(0, 4),
      beatStrengths: beatStrengths.slice(0, 4),
    });

    expect(result.timeSignature.confidence).toBeLessThanOrEqual(0.5);
    expect(result.grouping).toEqual([result.timeSignature.numerator]);
  });

  it('reports how a six divides as a grouping, not as a beat unit', () => {
    // Whether a beat divides into three is measured between the beats, which
    // this entry point never sees, so the compound reading is not one it can
    // reach: both sixes keep the requested unit and differ in the grouping.
    const compound = estimateMeter(groupedSeries([3, 3]));
    expect(compound.timeSignature.numerator).toBe(6);
    expect(compound.timeSignature.denominator).toBe(4);
    expect(compound.grouping).toEqual([3, 3]);
    for (const candidate of compound.candidates) {
      if (candidate.numerator === 6) {
        expect(candidate.denominator).toBe(4);
      }
    }

    // A six grouped into three twos is a simple meter, which is what keeps the
    // assertions above from being about 6 alone.
    const simple = estimateMeter(groupedSeries([2, 2, 2]));
    expect(simple.timeSignature.numerator).toBe(6);
    expect(simple.timeSignature.denominator).toBe(4);
    expect(simple.grouping).toEqual([2, 2, 2]);

    // The unit follows the request even for the bar that divides into threes.
    const inEighths = estimateMeter({ ...groupedSeries([3, 3]), denominator: 8 });
    expect(inEighths.timeSignature.denominator).toBe(8);
    expect(inEighths.grouping).toEqual([3, 3]);
  });

  it('says whether a search ran, so a fallback is not read as a detection', () => {
    const { beatTimes, beatStrengths } = groupedSeries([2, 2]);
    expect(estimateMeter({ beatTimes, beatStrengths }).searched).toBe(true);

    for (let count = 1; count < 8; count += 1) {
      const short = estimateMeter({
        beatTimes: beatTimes.slice(0, count),
        beatStrengths: beatStrengths.slice(0, count),
      });
      expect(short.searched).toBe(false);
      expect(short.timeSignature.numerator).toBe(4);
      expect(short.candidateScores.every((score) => score === 0)).toBe(true);
    }

    expect(
      estimateMeter({
        beatTimes: beatTimes.slice(0, 8),
        beatStrengths: beatStrengths.slice(0, 8),
      }).searched,
    ).toBe(true);
  });
});
