/**
 * Meter estimation over a caller-supplied beat series, plus the beat-level
 * observation streams the library's own downbeat pass scores.
 *
 * `estimateMeter` needs no audio: it scores the per-beat strengths alone, so
 * the beat series here is synthesized directly instead of being detected. The
 * `beatObservations` coverage does run a real analysis, because the streams
 * only exist as a by-product of one.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { analyze, estimateMeter, init } from '../src/index';

const SR = 22050;

/**
 * Builds a beat series with the first beat of every measure accented, which is
 * the accent pattern the meter estimator scores.
 *
 * @param numerator - Beats per measure.
 * @param measures - Number of measures to render.
 * @param weakAmp - Strength of the unaccented beats relative to the downbeat.
 * @param beatSec - Beat period in seconds.
 * @returns Parallel beat times and strengths.
 */
function beatSeries(
  numerator: number,
  measures: number,
  weakAmp = 0.25,
  beatSec = 0.5,
): { beatTimes: Float32Array; beatStrengths: Float32Array } {
  const count = numerator * measures;
  const beatTimes = new Float32Array(count);
  const beatStrengths = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    beatTimes[i] = i * beatSec;
    beatStrengths[i] = i % numerator === 0 ? 1 : weakAmp;
  }
  return { beatTimes, beatStrengths };
}

/**
 * Builds a percussive click track with every beat accented equally, enough for
 * the beat tracker to find beats to observe.
 *
 * @param beats - Number of clicks to render.
 * @param beatSec - Beat period in seconds.
 * @returns The generated mono click track.
 */
function clickTrack(beats: number, beatSec = 0.4): Float32Array {
  const out = new Float32Array(Math.round(SR * beatSec * beats));
  const clickLen = Math.round(SR * 0.06);
  // Deterministic LCG noise burst: broadband so the onset detector sees every
  // click, and reproducible so the analysis does not vary run to run.
  let seed = 12345;
  const noise = new Float32Array(clickLen);
  for (let i = 0; i < clickLen; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    noise[i] = (seed / 0x3fffffff - 1) * Math.exp(-i / (SR * 0.012));
  }
  for (let b = 0; b < beats; b++) {
    const amp = b % 4 === 0 ? 1.0 : 0.28;
    const start = Math.round(b * beatSec * SR);
    for (let i = 0; i < clickLen && start + i < out.length; i++) {
      out[start + i] += 0.6 * amp * noise[i];
    }
  }
  return out;
}

