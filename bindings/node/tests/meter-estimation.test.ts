/**
 * Meter estimation over caller-supplied observations and the observations
 * `analyze()` reports.
 *
 * Synthetic observations give the scorer known accent patterns. A separate
 * analysis result checks that the published observations match its meter.
 */

import { describe, expect, it } from 'vitest';
import { analyze, type BeatObservations, estimateMeter } from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const SR = 22050;

function observationsFromAccent(
  accent: ArrayLike<number>,
  subdivisionAccent: ArrayLike<number> = [],
): BeatObservations {
  return {
    accent: Array.from(accent),
    hasAttack: [],
    lowFrequencyEnergy: [],
    subdivisionAccent: Array.from(subdivisionAccent),
    chordChange: [],
  };
}

/**
 * Build a beat series whose every `numerator`-th beat is accented.
 *
 * @param numerator Beats per measure.
 * @param measures Number of measures to render.
 * @param phase Beat index the first accent lands on.
 * @returns Beat observations accented every `numerator`-th beat.
 */
function beatSeries(
  numerator: number,
  measures: number,
  phase = 0,
): { observations: BeatObservations } {
  const count = numerator * measures;
  const accent = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    accent[i] = (((i - phase) % numerator) + numerator) % numerator === 0 ? 1 : 0.3;
  }
  return { observations: observationsFromAccent(accent) };
}

