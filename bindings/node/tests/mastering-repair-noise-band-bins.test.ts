/**
 * The band grid `detectNoiseFloor` reports its per-band floors on.
 *
 * The case this entry point exists for is the empty band: a geometric edge pair
 * that rounds to the same bin measures nothing, and its floor comes back as the
 * sentinel -- the same value a genuinely silent band would read. Only the grid
 * separates the two, so the first test here drives the detector alongside the
 * grid and checks that the grid actually accounts for every sentinel, after
 * asserting that both kinds of band are present at all.
 */

import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  masteringRepairDetectNoiseFloor,
  masteringRepairNoiseBandBins,
} from '../src/index.js';

/** Bands in the grid; the returned array is one longer. */
const BAND_COUNT = 32;

/** The level an unmeasured band reports, in dBFS. */
const FLOOR_SENTINEL = -120;

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
  it('accounts for every sentinel band the detector reports', () => {
    const nFft = 1024;
    const sampleRate = 48000;
    const bins = masteringRepairNoiseBandBins({ nFft, sampleRate });
    const { empty, measured } = partition(bins);

    // Without both kinds present the comparison below decides nothing: an
    // all-measured grid never reaches the sentinel branch, and an all-empty one
    // never reaches the other.
    expect(empty.length).toBeGreaterThan(0);
    expect(measured.length).toBeGreaterThan(0);
    expect(empty.length + measured.length).toBe(BAND_COUNT);

    const detected = masteringRepairDetectNoiseFloor({
      samples: noise(8192, 0.05),
      sampleRate,
    });
    expect(detected.bandFloorDbfs).toHaveLength(BAND_COUNT);
    for (const k of empty) {
      expect(detected.bandFloorDbfs[k], `band ${k} covers no bin`).toBe(FLOOR_SENTINEL);
    }
    for (const k of measured) {
      expect(
        detected.bandFloorDbfs[k],
        `band ${k} covers bins [${bins[k]}, ${bins[k + 1]})`,
      ).toBeGreaterThan(FLOOR_SENTINEL);
    }
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
    expect(Array.from(masteringRepairNoiseBandBins({}))).toEqual(
      Array.from(masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 22050 })),
    );
  });

  it('refuses an nFft that is not a positive power of two', () => {
    for (const nFft of [1000, 0, -1024]) {
      let caught: { code?: number } | undefined;
      try {
        masteringRepairNoiseBandBins({ nFft });
      } catch (error) {
        caught = error as { code?: number };
      }
      expect(caught, `nFft ${nFft} should be refused`).toBeDefined();
      expect(caught?.code).toBe(ErrorCode.InvalidParameter);
    }
  });

  it('refuses a non-positive sampleRate on its own authority', () => {
    // A RangeError rather than the addon's coded refusal: the facade checks the
    // rate before the request leaves it. The nFft case above is untouched and
    // still carries the code, so this cannot read as the entry point having
    // stopped refusing altogether.
    for (const sampleRate of [0, -48000]) {
      expect(
        () => masteringRepairNoiseBandBins({ sampleRate }),
        `sampleRate ${sampleRate} should be refused`,
      ).toThrow(/sampleRate must be a positive integer/);
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
    ).toThrow(/sampleRate must be an integer/);
  });
});