describe('WASM estimateMeter', () => {
  beforeAll(async () => {
    await init();
  });

  describe('scoring', () => {
    it('resolves a 4-beat accent pattern to 4/4 with the default candidate set', () => {
      const result = estimateMeter(beatSeries(4, 8));

      expect(result.timeSignature.numerator).toBe(4);
      expect(result.timeSignature.denominator).toBe(4);
      expect(Number.isFinite(result.timeSignature.confidence)).toBe(true);
    });

    it('resolves a 5-beat accent pattern to 5 only when 5 is a candidate', () => {
      // 5 x 8 = 40 beats, which no other candidate numerator divides into equal
      // measures starting on the same accent, so the widened run has one answer.
      const series = beatSeries(5, 8);
      const widened = estimateMeter({ ...series, candidateNumerators: [3, 4, 5, 6] });
      const defaults = estimateMeter(series);

      expect(widened.timeSignature.numerator).toBe(5);
      expect(defaults.timeSignature.numerator).not.toBe(5);
    });

    it('resolves a 7-beat accent pattern to 7 only when 7 is a candidate', () => {
      // 7 x 7 = 49 beats: unlike 7 x 5 = 35, the beat count is not also
      // divisible by 5, so a 5-beat comb cannot fit the accents and win.
      const series = beatSeries(7, 7);
      const widened = estimateMeter({ ...series, candidateNumerators: [3, 4, 5, 6, 7] });
      const defaults = estimateMeter(series);

      expect(widened.timeSignature.numerator).toBe(7);
      expect(defaults.timeSignature.numerator).not.toBe(7);
    });

    it('reports the requested denominator', () => {
      const result = estimateMeter({ ...beatSeries(4, 8), denominator: 8 });

      expect(result.timeSignature.denominator).toBe(8);
    });

    it('keeps downbeatPhase within [0, numerator)', () => {
      // Dropping the leading beats moves the first measure start off beat 0, so
      // the phase has somewhere to move to.
      for (const offset of [0, 1, 2, 3]) {
        const series = beatSeries(4, 8);
        const result = estimateMeter({
          beatTimes: series.beatTimes.slice(offset),
          beatStrengths: series.beatStrengths.slice(offset),
        });

        expect(Number.isInteger(result.downbeatPhase)).toBe(true);
        expect(result.downbeatPhase).toBeGreaterThanOrEqual(0);
        expect(result.downbeatPhase).toBeLessThan(result.timeSignature.numerator);
      }
    });

    it('scores one entry per requested numerator and ranks the candidates', () => {
      const result = estimateMeter({
        ...beatSeries(4, 8),
        candidateNumerators: [3, 4, 5, 6, 7],
      });

      // candidateScores is parallel to the REQUESTED numerators, in the order
      // they were given; candidates is a ranking by descending support. The two
      // do not index alike, so only the score list has the requested length.
      expect(result.candidateScores).toHaveLength(5);
      for (const score of result.candidateScores) {
        expect(Number.isFinite(score)).toBe(true);
      }
      expect(result.candidates.length).toBeGreaterThan(0);
      expect(result.candidates[0].numerator).toBe(result.timeSignature.numerator);
      for (let i = 1; i < result.candidates.length; i++) {
        expect(result.candidates[i].confidence).toBeLessThanOrEqual(
          result.candidates[i - 1].confidence,
        );
      }
    });

    it('reads accent contrast rather than absolute level', () => {
      // The documented strength source arrives in the onset envelope's units
      // and runs well above 1, so scoring divides the series by its own maximum
      // and a caller has no scale to supply. Saturating it instead would
      // flatten every beat to one value and tie every candidate.
      const { beatTimes, beatStrengths } = beatSeries(4, 8);
      const scaled = Float32Array.from(beatStrengths, (value) => value * 20);

      const unit = estimateMeter({ beatTimes, beatStrengths });
      const loud = estimateMeter({ beatTimes, beatStrengths: scaled });

      expect(loud).toEqual(unit);
      expect(loud.timeSignature.numerator).toBe(4);
      expect(new Set(loud.candidateScores).size).toBe(loud.candidateScores.length);
    });

    it('scores an unmetred series below the no-meter level', () => {
      // Zero is what a numerator reaches on beats carrying no meter, so a flat
      // series lands under it, and the wider the numerator the further under:
      // it searched more phases and found nothing in any of them.
      const beatTimes = Float32Array.from({ length: 48 }, (_, i) => i * 0.5);
      const beatStrengths = new Float32Array(48).fill(0.5);

      const result = estimateMeter({ beatTimes, beatStrengths, candidateNumerators: [3, 4, 13] });

      expect(result.candidateScores.every((score) => score < 0)).toBe(true);
      expect(result.candidateScores).toEqual([...result.candidateScores].sort((a, b) => b - a));
      expect(result.timeSignature.confidence).toBeLessThan(0.6);
    });

    it('accepts a plain number array as well as a Float32Array', () => {
      const series = beatSeries(4, 8);
      const fromTyped = estimateMeter(series);
      const fromPlain = estimateMeter({
        beatTimes: Array.from(series.beatTimes),
        beatStrengths: Array.from(series.beatStrengths),
      });

      expect(fromPlain).toEqual(fromTyped);
    });
  });

  describe('input validation', () => {
    it('rejects mismatched beatTimes / beatStrengths lengths', () => {
      const series = beatSeries(4, 8);
      expect(() =>
        estimateMeter({
          beatTimes: series.beatTimes,
          beatStrengths: series.beatStrengths.slice(1),
        }),
      ).toThrow();
    });

    it('rejects an empty beat series', () => {
      // No beats is nothing to search, so answering it with the fixed
      // low-confidence 4/4 would read as a detection result. Matched on an
      // unanchored substring because the surrounding decoration differs
      // between surfaces, and on `beatTimes` specifically so this cannot pass
      // on the separate "candidateNumerators must not be empty" guard.
      expect(() => estimateMeter({ beatTimes: [], beatStrengths: [] })).toThrow(
        /beatTimes must not be empty/,
      );
      expect(() =>
        estimateMeter({ beatTimes: new Float32Array(0), beatStrengths: new Float32Array(0) }),
      ).toThrow(/beatTimes must not be empty/);
    });

    it('still answers a single-beat series, so the line is drawn at empty not at short', () => {
      // One beat is something to score, so it keeps the low-confidence default
      // rather than being rejected. Without this the guard above would be
      // satisfied just as well by a wrong rule that rejected any short series.
      const result = estimateMeter({ beatTimes: [0], beatStrengths: [1] });

      expect(result.timeSignature.numerator).toBe(4);
      expect(result.timeSignature.denominator).toBe(4);
      expect(result.timeSignature.confidence).toBeLessThanOrEqual(0.5);
    });

    it('rejects an empty candidateNumerators list', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [] })).toThrow();
    });

    it('rejects more than 16 candidateNumerators entries', () => {
      const seventeen = Array.from({ length: 17 }, (_, i) => 2 + (i % 30));
      expect(() =>
        estimateMeter({ ...beatSeries(4, 8), candidateNumerators: seventeen }),
      ).toThrow();
    });

    it('rejects a candidateNumerators entry outside [2, 32]', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [1] })).toThrow();
      expect(() => estimateMeter({ ...beatSeries(4, 8), candidateNumerators: [33] })).toThrow();
    });

    it('rejects a denominator that is not a power of two', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), denominator: 3 })).toThrow();
    });

    it('rejects a non-finite scoring weight', () => {
      expect(() => estimateMeter({ ...beatSeries(4, 8), downbeatWeight: Number.NaN })).toThrow();
    });

    it('rejects out-of-order beat times', () => {
      const series = beatSeries(4, 8);
      const beatTimes = Float32Array.from(series.beatTimes);
      beatTimes[3] = beatTimes[2] - 0.1;
      expect(() => estimateMeter({ beatTimes, beatStrengths: series.beatStrengths })).toThrow();
    });
  });
});

