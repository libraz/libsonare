/**
 * `detectBoundaries`: the transitions, the novelty curve, and the two
 * thresholds that decide which peaks become boundaries.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { detectBoundaries, ErrorCode, init, SonareError } from '../dist/index.js';

const SAMPLE_RATE = 22050;
const HOP = 512;
const SECTION_SEC = 4;

/** Deterministic PRNG, so a noise section is the same signal on every run. */
function noiseSource(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = Math.imul(state ^ (state >>> 15), state | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function whiteNoise(durationSec: number, seed = 1, amp = 0.5): Float32Array {
  const rand = noiseSource(seed);
  const out = new Float32Array(Math.floor(SAMPLE_RATE * durationSec));
  for (let i = 0; i < out.length; i++) {
    out[i] = amp * (rand() * 2 - 1);
  }
  return out;
}

function tone(freqHz: number, durationSec: number, amp = 0.5): Float32Array {
  const out = new Float32Array(Math.floor(SAMPLE_RATE * durationSec));
  for (let i = 0; i < out.length; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / SAMPLE_RATE);
  }
  return out;
}

function concat(parts: Float32Array[]): Float32Array {
  const out = new Float32Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

/** 220 Hz tone, white noise, 660 Hz tone -- two transitions, at 4 s and 8 s. */
function threeSections(): Float32Array {
  return concat([tone(220, SECTION_SEC), whiteNoise(SECTION_SEC), tone(660, SECTION_SEC)]);
}

describe('detectBoundaries', () => {
  beforeAll(async () => {
    await init();
  });

  it('finds the two transitions of a three-section signal', () => {
    const result = detectBoundaries({ samples: threeSections(), sampleRate: SAMPLE_RATE });

    // The grid the frame indices live on: an input at or below 22050 Hz is not
    // resampled, and nothing here is long enough to pool.
    expect(result.sampleRate).toBe(SAMPLE_RATE);
    expect(result.hopLength).toBe(HOP);
    expect(result.frameStride).toBe(1);
    expect(result.nFrames).toBeGreaterThan(0);
    expect(result.noveltyCurve.length).toBe(result.nFrames);
    expect(result.noveltyPeak).toBeGreaterThan(0);

    const tolerance = (2 * HOP) / SAMPLE_RATE;
    const times = result.boundaries.map((b) => b.time);
    for (const expected of [SECTION_SEC, 2 * SECTION_SEC]) {
      const nearest = times.reduce(
        (best, t) => (Math.abs(t - expected) < Math.abs(best - expected) ? t : best),
        Number.POSITIVE_INFINITY,
      );
      expect(Math.abs(nearest - expected)).toBeLessThanOrEqual(tolerance);
    }

    // `frame` indexes the same grid `time` is measured on, which is what makes
    // it interpretable at all.
    for (const boundary of result.boundaries) {
      expect(boundary.frame).toBe(Math.round((boundary.time * result.sampleRate) / HOP));
      expect(boundary.strength).toBeGreaterThan(0);
    }
  });

  it('empties the list when the absolute floor is raised above the peak it reads', () => {
    const samples = threeSections();
    const found = detectBoundaries({ samples, sampleRate: SAMPLE_RATE });
    expect(found.boundaries.length).toBeGreaterThan(0);

    // The floor is applied to the raw response, whose maximum is noveltyPeak, so
    // a floor above it can admit nothing. The pair is the control: the same
    // signal answers with boundaries and without, and only the threshold moved.
    const refused = detectBoundaries({
      samples,
      sampleRate: SAMPLE_RATE,
      absoluteThreshold: found.noveltyPeak * 1.1,
    });
    expect(refused.boundaries).toEqual([]);
    // The curve is still computed and still says what it said; only the
    // selection changed.
    expect(refused.noveltyPeak).toBeCloseTo(found.noveltyPeak, 6);
  });

  it('segments steady noise only once the absolute floor is removed', () => {
    const samples = whiteNoise(3 * SECTION_SEC);
    // Nothing changes in this signal, so the default floor answers with nothing
    // -- the relative threshold alone would not, since the curve is scaled by
    // its own maximum and residual fluctuation reaches 1.0.
    expect(detectBoundaries({ samples, sampleRate: SAMPLE_RATE }).boundaries).toEqual([]);
    expect(
      detectBoundaries({ samples, sampleRate: SAMPLE_RATE, absoluteThreshold: 0 }).boundaries
        .length,
    ).toBeGreaterThan(0);
  });

  it('refuses a configuration with neither feature stream', () => {
    expect(() =>
      detectBoundaries({
        samples: tone(220, 1),
        sampleRate: SAMPLE_RATE,
        useMfcc: false,
        useChroma: false,
      }),
    ).toThrow(SonareError);
    try {
      detectBoundaries({
        samples: tone(220, 1),
        sampleRate: SAMPLE_RATE,
        useMfcc: false,
        useChroma: false,
      });
      expect.unreachable('expected a refusal');
    } catch (error) {
      expect((error as SonareError).code).toBe(ErrorCode.InvalidParameter);
    }
  });

  // The facade calls the detector directly rather than through the C ABI, so the
  // refusals live in the embind wrapper as well as in TypeScript. Driving the
  // module directly is the only way to reach that copy: every case above is
  // answered by the TypeScript layer first, and a backstop nothing can reach is
  // a backstop nothing has measured.
  it('carries the refusals in the embind wrapper, below the TypeScript guards', async () => {
    const createModule = (await import('../dist/sonare.js')).default;
    const module = await createModule();
    const samples = tone(220, 1);
    const valid = {
      nFft: 2048,
      hopLength: 512,
      kernelSize: 64,
      threshold: 0.3,
      absoluteThreshold: 0.005,
      nMfcc: 13,
      nChroma: 12,
      peakDistance: 2.0,
      useMfcc: true,
      useChroma: true,
    };
    // Non-vacuity: the same call with the same route succeeds.
    expect(module.detectBoundaries(samples, SAMPLE_RATE, valid).sampleRate).toBe(SAMPLE_RATE);
    for (const bad of [
      { nFft: 0 },
      { hopLength: 0 },
      { kernelSize: 0 },
      { nMfcc: 0 },
      { nChroma: 0 },
      { threshold: Number.NaN },
      { absoluteThreshold: -1 },
      { peakDistance: Number.POSITIVE_INFINITY },
      { useMfcc: false, useChroma: false },
    ]) {
      expect(() => module.detectBoundaries(samples, SAMPLE_RATE, { ...valid, ...bad })).toThrow();
    }
  });

  it('refuses non-positive sizing and a non-finite threshold', () => {
    const samples = tone(220, 1);
    expect(() => detectBoundaries({ samples, sampleRate: SAMPLE_RATE, nFft: 0 })).toThrow(
      RangeError,
    );
    expect(() => detectBoundaries({ samples, sampleRate: SAMPLE_RATE, kernelSize: -1 })).toThrow(
      RangeError,
    );
    expect(() => detectBoundaries({ samples, sampleRate: SAMPLE_RATE, nMfcc: 0 })).toThrow(
      RangeError,
    );
    expect(() =>
      detectBoundaries({ samples, sampleRate: SAMPLE_RATE, threshold: Number.NaN }),
    ).toThrow(RangeError);
    expect(() => detectBoundaries({ samples, sampleRate: SAMPLE_RATE, peakDistance: -1 })).toThrow(
      RangeError,
    );
  });
});
