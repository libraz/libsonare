import { describe, expect, it } from 'vitest';
import {
  type DereverbClassicalOptions,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassicalStereo,
} from '../src/index.js';

const SR = 22050;
const SECONDS = 0.5;
const LENGTH = Math.floor(SR * SECONDS);

/** Deterministic uniform noise, so every comparison below is repeatable. */
function noise(seed: number, amp: number, length = LENGTH): Float32Array {
  const out = new Float32Array(length);
  let state = seed >>> 0;
  for (let i = 0; i < length; i += 1) {
    state = (state * 1664525 + 1013904223) >>> 0;
    out[i] = amp * (state / 0x7fffffff - 1);
  }
  return out;
}

/** Short bursts separated by silence: nothing sustains across the late lag. */
function bursts(seed: number, length = LENGTH): Float32Array {
  const source = noise(seed, 0.4, length);
  const out = new Float32Array(length);
  const burst = Math.floor(SR * 0.02);
  const period = Math.floor(SR * 0.1);
  for (let i = 0; i < length; i += 1) {
    if (i % period < burst) out[i] = source[i];
  }
  return out;
}

/** A feedback comb, which gives the bursts above a tail that does sustain. */
function reverberant(dry: Float32Array): Float32Array {
  const out = new Float32Array(dry.length);
  const delay = Math.floor(SR * 0.025);
  for (let i = 0; i < dry.length; i += 1) {
    out[i] = dry[i] + (i >= delay ? 0.85 * out[i - delay] : 0);
  }
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

function wetPair(): { left: Float32Array; right: Float32Array } {
  return { left: reverberant(bursts(2468)), right: reverberant(bursts(1357)) };
}

describe('masteringRepairDereverbClassicalStereo', () => {
  it('dereverberates a pair and reports one shared mask', () => {
    const { left, right } = wetPair();
    const result = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);
    // Independent burst seeds, so a channel returned as a copy of the other
    // would fail here.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));

    // A mask that actually subtracted something, over a gate the default
    // threshold of 0 opens for every cell carrying late energy.
    expect(result.report.meanReductionDb).toBeGreaterThan(0.1);
    expect(result.report.suppressedFraction).toBeGreaterThan(0.5);
    expect(result.report.suppressedFraction).toBeLessThanOrEqual(1);
    expect(Number.isFinite(result.report.detected.lateDecayRatioDb)).toBe(true);
    expect(result.report.detected.lateDecayRatioDb).not.toBe(0);
  });

  it('applies one mask unchanged to both channels, so an interchannel level difference survives', () => {
    // The right channel is the left at exactly -6 dB. One shared mask, built
    // from the summed power, scales both by the same gain; a per-channel mask
    // would move the ratio.
    const left = reverberant(bursts(777));
    const right = scaled(left, 0.5);
    const result = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });

    const reference = peak(result.left);
    expect(reference).toBeGreaterThan(0.05);
    let worst = 0;
    for (let i = 0; i < result.left.length; i += 1) {
      worst = Math.max(worst, Math.abs(result.right[i] - 0.5 * result.left[i]));
    }
    expect(worst).toBeLessThan(1e-4 * reference);
  });

  it('leaves both WPE fields at zero unless wpeEnabled, which is the measurement and not an unset field', () => {
    const { left, right } = wetPair();
    const off = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    expect(off.report.detected.latePredictability).toBe(0);
    expect(off.report.wpePredictorNorm).toBe(0);

    // Without this half the test above would pass against a build whose WPE
    // stage does not exist at all.
    const on = masteringRepairDereverbClassicalStereo({
      left,
      right,
      sampleRate: SR,
      wpeEnabled: true,
    });
    expect(on.report.detected.latePredictability).toBeGreaterThan(0);
    expect(on.report.wpePredictorNorm).toBeGreaterThan(0);
    // The reported norm is taken after the clamp and the detection before it,
    // so the pair can only run this way round.
    expect(on.report.wpePredictorNorm).toBeLessThanOrEqual(on.report.detected.latePredictability);
    // The stage reached the samples too, not only the report.
    expect(Array.from(on.left)).not.toEqual(Array.from(off.left));
  });

  it('reads a reverberant input higher than the same material dry', () => {
    // lateDecayRatioDb runs against its name: less negative means the material
    // sustains across the module's late lag, which a tail does and a gapped
    // burst train does not.
    const dryLeft = bursts(2468);
    const dryRight = bursts(1357);
    const dry = masteringRepairDereverbClassicalStereo({
      left: dryLeft,
      right: dryRight,
      sampleRate: SR,
    });
    const wet = masteringRepairDereverbClassicalStereo({
      left: reverberant(dryLeft),
      right: reverberant(dryRight),
      sampleRate: SR,
    });

    for (const value of [
      dry.report.detected.lateDecayRatioDb,
      wet.report.detected.lateDecayRatioDb,
    ]) {
      expect(Number.isFinite(value)).toBe(true);
      // 0 is the "no decay measured" sentinel, so neither reading may be it.
      expect(value).not.toBe(0);
      expect(value).toBeGreaterThan(-200);
      expect(value).toBeLessThan(60);
    }
    expect(wet.report.detected.lateDecayRatioDb).toBeGreaterThan(
      dry.report.detected.lateDecayRatioDb + 1,
    );
  });

  it('admits fewer cells as the threshold gate closes, which is the only observation of that knob', () => {
    const { left, right } = wetPair();
    const open = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    // The knob is a fraction of a bin's own power, bounded to [0, 1]; at 0.9
    // only a cell whose lagged power nearly matches its current one is admitted.
    const gated = masteringRepairDereverbClassicalStereo({
      left,
      right,
      sampleRate: SR,
      threshold: 0.9,
    });

    expect(open.report.suppressedFraction).toBeGreaterThan(0.5);
    expect(gated.report.suppressedFraction).toBeGreaterThanOrEqual(0);
    expect(gated.report.suppressedFraction).toBeLessThan(open.report.suppressedFraction);
    expect(gated.report.meanReductionDb).toBeLessThan(open.report.meanReductionDb);
  });

  it('pads an input shorter than nFft, where the denoise pair rejects one', () => {
    const short = 512;
    const left = reverberant(bursts(31, short));
    const right = reverberant(bursts(32, short));

    const result = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    expect(result.left).toHaveLength(short);
    expect(result.right).toHaveLength(short);
    expect(result.left.every(Number.isFinite)).toBe(true);

    // The sibling entry point refuses the very same input, which is the
    // assertion that catches the two being wired to each other's core call.
    expect(() => masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR })).toThrow();
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = wetPair();
    const bad = (options: DereverbClassicalOptions) =>
      masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR, ...options });

    expect(() =>
      masteringRepairDereverbClassicalStereo({
        left,
        right: right.slice(0, 10),
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo({
        left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo({ left, right, sampleRate: Number.NaN }),
    ).toThrow();
    expect(() => masteringRepairDereverbClassicalStereo({ left, right, sampleRate: 0 })).toThrow();
    expect(() => bad({ nFft: 1000 })).toThrow();
    expect(() => bad({ hopLength: 0 })).toThrow();
    // Unlike the denoise pair, this entry also bounds the hop by nFft.
    expect(() => bad({ nFft: 1024, hopLength: 2048 })).toThrow();
  });
});