/**
 * Percussive click track with the first beat of every measure accented.
 *
 * @param numerator Beats per measure.
 * @param measures Number of measures to render.
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
  it('reports masked beats in index order when per-beat energy is supplied', () => {
    const observations = observationsFromAccent([1, 0.35, 0.35, 1, 0.35, 0.35, 1, 0.35]);
    observations.hasAttack = [false, true, true, true, true, true, false, true];
    expect(estimateMeter({ observations }).unobservedBeats).toEqual([]);
    observations.lowFrequencyEnergy = [16, 1, 1, 1, 1, 1, 24, 1];
    expect(estimateMeter({ observations }).unobservedBeats).toEqual([0, 6]);
  });

  it('resolves a 4-beat accent pattern with the default candidate set', () => {
    const result = estimateMeter(beatSeries(4, 8));

    expect(result.timeSignature.numerator).toBe(4);
    expect(result.timeSignature.denominator).toBe(4);
    expect(result.timeSignature.confidence).toBeGreaterThan(0);
  });

  it('reads accent contrast rather than absolute level', () => {
    const { observations } = beatSeries(4, 8);
    const scaled = {
      ...observations,
      accent: observations.accent.map((accent) => accent * 20),
    };

    const unit = estimateMeter({ observations });
    const loud = estimateMeter({ observations: scaled });

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
    const observations = observationsFromAccent(new Float32Array(48).fill(0.5));

    const result = estimateMeter({ observations, candidateNumerators: [3, 4, 13] });

    expect(result.candidateScores.every((score) => score < 0)).toBe(true);
    expect(result.candidateScores).toEqual([...result.candidateScores].sort((a, b) => b - a));
    expect(result.timeSignature.confidence).toBeLessThan(0.6);
  });

  it('accepts readonly observation streams', () => {
    const { observations } = beatSeries(4, 8);
    const frozen = Object.freeze({
      ...observations,
      accent: Object.freeze([...observations.accent]),
      hasAttack: Object.freeze([...observations.hasAttack]),
      lowFrequencyEnergy: Object.freeze([...observations.lowFrequencyEnergy]),
      subdivisionAccent: Object.freeze([...observations.subdivisionAccent]),
      chordChange: Object.freeze([...observations.chordChange]),
    });

    expect(estimateMeter({ observations: frozen as unknown as BeatObservations })).toEqual(
      estimateMeter({ observations }),
    );
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
    it('refuses a missing or non-object observations record', () => {
      for (const observations of [undefined, null, 'observations', []]) {
        expect(() => estimateMeter({ observations } as never)).toThrow(
          new TypeError('estimateMeter: observations must be an object'),
        );
      }
    });

    it('refuses non-array streams and non-boolean attack flags', () => {
      const observations = beatSeries(4, 2).observations;
      expect(() =>
        estimateMeter({ observations: { ...observations, accent: 'accent' } as never }),
      ).toThrow(TypeError);
      expect(() =>
        estimateMeter({
          observations: { ...observations, hasAttack: [true, 1] } as never,
        }),
      ).toThrow(TypeError);
    });

    it('refuses an unknown observation key', () => {
      const { observations } = beatSeries(4, 2);
      expect(() =>
        estimateMeter({ observations: { ...observations, onsetStrength: [] } as never }),
      ).toThrow(/unknown key observations\.onsetStrength/);
    });

    it('refuses optional streams with the wrong lengths', () => {
      const observations = beatSeries(4, 2).observations;
      expect(() => estimateMeter({ observations: { ...observations, hasAttack: [true] } })).toThrow(
        RangeError,
      );
      expect(() =>
        estimateMeter({ observations: { ...observations, lowFrequencyEnergy: [0.5] } }),
      ).toThrow(RangeError);
      expect(() =>
        estimateMeter({ observations: { ...observations, subdivisionAccent: [0.5] } }),
      ).toThrow(RangeError);
    });

    it('rejects an empty accent but answers a single-beat stream with the fallback', () => {
      expect(() => estimateMeter({ observations: observationsFromAccent([]) })).toThrow(
        new RangeError('estimateMeter: observations.accent must not be empty'),
      );

      const result = estimateMeter({ observations: observationsFromAccent([1]) });

      expect(result.timeSignature.numerator).toBe(4);
      expect(result.timeSignature.denominator).toBe(4);
      expect(result.timeSignature.confidence).toBeLessThan(
        estimateMeter(beatSeries(4, 8)).timeSignature.confidence,
      );
    });

    it('rejects an empty candidateNumerators list', () => {
      expectRefusal(
        () => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [] }),
        RangeError,
      );
    });

    it('rejects more than 16 candidateNumerators entries with a RangeError', () => {
      expectRefusal(
        () => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: overLongList() }),
        RangeError,
        /at most 16 entries/,
      );
    });

    it('rejects a candidateNumerators entry outside [2, 32]', () => {
      expectRefusal(
        () => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [1] }),
        RangeError,
      );
      expectRefusal(
        () => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [33] }),
        RangeError,
      );
    });

    it('rejects a denominator that is not a power of two', () => {
      expectRefusal(() => estimateMeter({ ...beatSeries(4, 8), denominator: 3 }), RangeError);
    });

    it('refuses non-finite or negative observation values', () => {
      const observations = beatSeries(4, 8).observations;
      expect(() =>
        estimateMeter({
          observations: { ...observations, accent: [-1, ...observations.accent.slice(1)] },
        }),
      ).toThrow(RangeError);
      expect(() =>
        estimateMeter({
          observations: {
            ...observations,
            lowFrequencyEnergy: [Number.NaN, ...new Array(observations.accent.length - 1).fill(0)],
          },
        }),
      ).toThrow(RangeError);
      expect(() =>
        estimateMeter({
          observations: {
            ...observations,
            subdivisionAccent: [-1, ...new Array(observations.accent.length - 2).fill(0)],
          },
        }),
      ).toThrow(RangeError);
    });
  });
});

describe('analyze() beat observations', () => {
  const result = analyze(clickTrack(4, 8), SR);

  it('reports the measured streams at their beat and interval lengths', () => {
    const { accent, hasAttack, lowFrequencyEnergy, subdivisionAccent, chordChange } =
      result.beatObservations;

    expect(result.beats.length).toBeGreaterThan(0);
    expect(accent).toHaveLength(result.beats.length);
    expect(hasAttack).toHaveLength(result.beats.length);
    expect(hasAttack.every((value) => typeof value === 'boolean')).toBe(true);
    expect(subdivisionAccent).toHaveLength(result.beats.length - 1);
    for (const stream of [lowFrequencyEnergy, chordChange]) {
      expect(Array.isArray(stream)).toBe(true);
      if (stream.length > 0) {
        expect(stream).toHaveLength(result.beats.length);
      }
    }
  });

  it('reports beat and subdivision accents separately', () => {
    const { accent, subdivisionAccent } = result.beatObservations;
    expect(accent.length).toBeGreaterThan(0);
    expect(accent).not.toEqual(result.beats.map((beat) => beat.strength));
    expect(subdivisionAccent.some((value) => value === 0)).toBe(true);
  });

  it('matches the meter reported by analysis', () => {
    expect(result.beats.length).toBeGreaterThanOrEqual(8);
    const estimate = estimateMeter({ observations: result.beatObservations });

    expect(estimate.searched).toBe(true);
    expect(estimate.timeSignature.numerator).toBe(result.timeSignature.numerator);
    expect(estimate.timeSignature.denominator).toBe(result.timeSignature.denominator);
    expect(
      Math.abs(estimate.timeSignature.confidence - result.timeSignature.confidence),
    ).toBeLessThanOrEqual(1e-6);
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
 * @param subdivisionAccent When given, fills the interval stream with this accent.
 * @returns The observations with the grouped accent shape.
 */
