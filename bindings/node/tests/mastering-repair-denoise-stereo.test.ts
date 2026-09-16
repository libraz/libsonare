import { describe, expect, it } from 'vitest';
import {
  type DenoiseClassicalOptions,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassicalStereo,
} from '../src/index.js';

const SR = 22050;
const SECONDS = 0.5;
const LENGTH = Math.floor(SR * SECONDS);

/** Deterministic uniform noise, so every level comparison below is repeatable. */
function noise(seed: number, amp: number, length = LENGTH): Float32Array {
  const out = new Float32Array(length);
  let state = seed >>> 0;
  for (let i = 0; i < length; i += 1) {
    state = (state * 1664525 + 1013904223) >>> 0;
    out[i] = amp * (state / 0x7fffffff - 1);
  }
  return out;
}

function tone(freq: number, amp: number, length = LENGTH): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i += 1) out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  return out;
}

function add(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i += 1) out[i] = a[i] + b[i];
  return out;
}

function scaled(a: Float32Array, factor: number): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i += 1) out[i] = a[i] * factor;
  return out;
}

function peak(a: Float32Array): number {
  let max = 0;
  for (const value of a) max = Math.max(max, Math.abs(value));
  return max;
}

/** Two distinguishable tones over a common noise bed. */
function pair(): { left: Float32Array; right: Float32Array } {
  const bed = noise(12345, 0.02);
  return { left: add(tone(700, 0.3), bed), right: add(tone(1900, 0.3), bed) };
}

