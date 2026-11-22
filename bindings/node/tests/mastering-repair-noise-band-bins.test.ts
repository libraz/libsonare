/**
 * The band grid `detectNoiseFloor` reports its per-band floors on.
 *
 * A band whose geometric edge pair rounds to the same bin measures nothing, so
 * the detection leaves it out and names the bands it did measure in
 * `bandFloorBand`. The first test drives the detector alongside the grid and
 * checks that the grid accounts for that list exactly, after asserting that
 * both kinds of band are present at all.
 */

import { describe, expect, it } from 'vitest';
import { masteringRepairDetectNoiseFloor, masteringRepairNoiseBandBins } from '../src/index.js';

/** Bands in the grid; the returned array is one longer. */
const BAND_COUNT = 32;

/** A deterministic pseudo-noise run, so a floor measured here is reproducible. */
function noise(length: number, amp: number): Float32Array {
  const out = new Float32Array(length);
  let state = 12345;
  for (let i = 0; i < length; i += 1) {
    state = (state * 1103515245 + 12345) & 0x7fffffff;
    out[i] = amp * (state / 0x3fffffff - 1);
  }
  return out;
}

/** The band indices the grid says are empty, and the ones it says are measured. */
function partition(bins: Int32Array): { empty: number[]; measured: number[] } {
  const empty: number[] = [];
  const measured: number[] = [];
  for (let k = 0; k < BAND_COUNT; k += 1) {
    (bins[k] === bins[k + 1] ? empty : measured).push(k);
  }
  return { empty, measured };
}

describe('masteringRepairNoiseBandBins', () => {
  it('names exactly the bands of the grid that hold a bin in bandFloorBand', () => {
    const nFft = 1024;
    const sampleRate = 48000;
    const bins = masteringRepairNoiseBandBins({ nFft, sampleRate });
    const { empty, measured } = partition(bins);

    // Without both kinds present the comparison below decides nothing: an
    // all-measured grid never leaves a band out, and an all-empty one never
    // lists one.
    expect(empty.length).toBeGreaterThan(0);
    expect(measured.length).toBeGreaterThan(0);
    expect(empty.length + measured.length).toBe(BAND_COUNT);

    const detected = masteringRepairDetectNoiseFloor({
      samples: noise(8192, 0.05),
      sampleRate,
    });
    expect(detected.bandFloorBand).toBeInstanceOf(Int32Array);
    expect(Array.from(detected.bandFloorBand)).toEqual(measured);
    expect(detected.bandFloorDbfs).toHaveLength(measured.length);
    expect(Array.from(detected.bandFloorDbfs).every(Number.isFinite)).toBe(true);
    expect(Number.isFinite(detected.floorDbfs)).toBe(true);
  });

  it('reports digital silence as -Infinity in every measured band and in the broadband floor', () => {
    const sampleRate = 48000;
    const { measured } = partition(masteringRepairNoiseBandBins({ nFft: 1024, sampleRate }));
    const detected = masteringRepairDetectNoiseFloor({
      samples: new Float32Array(8192),
      sampleRate,
    });
    // Silence is a measurement, so the measured bands stay listed rather than dropping out.
    expect(Array.from(detected.bandFloorBand)).toEqual(measured);
    expect(Array.from(detected.bandFloorDbfs)).toEqual(
      measured.map(() => Number.NEGATIVE_INFINITY),
    );
    expect(detected.floorDbfs).toBe(Number.NEGATIVE_INFINITY);
  });

  it('spans the one-sided spectrum, low to high', () => {
    const nFft = 2048;
    const bins = masteringRepairNoiseBandBins({ nFft, sampleRate: 44100 });
    expect(bins).toHaveLength(BAND_COUNT + 1);
    for (let k = 0; k < BAND_COUNT; k += 1) {
      expect(bins[k + 1], `edge ${k + 1} may not fall below edge ${k}`).toBeGreaterThanOrEqual(
        bins[k],
      );
    }
    expect(bins[BAND_COUNT]).toBe(nFft / 2 + 1);
  });

  it('follows both nFft and sampleRate', () => {
    const base = masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 48000 });
    const finer = masteringRepairNoiseBandBins({ nFft: 4096, sampleRate: 48000 });
    const slower = masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 22050 });

    expect(finer[BAND_COUNT]).toBe(4096 / 2 + 1);
    expect(Array.from(finer)).not.toEqual(Array.from(base));
    // The top edge is nFft/2 + 1 whatever the rate is, so only the interior can
    // show that sampleRate reached the grid at all.
    expect(slower[BAND_COUNT]).toBe(base[BAND_COUNT]);
    expect(Array.from(slower)).not.toEqual(Array.from(base));
    // Finer bins put the bottom edges further apart, so fewer of them round
    // together.
    expect(partition(finer).empty.length).toBeLessThan(partition(base).empty.length);
  });

  it('defaults to the grid the default denoise reports on', () => {
    expect(Array.from(masteringRepairNoiseBandBins({ sampleRate: 22050 }))).toEqual(
      Array.from(masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 22050 })),
    );
  });

  it('refuses an nFft that is not a positive power of two', () => {
    for (const nFft of [1000, 0, -1024]) {
      let caught: unknown;
      try {
        masteringRepairNoiseBandBins({ nFft, sampleRate: 22050 });
      } catch (error) {
        caught = error;
      }
      expect(caught, `nFft ${nFft} should be refused`).toBeInstanceOf(RangeError);
      expect((caught as Error).message).not.toMatch(/sampleRate must be/);
    }
  });

  it('refuses a non-positive sampleRate on its own authority', () => {
    // Worded by the facade, which checks the rate before the request leaves it.
    // The nFft case above is the core's refusal, so this cannot read as the
    // entry point having stopped refusing altogether.
    for (const sampleRate of [0, -48000]) {
      expect(
        () => masteringRepairNoiseBandBins({ sampleRate }),
        `sampleRate ${sampleRate} should be refused`,
      ).toThrow(/sampleRate must be an integer in \[1, 2147483647\]/);
    }
  });

  it('refuses a fractional sampleRate the addon narrowing would truncate', () => {
    // 22050.7 is the shape a fixed probe misses: it narrows onto 22050, which
    // the core accepts, so without this check the call answers for a rate the
    // caller never asked for.
    // Named as a fraction rather than as non-positive, which is what the case
    // above reports: 22050.7 is already positive.
    expect(() => masteringRepairNoiseBandBins({ sampleRate: 22050.7 })).toThrow(
      /sampleRate must be an integer/,
    );
  });

  it('refuses a sampleRate that is not a number rather than substituting its default', () => {
    expect(() =>
      masteringRepairNoiseBandBins({ sampleRate: 'not-a-number' as unknown as number }),
    ).toThrow(new TypeError('masteringRepairNoiseBandBins: sampleRate must be a number'));
  });
});