describe('WASM analyze() beatObservations', () => {
  let result: ReturnType<typeof analyze>;

  beforeAll(async () => {
    await init();
    result = analyze(clickTrack(32), SR);
  });

  it('exposes each non-empty stream parallel to beats', () => {
    expect(result.beats.length).toBeGreaterThan(0);
    // onsetStrength always comes out of a completed analysis; the other two are
    // conditional (lowFrequencyEnergy needs audio, chordChange needs chords),
    // and an empty stream means the analysis could not produce it rather than
    // every beat having scored zero — so those are length-checked only when
    // populated, and asserting onsetStrength unconditionally keeps the loop
    // from passing vacuously.
    expect(result.beatObservations.onsetStrength).toHaveLength(result.beats.length);
    for (const stream of [
      result.beatObservations.lowFrequencyEnergy,
      result.beatObservations.chordChange,
    ]) {
      expect(Array.isArray(stream)).toBe(true);
      if (stream.length > 0) {
        expect(stream).toHaveLength(result.beats.length);
      }
    }
  });

  it('measures onsetStrength over a window, not at the beat frame', () => {
    // beats[].strength is a single unwindowed envelope frame while
    // onsetStrength is a beat-local aggregate: genuinely different quantities,
    // so they must not come back element-wise identical.
    const observations = result.beatObservations.onsetStrength;
    expect(observations.length).toBeGreaterThan(0);
    const beatStrengths = result.beats.map((beat) => beat.strength);
    expect(observations).not.toEqual(beatStrengths);
  });

  it('feeds estimateMeter without re-running the analysis', () => {
    const observations = result.beatObservations.onsetStrength;
    // The stream is a windowed aggregate in the onset envelope's own units, not
    // a pre-scaled salience, so this is the range the documented source
    // actually arrives in.
    expect(Math.max(...observations)).toBeGreaterThan(1);

    const estimate = estimateMeter({
      beatTimes: result.beats.map((beat) => beat.time),
      beatStrengths: observations,
    });

    expect(estimate.timeSignature.numerator).toBeGreaterThanOrEqual(2);
    expect(estimate.downbeatPhase).toBeLessThan(estimate.timeSignature.numerator);
    // Candidates scoring alike is what a saturated series produces, so they
    // have to separate for this to be a search rather than a fallback.
    expect(new Set(estimate.candidateScores).size).toBe(estimate.candidateScores.length);
  });
});

/**
 * Builds a beat series whose bars divide exactly as `grouping` says.
 *
 * The downbeat is loudest, every following group starts on a middling beat and
 * the rest are quiet, which is the accent shape an additive meter is written
 * for.
 *
 * @param grouping - Beats per accent group within one bar.
 * @param measures - Number of measures to render.
 * @returns Parallel beat times and strengths.
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

describe('WASM estimateMeter grouping', () => {
  beforeAll(async () => {
    await init();
  });

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
