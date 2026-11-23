/** Meter estimation from the observation streams returned by full analysis. */

import { beforeAll, describe, expect, it } from 'vitest';
import { analyze, type BeatObservations, estimateMeter, init } from '../src/index';

const SR = 22050;

function meterObservations(accent: number[], subdivisionAccent: number[] = []): BeatObservations {
  return {
    accent,
    hasAttack: Array.from({ length: accent.length }, () => true),
    lowFrequencyEnergy: [],
    subdivisionAccent,
    chordChange: [],
  };
}

function beatSeries(numerator: number, measures: number, weakAmp = 0.25): BeatObservations {
  const accent = Array.from({ length: numerator * measures }, (_, i) =>
    i % numerator === 0 ? 1 : weakAmp,
  );
  return meterObservations(accent);
}

function sliceObservations(
  observations: BeatObservations,
  start: number,
  end?: number,
): BeatObservations {
  return {
    ...observations,
    accent: observations.accent.slice(start, end),
    hasAttack: observations.hasAttack.slice(start, end),
    lowFrequencyEnergy: observations.lowFrequencyEnergy.slice(start, end),
    subdivisionAccent: [],
    chordChange: observations.chordChange.slice(start, end),
  };
}

function groupedSeries(grouping: number[], measures = 12): BeatObservations {
  const accents = new Set<number>();
  let position = 0;
  for (const part of grouping.slice(0, -1)) {
    position += part;
    accents.add(position);
  }
  const numerator = grouping.reduce((sum, part) => sum + part, 0);
  const accent = Array.from({ length: numerator * measures }, (_, i) => {
    const beatPosition = i % numerator;
    return beatPosition === 0 ? 1 : accents.has(beatPosition) ? 0.65 : 0.35;
  });
  return meterObservations(accent);
}

