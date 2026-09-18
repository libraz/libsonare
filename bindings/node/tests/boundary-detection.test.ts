/**
 * `detectBoundaries` finds the transitions it is pointed at, and its absolute
 * threshold reaches the detector rather than being accepted and dropped.
 *
 * The threshold is asserted from both sides, because either half alone passes
 * on an option that is read and discarded: raising it above the returned
 * `noveltyPeak` must empty a list that was populated, and lowering it to 0 must
 * populate a list that steady noise leaves empty at the default. Only the pair
 * says the value travelled.
 */

import { describe, expect, it } from 'vitest';
import { detectBoundaries } from '../src/index.js';

const SR = 22050;
const SECTION_SEC = 4;

/** Deterministic noise, so a run's boundary set is the same every time. */
function lcg(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state * 1664525 + 1013904223) >>> 0;
    return state / 0x100000000;
  };
}

function noise(seconds: number, seed: number, sampleRate = SR): Float32Array {
  const rand = lcg(seed);
  const samples = new Float32Array(Math.floor(sampleRate * seconds));
  for (let i = 0; i < samples.length; i++) {
    samples[i] = 0.5 * (rand() * 2 - 1);
  }
  return samples;
}

/** Three timbrally distinct sections: a 220 Hz tone, white noise, a 660 Hz tone. */
function threeSections(): Float32Array {
  const n = SR * SECTION_SEC;
  const samples = new Float32Array(n * 3);
  const rand = lcg(12345);
  for (let i = 0; i < n; i++) {
    samples[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR);
    samples[n + i] = 0.5 * (rand() * 2 - 1);
    samples[2 * n + i] = 0.5 * Math.sin((2 * Math.PI * 660 * i) / SR);
  }
  return samples;
}

describe('detectBoundaries', () => {
  it('lands on the two transitions of a three-section signal', () => {
    const result = detectBoundaries({ samples: threeSections(), sampleRate: SR });

    // The analysis grid, not the input's: `frame` indexes this and `frameStride`
    // is what a pooled run coarsens it by.
    const hopSec = (result.hopLength * result.frameStride) / result.sampleRate;
    const times = result.boundaries.map((boundary) => boundary.time);
    expect(times).toHaveLength(2);
    expect(Math.abs((times[0] as number) - SECTION_SEC)).toBeLessThanOrEqual(2 * hopSec);
    expect(Math.abs((times[1] as number) - 2 * SECTION_SEC)).toBeLessThanOrEqual(2 * hopSec);

    // `frame` is only interpretable against the analysis rate the result carries.
    for (const boundary of result.boundaries) {
      expect(boundary.frame * hopSec).toBeCloseTo(boundary.time, 3);
      expect(boundary.strength).toBeGreaterThan(0);
    }

    expect(result.noveltyCurve).toBeInstanceOf(Float32Array);
    expect(result.noveltyCurve.length).toBe(result.nFrames);
    expect(result.noveltyPeak).toBeGreaterThan(0);
    // The curve is scaled by its own maximum, so it tops out at 1 whatever the
    // raw response was.
    expect(Math.max(...result.noveltyCurve)).toBeCloseTo(1, 5);
  });

  it('empties the list when the absolute threshold is raised above the peak', () => {
    const samples = threeSections();
    const found = detectBoundaries({ samples, sampleRate: SR });
    expect(found.boundaries.length).toBeGreaterThan(0);

    const raised = detectBoundaries({
      samples,
      sampleRate: SR,
      absoluteThreshold: found.noveltyPeak * 1.5,
    });
    expect(raised.boundaries).toEqual([]);
  });

  it('segments steady noise only once the absolute threshold is removed', () => {
    const samples = noise(3 * SECTION_SEC, 999);

    // Nothing changes, so the floor that asks whether the features moved at all
    // is the only thing holding the list empty.
    const atDefault = detectBoundaries({ samples, sampleRate: SR });
    expect(atDefault.boundaries).toEqual([]);
    expect(atDefault.noveltyPeak).toBeLessThan(0.005);

    const withoutFloor = detectBoundaries({ samples, sampleRate: SR, absoluteThreshold: 0 });
    expect(withoutFloor.boundaries.length).toBeGreaterThan(0);
  });

  it('refuses the options the C ABI refuses', () => {
    const samples = noise(SECTION_SEC, 7);
    expect(() => detectBoundaries({ samples, sampleRate: SR, kernelSize: 0 })).toThrow();
    expect(() => detectBoundaries({ samples, sampleRate: SR, nMfcc: 0 })).toThrow();
    expect(() =>
      detectBoundaries({ samples, sampleRate: SR, useMfcc: false, useChroma: false }),
    ).toThrow();
    expect(() =>
      detectBoundaries({ samples, sampleRate: SR, absoluteThreshold: -1 }),
    ).toThrow();
  });

  it('refuses a wrong-typed option by name rather than substituting a default', () => {
    const samples = noise(SECTION_SEC, 7);
    expect(() =>
      detectBoundaries({ samples, sampleRate: SR, threshold: '0.4' as unknown as number }),
    ).toThrow(/threshold/);
    expect(() =>
      detectBoundaries({ samples, sampleRate: SR, useMfcc: 1 as unknown as boolean }),
    ).toThrow(/useMfcc/);
  });
});