function groupedSeries(
  grouping: number[],
  measures = 12,
  subdivisionAccent?: number,
): { observations: BeatObservations } {
  const accents = new Set<number>();
  let position = 0;
  for (const part of grouping.slice(0, -1)) {
    position += part;
    accents.add(position);
  }

  const numerator = grouping.reduce((sum, part) => sum + part, 0);
  const count = numerator * measures;
  const beatStrengths = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    const pos = i % numerator;
    beatStrengths[i] = pos === 0 ? 1 : accents.has(pos) ? 0.65 : 0.35;
  }
  const subdivisions =
    subdivisionAccent === undefined ? [] : new Array(count - 1).fill(subdivisionAccent);
  return { observations: observationsFromAccent(beatStrengths, subdivisions) };
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
    const observations = beatSeries(4, 8).observations;
    const result = estimateMeter({
      observations: { ...observations, accent: observations.accent.slice(0, 4) },
    });

    expect(result.timeSignature.confidence).toBeLessThanOrEqual(0.5);
    expect(result.grouping).toEqual([result.timeSignature.numerator]);
  });

  it('uses subdivision evidence to distinguish compound and simple sixes', () => {
    const compound = estimateMeter(groupedSeries([3, 3]));
    expect(compound.timeSignature.numerator).toBe(6);
    expect(compound.timeSignature.denominator).toBe(4);
    expect(compound.grouping).toEqual([3, 3]);

    const measuredCompound = estimateMeter(groupedSeries([3, 3], 12, 0.8));
    expect(measuredCompound.timeSignature.numerator).toBe(6);
    expect(measuredCompound.timeSignature.denominator).toBe(8);
    expect(measuredCompound.grouping).toEqual([3, 3]);
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

    const inEighths = estimateMeter({ ...groupedSeries([3, 3], 12, 0.8), denominator: 8 });
    expect(inEighths.timeSignature.denominator).toBe(8);
    expect(inEighths.grouping).toEqual([3, 3]);
  });

  it('says whether a search ran, so a fallback is not read as a detection', () => {
    const { observations } = groupedSeries([2, 2]);
    expect(estimateMeter({ observations }).searched).toBe(true);

    for (let count = 1; count < 8; count += 1) {
      const short = estimateMeter({
        observations: { ...observations, accent: observations.accent.slice(0, count) },
      });
      expect(short.searched).toBe(false);
      expect(short.timeSignature.numerator).toBe(4);
      expect(short.candidateScores.every((score) => score === 0)).toBe(true);
    }

    expect(
      estimateMeter({ observations: { ...observations, accent: observations.accent.slice(0, 8) } })
        .searched,
    ).toBe(true);
  });
});
