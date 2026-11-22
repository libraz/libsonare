/**
 * Tests for `masteringRepairNoiseBandBins`, the bin grid `masteringRepairDetectNoiseFloor`
 * reports `bandFloorDbfs` on.
 *
 * A band with no bin measures nothing, so the detection leaves it out and names the bands it
 * did measure in `bandFloorBand`. The central case here is whether the grid EXPLAINS that list:
 * the bands the grid calls non-empty must be exactly the ones the detection reports. That
 * comparison decides nothing unless both sets are non-empty, so their counts are asserted first.
 *
 * This surface calls the core directly rather than through the C ABI, so the power-of-two rule
 * on `nFft` — which lives in the C ABI layer, not in the core — is the wrapper's own. The
 * rejection of `nFft: 1000` is what holds it here.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDetectNoiseFloor, masteringRepairNoiseBandBins } from '../src/index';

const SR = 48000;
const N_FFT = 1024;
const BAND_COUNT = 32;
const EDGE_COUNT = BAND_COUNT + 1;

// Deterministic LCG noise: broadband enough that every band holding a bin measures a finite
// level, and the same fixture every run.
function noise(frames: number, amp: number, seed: number): Float32Array {
  let state = seed >>> 0;
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const u = (state >>> 8) / (1 << 24);
    out[i] = (u - 0.5) * amp;
  }
  return out;
}

describe('masteringRepairNoiseBandBins', () => {
  beforeAll(async () => {
    await init();
  });

  it('names exactly the bands of the grid that hold a bin in bandFloorBand', () => {
    const bins = masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: SR });
    const detected = masteringRepairDetectNoiseFloor({
      samples: noise(SR, 0.2, 1),
      sampleRate: SR,
      nFft: N_FFT,
    });

    const empty: number[] = [];
    const filled: number[] = [];
    for (let k = 0; k < BAND_COUNT; k++) {
      (bins[k] === bins[k + 1] ? empty : filled).push(k);
    }

    // Without both sets populated the comparison below would hold for a grid that
    // called every band empty, or none of them.
    expect(empty.length).toBeGreaterThan(0);
    expect(filled.length).toBeGreaterThan(0);
    expect(empty.length + filled.length).toBe(BAND_COUNT);
    // Integer geometry, so the split is exact rather than approximate.
    expect(empty.length).toBe(6);
    expect(filled.length).toBe(26);

    expect(detected.bandFloorBand).toBeInstanceOf(Int32Array);
    expect(Array.from(detected.bandFloorBand)).toEqual(filled);
    expect(detected.bandFloorDbfs).toHaveLength(filled.length);
    expect(Array.from(detected.bandFloorDbfs).every(Number.isFinite)).toBe(true);
    expect(Number.isFinite(detected.floorDbfs)).toBe(true);

    // A band comes out empty only where the geometric edges sit closer together
    // than the bin spacing, which is the low end -- band 0 still holds DC, so the
    // empty ones are not a prefix. Above the last of them every band holds a bin.
    const lastEmpty = Math.max(...empty);
    expect(lastEmpty).toBeLessThan(BAND_COUNT / 2);
    for (let k = lastEmpty + 1; k < BAND_COUNT; k++) {
      expect(bins[k + 1]).toBeGreaterThan(bins[k] as number);
    }
  });

  it('reports digital silence as -Infinity in every measured band and in the broadband floor', () => {
    const bins = masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: SR });
    const filled: number[] = [];
    for (let k = 0; k < BAND_COUNT; k++) {
      if (bins[k] !== bins[k + 1]) {
        filled.push(k);
      }
    }
    const detected = masteringRepairDetectNoiseFloor({
      samples: new Float32Array(SR),
      sampleRate: SR,
      nFft: N_FFT,
    });
    // Silence is a measurement, so the measured bands stay listed rather than dropping out.
    expect(Array.from(detected.bandFloorBand)).toEqual(filled);
    expect(Array.from(detected.bandFloorDbfs)).toEqual(filled.map(() => Number.NEGATIVE_INFINITY));
    expect(detected.floorDbfs).toBe(Number.NEGATIVE_INFINITY);
  });

  it('returns one more index than there are bands, non-decreasing, ending at the spectrum', () => {
    const bins = masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: SR });
    expect(bins).toBeInstanceOf(Int32Array);
    expect(bins.length).toBe(EDGE_COUNT);
    for (let k = 1; k < EDGE_COUNT; k++) {
      expect(bins[k]).toBeGreaterThanOrEqual(bins[k - 1] as number);
    }
    expect(bins[0]).toBe(0);
    expect(bins[EDGE_COUNT - 1]).toBe(N_FFT / 2 + 1);
  });

  it('tracks nFft and sampleRate independently', () => {
    const base = masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: SR });
    const wider = masteringRepairNoiseBandBins({ nFft: 2048, sampleRate: SR });
    const slower = masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: 22050 });

    // A constant grid would pass every shape assertion above; neither of these
    // agrees with the base, and the last edge follows nFft alone.
    expect(Array.from(wider)).not.toEqual(Array.from(base));
    expect(Array.from(slower)).not.toEqual(Array.from(base));
    expect(wider[EDGE_COUNT - 1]).toBe(2048 / 2 + 1);
    expect(slower[EDGE_COUNT - 1]).toBe(N_FFT / 2 + 1);

    // Halving the rate halves the bin spacing, so the lower rate resolves bands
    // the higher one leaves empty.
    const emptyAt = (grid: Int32Array) => {
      let n = 0;
      for (let k = 0; k < BAND_COUNT; k++) {
        if (grid[k] === grid[k + 1]) {
          n++;
        }
      }
      return n;
    };
    expect(emptyAt(slower)).toBeLessThan(emptyAt(base));
  });

  it('rejects a non-power-of-two nFft, which the core does not check', () => {
    // The regression this file exists for: the core validates positivity only,
    // and the C ABI layer that carries the power-of-two rule is not a binding
    // source here, so accepting 1000 would put this surface alone out of step.
    expect(() => masteringRepairNoiseBandBins({ nFft: 1000, sampleRate: SR })).toThrow(
      /masteringRepairNoiseBandBins: nFft/,
    );
  });

  it('rejects a non-positive, non-finite or wrong-typed argument', () => {
    expect(() => masteringRepairNoiseBandBins({ nFft: 0, sampleRate: SR })).toThrow(
      /masteringRepairNoiseBandBins: nFft/,
    );
    expect(() => masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: 0 })).toThrow(
      /masteringRepairNoiseBandBins: sampleRate/,
    );
    expect(() => masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: Number.NaN })).toThrow(
      /sampleRate/,
    );
    expect(() =>
      masteringRepairNoiseBandBins({
        nFft: N_FFT,
        sampleRate: 'not-a-number' as unknown as number,
      }),
    ).toThrow(/sampleRate/);
    expect(() => masteringRepairNoiseBandBins({ nFft: N_FFT, sampleRate: 48000.5 })).toThrow(
      /sampleRate/,
    );
  });

  it('defaults nFft to 1024 and answers differently at a different rate', () => {
    const expected = Array.from(masteringRepairNoiseBandBins({ nFft: 1024, sampleRate: 22050 }));
    expect(Array.from(masteringRepairNoiseBandBins({ sampleRate: 22050 }))).toEqual(expected);

    // The default is a real choice rather than whatever the last call used:
    // a different rate answers differently.
    expect(Array.from(masteringRepairNoiseBandBins({ sampleRate: 48000 }))).not.toEqual(expected);
  });
});