describe('masteringRepairDenoiseClassicalStereo', () => {
  it('denoises a pair and reports one shared mask', () => {
    const { left, right } = pair();
    const result = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // The two content tones survive the mask, so the channels stay distinct --
    // the assertion that would fail if one channel were a copy of the other.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));

    // A real noise bed at -34 dBFS: the floor must land well inside the dB
    // range rather than on the report's floor sentinel or at 0.
    expect(result.report.detected.floorDbfs).toBeGreaterThan(-90);
    expect(result.report.detected.floorDbfs).toBeLessThan(-10);
    expect(result.report.detected.bandFloorDbfs).toHaveLength(32);
    expect(result.report.detected.bandFloorDbfs.every(Number.isFinite)).toBe(true);

    // A mask that actually attenuated something, bounded by the configured
    // depth: at the default reductionDb of 26 no cell may report more.
    expect(result.report.meanReductionDb).toBeGreaterThan(0.5);
    expect(result.report.maxReductionDb).toBeGreaterThan(result.report.meanReductionDb);
    expect(result.report.maxReductionDb).toBeLessThanOrEqual(26.001);
    expect(result.report.floorLimitedFraction).toBeGreaterThanOrEqual(0);
    expect(result.report.floorLimitedFraction).toBeLessThanOrEqual(1);
  });

  it('applies one mask unchanged to both channels, so an interchannel level difference survives', () => {
    // The right channel is the left at exactly -6 dB. One shared mask scales
    // both by the same gain, so the ratio comes back untouched; a per-channel
    // mask would gain the quieter channel differently and move it.
    const left = add(tone(700, 0.3), noise(4242, 0.02));
    const right = scaled(left, 0.5);
    const result = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });

    const reference = peak(result.left);
    expect(reference).toBeGreaterThan(0.01);
    let worst = 0;
    for (let i = 0; i < result.left.length; i += 1) {
      worst = Math.max(worst, Math.abs(result.right[i] - 0.5 * result.left[i]));
    }
    expect(worst).toBeLessThan(1e-4 * reference);
  });

  it('reports an absolute pair-level floor, measured on the channel-summed power', () => {
    // Every level here is absolute dBFS taken on the summed power, which is
    // what makes a stereo floor comparable only against another stereo floor.
    const { left } = pair();
    const silent = new Float32Array(left.length);

    const both = masteringRepairDenoiseClassicalStereo({ left, right: left, sampleRate: SR });
    const oneSided = masteringRepairDenoiseClassicalStereo({ left, right: silent, sampleRate: SR });
    const halved = masteringRepairDenoiseClassicalStereo({
      left: scaled(left, 0.5),
      right: scaled(left, 0.5),
      sampleRate: SR,
    });

    for (const floor of [
      both.report.detected.floorDbfs,
      oneSided.report.detected.floorDbfs,
      halved.report.detected.floorDbfs,
    ]) {
      expect(floor).toBeGreaterThan(-90);
      expect(floor).toBeLessThan(-10);
    }

    // Twice the summed power is 3.01 dB, so carrying the same material in both
    // channels reads that much above carrying it in one.
    expect(both.report.detected.floorDbfs - oneSided.report.detected.floorDbfs).toBeCloseTo(3.01, 1);
    // Half the amplitude is a quarter of the power: an absolute level follows
    // it, a ratio would not move at all.
    expect(both.report.detected.floorDbfs - halved.report.detected.floorDbfs).toBeCloseTo(6.02, 1);
  });

  it('reports no floor-limited fraction in spectralSubtraction mode, which is the mode and not a measurement', () => {
    // A shallow reduction depth puts many cells on the gain floor, so the
    // default mode has a nonzero fraction to lose -- without that the zero
    // below would pass against an implementation that never fills the field.
    const { left, right } = pair();
    const logMmse = masteringRepairDenoiseClassicalStereo({
      left,
      right,
      sampleRate: SR,
      reductionDb: 6,
    });
    expect(logMmse.report.floorLimitedFraction).toBeGreaterThan(0);

    const spectral = masteringRepairDenoiseClassicalStereo({
      left,
      right,
      sampleRate: SR,
      reductionDb: 6,
      mode: 'spectralSubtraction',
    });
    // A mask genuinely ran in this mode, so the 0 is that mode flooring on
    // spectralFloor rather than nothing having happened.
    expect(spectral.report.meanReductionDb).toBeGreaterThan(0.5);
    expect(spectral.report.floorLimitedFraction).toBe(0);
  });

  it('reads overSubtraction and spectralFloor only in spectralSubtraction, and the gain options only in the others', () => {
    const { left, right } = pair();
    const run = (options: DenoiseClassicalOptions) =>
      Array.from(
        masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR, ...options }).left,
      );

    // Default mode (logMmse): the Berouti knobs are dead and the gain knobs
    // are live.
    expect(run({ overSubtraction: 2 })).toEqual(run({ overSubtraction: 8 }));
    expect(run({ spectralFloor: 0.05 })).toEqual(run({ spectralFloor: 0.5 }));
    expect(run({ gainSmoothing: true })).not.toEqual(run({ gainSmoothing: false }));
    expect(run({ speechPresenceGain: true })).not.toEqual(run({ speechPresenceGain: false }));

    // spectralSubtraction: exactly the other way round.
    const ss: DenoiseClassicalOptions = { mode: 'spectralSubtraction' };
    expect(run({ ...ss, overSubtraction: 2 })).not.toEqual(run({ ...ss, overSubtraction: 8 }));
    expect(run({ ...ss, spectralFloor: 0.05 })).not.toEqual(run({ ...ss, spectralFloor: 0.5 }));
    expect(run({ ...ss, gainSmoothing: true })).toEqual(run({ ...ss, gainSmoothing: false }));
    expect(run({ ...ss, speechPresenceGain: true })).toEqual(
      run({ ...ss, speechPresenceGain: false }),
    );
  });

  it('rejects an input shorter than nFft, where the dereverb pair pads one', () => {
    const short = 512;
    const left = add(tone(700, 0.3, short), noise(99, 0.02, short));
    const right = add(tone(1900, 0.3, short), noise(98, 0.02, short));

    expect(() => masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR })).toThrow();

    // The rejection is about nFft, not about a short buffer: the same input
    // goes through once the window fits inside it.
    const fitted = masteringRepairDenoiseClassicalStereo({
      left,
      right,
      sampleRate: SR,
      nFft: 256,
    });
    expect(fitted.left).toHaveLength(short);

    // The sibling entry point takes the very same rejected input, which is the
    // assertion that catches the two being wired to each other's core call.
    const padded = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    expect(padded.left).toHaveLength(short);
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = pair();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right: right.slice(0, 10), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: Number.NaN }),
    ).toThrow();
    expect(() => masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: 0 })).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR, nFft: 1000 }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR, hopLength: 0 }),
    ).toThrow();
  });
});