function clickTrack(beats: number, beatSec = 0.4): Float32Array {
  const out = new Float32Array(Math.round(SR * beatSec * beats));
  const clickLen = Math.round(SR * 0.06);
  let seed = 12345;
  const noise = new Float32Array(clickLen);
  for (let i = 0; i < clickLen; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    noise[i] = (seed / 0x3fffffff - 1) * Math.exp(-i / (SR * 0.012));
  }
  for (let beat = 0; beat < beats; beat++) {
    const amp = beat % 4 === 0 ? 1 : 0.28;
    const start = Math.round(beat * beatSec * SR);
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

  it('scores accent patterns and honors the candidate list', () => {
    const four = estimateMeter({ observations: beatSeries(4, 8) });
    const five = estimateMeter({
      observations: beatSeries(5, 8),
      candidateNumerators: [3, 4, 5, 6],
    });
    const defaultFive = estimateMeter({ observations: beatSeries(5, 8) });
    const seven = estimateMeter({
      observations: beatSeries(7, 7),
      candidateNumerators: [3, 4, 5, 6, 7],
    });

    expect(four.timeSignature.numerator).toBe(4);
    expect(four.timeSignature.denominator).toBe(4);
    expect(four.candidates[0].numerator).toBe(4);
    expect(four.downbeatPhase).toBeGreaterThanOrEqual(0);
    expect(four.downbeatPhase).toBeLessThan(4);
    expect(five.timeSignature.numerator).toBe(5);
    expect(defaultFive.timeSignature.numerator).not.toBe(5);
    expect(seven.timeSignature.numerator).toBe(7);
  });

  it('normalizes accent scale and reports requested options', () => {
    const observations = beatSeries(4, 8);
    const scaled = { ...observations, accent: observations.accent.map((value) => value * 20) };
    const base = estimateMeter({ observations });

    expect(estimateMeter({ observations: scaled })).toEqual(base);
    expect(estimateMeter({ observations: { ...observations, hasAttack: [] } })).toEqual(base);
    expect(estimateMeter({ observations, denominator: 8 }).timeSignature.denominator).toBe(8);
    expect(
      estimateMeter({ observations: { ...observations, chordChange: null } as never }),
    ).toEqual(base);
  });

  it('uses measured subdivisions for compound meter and preserves missing measurements', () => {
    const observations = groupedSeries([3, 3]);
    const missing = estimateMeter({ observations });
    const measured = estimateMeter({
      observations: {
        ...observations,
        subdivisionAccent: Array.from({ length: observations.accent.length - 1 }, () => 0.8),
      },
    });
    const measuredZero = estimateMeter({
      observations: {
        ...observations,
        subdivisionAccent: Array.from({ length: observations.accent.length - 1 }, () => 0),
      },
    });

    expect(missing.timeSignature.numerator).toBe(6);
    expect(missing.timeSignature.denominator).toBe(4);
    expect(measured.timeSignature.numerator).toBe(6);
    expect(measured.timeSignature.denominator).toBe(8);
    expect(measuredZero.timeSignature.numerator).toBe(3);
    expect(measuredZero.timeSignature.denominator).toBe(4);
  });

  it('reports additive grouping and a fallback below eight beats', () => {
    const candidateNumerators = [3, 4, 5, 6, 7, 9, 11, 13];
    for (const grouping of [
      [3, 2, 2],
      [2, 3, 2],
      [2, 2, 3],
      [3, 2],
      [2, 3],
    ]) {
      const result = estimateMeter({
        observations: groupedSeries(grouping),
        candidateNumerators,
      });
      expect(result.timeSignature.numerator).toBe(grouping.reduce((sum, part) => sum + part, 0));
      expect(result.grouping).toEqual(grouping);
    }

    const observations = groupedSeries([2, 2]);
    expect(estimateMeter({ observations }).searched).toBe(true);
    for (let count = 1; count < 8; count += 1) {
      const short = estimateMeter({ observations: sliceObservations(observations, 0, count) });
      expect(short.searched).toBe(false);
      expect(short.timeSignature.numerator).toBe(4);
      expect(short.candidateScores.every((score) => score === 0)).toBe(true);
    }
    expect(estimateMeter({ observations: sliceObservations(observations, 0, 8) }).searched).toBe(
      true,
    );
  });

  it('accepts the analysis observation record and returns its meter', () => {
    const result = analyze(clickTrack(32), SR);
    expect(result.beats.length).toBeGreaterThanOrEqual(8);
    const estimate = estimateMeter({ observations: result.beatObservations });

    expect(estimate.searched).toBe(true);
    expect(result.beatObservations.accent).toHaveLength(result.beats.length);
    expect(result.beatObservations.hasAttack).toHaveLength(result.beats.length);
    expect(result.beatObservations.hasAttack.every((value) => typeof value === 'boolean')).toBe(
      true,
    );
    expect(estimate.timeSignature.numerator).toBe(result.timeSignature.numerator);
    expect(estimate.timeSignature.denominator).toBe(result.timeSignature.denominator);
    expect(
      Math.abs(estimate.timeSignature.confidence - result.timeSignature.confidence),
    ).toBeLessThanOrEqual(1e-6);
  });

  it('rejects invalid scoring options', () => {
    const observations = beatSeries(4, 8);
    for (const candidateNumerators of [[], [1], [33], new Array(17).fill(4)]) {
      expect(() => estimateMeter({ observations, candidateNumerators })).toThrow(RangeError);
    }
    expect(() => estimateMeter({ observations, denominator: 3 })).toThrow(RangeError);
    expect(() => estimateMeter({ observations, downbeatWeight: Number.NaN })).toThrow(RangeError);
  });

  it('refuses empty accents, missing streams, malformed values, and mismatched lengths', () => {
    const observations = beatSeries(4, 8);
    expect(() =>
      estimateMeter({ observations: { ...observations, onsetStrength: [] } as never }),
    ).toThrow(/unknown key observations\.onsetStrength/);
    expect(() => estimateMeter({ observations: meterObservations([]) })).toThrow(
      new RangeError('estimateMeter: observations.accent must not be empty'),
    );
    for (const value of [null, 'observations', []]) {
      expect(() => estimateMeter({ observations: value } as never)).toThrow(
        new TypeError('estimateMeter: observations must be an object'),
      );
    }
    expect(() => estimateMeter({ observations: { accent: 3 } } as never)).toThrow(
      new TypeError('estimateMeter: observations.accent must be an array'),
    );
    for (const field of ['hasAttack', 'lowFrequencyEnergy', 'subdivisionAccent']) {
      const incomplete = { ...observations } as Record<string, unknown>;
      delete incomplete[field];
      const path = `observations.${field}`;
      const expected =
        field === 'hasAttack' ? `${path} must be an array of booleans` : `${path} must be an array`;
      expect(() => estimateMeter({ observations: incomplete } as never)).toThrow(
        new TypeError(`estimateMeter: ${expected}`),
      );
    }
    expect(() =>
      estimateMeter({
        observations: { ...observations, hasAttack: [true] },
      }),
    ).toThrow(
      new RangeError(
        'estimateMeter: observations.hasAttack must be empty or have the same length as observations.accent',
      ),
    );
    expect(() =>
      estimateMeter({
        observations: { ...observations, lowFrequencyEnergy: [1] },
      }),
    ).toThrow(
      new RangeError(
        'estimateMeter: observations.lowFrequencyEnergy must be empty or have the same length as observations.accent',
      ),
    );
    expect(() =>
      estimateMeter({
        observations: { ...observations, subdivisionAccent: [1] },
      }),
    ).toThrow(
      new RangeError(
        'estimateMeter: observations.subdivisionAccent must be empty or have one fewer entry than observations.accent',
      ),
    );
    expect(() =>
      estimateMeter({
        observations: { ...observations, hasAttack: [true, 1] } as never,
      }),
    ).toThrow(new TypeError('estimateMeter: observations.hasAttack[1] must be a boolean'));
    expect(() =>
      estimateMeter({
        observations: { ...observations, accent: [1, 'bad'] } as never,
      }),
    ).toThrow(new TypeError('estimateMeter: observations.accent[1] must be a number'));
    expect(() =>
      estimateMeter({
        observations: { ...observations, accent: [-1, ...observations.accent.slice(1)] },
      }),
    ).toThrow(
      new RangeError(
        'estimateMeter: observations.accent must hold only finite values of at least 0',
      ),
    );
  });
});
