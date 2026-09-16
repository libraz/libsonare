import { describe, expect, it } from 'vitest';
import {
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
} from '../src/index.js';

const SR = 22050;
const LENGTH = Math.floor(SR * 0.5);

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
  for (let i = 0; i < length; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function add(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i += 1) {
    out[i] = a[i] + b[i];
  }
  return out;
}

function peak(a: Float32Array): number {
  let max = 0;
  for (const value of a) {
    max = Math.max(max, Math.abs(value));
  }
  return max;
}

/**
 * Index of the first element the two buffers disagree on, or -1 when they are
 * identical. A length disagreement reports 0, which fails the same assertion.
 */
function firstDifference(a: Float32Array, b: Float32Array): number {
  if (a.length !== b.length) {
    return 0;
  }
  for (let i = 0; i < a.length; i += 1) {
    if (a[i] !== b[i]) {
      return i;
    }
  }
  return -1;
}

/** Two distinguishable tones over a common noise bed. */
function pair(): { left: Float32Array; right: Float32Array } {
  const bed = noise(12345, 0.02);
  return { left: add(tone(700, 0.3), bed), right: add(tone(1900, 0.3), bed) };
}

const silence = (length = LENGTH) => new Float32Array(length);

describe('masteringRepairDenoiseClassicalLinked', () => {
  it('reproduces the mono entry from one channel, sample for sample', () => {
    const { left } = pair();
    const mono = masteringRepairDenoiseClassical({ samples: left, sampleRate: SR });
    const linked = masteringRepairDenoiseClassicalLinked({ channels: [left], sampleRate: SR });

    expect(linked.channels).toHaveLength(1);
    expect(linked.channels[0]).toHaveLength(left.length);
    expect(peak(linked.channels[0])).toBeGreaterThan(0.01);
    expect(firstDifference(linked.channels[0], mono)).toBe(-1);
  });

  it('reproduces the stereo pair from two channels, plane for plane', () => {
    // Two different non-silent channels, so a swapped pair fails rather than
    // passing on two copies of one signal.
    const { left, right } = pair();
    const stereo = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });
    const linked = masteringRepairDenoiseClassicalLinked({
      channels: [left, right],
      sampleRate: SR,
    });

    expect(linked.channels).toHaveLength(2);
    expect(firstDifference(linked.channels[0], stereo.left)).toBe(-1);
    expect(firstDifference(linked.channels[1], stereo.right)).toBe(-1);
    expect(firstDifference(linked.channels[0], linked.channels[1])).not.toBe(-1);
    expect(linked.report).toEqual(stereo.report);
  });

  it('reports a set-level floor that rises with the channel count while the fractions hold', () => {
    // The floor is referred to the summed mean square, so N copies of one
    // channel read 10*log10(N) above that channel alone. The attenuation
    // figures are fractions of the same mask and do not move at all.
    const { left } = pair();
    const floorOf = (count: number) =>
      masteringRepairDenoiseClassicalLinked({
        channels: Array.from({ length: count }, () => left),
        sampleRate: SR,
      }).report;

    const one = floorOf(1);
    const two = floorOf(2);
    const three = floorOf(3);

    expect(one.detected.floorDbfs).toBeGreaterThan(-90);
    expect(one.detected.floorDbfs).toBeLessThan(-10);
    expect(two.detected.floorDbfs - one.detected.floorDbfs).toBeCloseTo(10 * Math.log10(2), 3);
    expect(three.detected.floorDbfs - one.detected.floorDbfs).toBeCloseTo(10 * Math.log10(3), 3);

    // A mask that genuinely attenuated something, so the equalities below are
    // three copies of a measurement rather than three copies of zero.
    expect(one.meanReductionDb).toBeGreaterThan(0.5);
    expect(one.floorLimitedFraction).toBeGreaterThan(0);
    expect(three.meanReductionDb).toBe(one.meanReductionDb);
    expect(three.maxReductionDb).toBe(one.maxReductionDb);
    expect(three.floorLimitedFraction).toBe(one.floorLimitedFraction);
  });

  it('gives three identical channels one shared mask', () => {
    const { left } = pair();
    const linked = masteringRepairDenoiseClassicalLinked({
      channels: [left, left, left],
      sampleRate: SR,
    });

    expect(linked.channels).toHaveLength(3);
    expect(peak(linked.channels[0])).toBeGreaterThan(0.01);
    expect(firstDifference(linked.channels[0], linked.channels[1])).toBe(-1);
    expect(firstDifference(linked.channels[0], linked.channels[2])).toBe(-1);
  });

  it('routes each channel to its own output plane', () => {
    // A silent channel contributes exactly zero to the summed power, so both
    // runs build the same mask from the same content and the moved channel must
    // come back bit-identical from the plane it was fed into.
    const { left } = pair();
    const first = masteringRepairDenoiseClassicalLinked({
      channels: [left, silence(), silence()],
      sampleRate: SR,
    });
    const second = masteringRepairDenoiseClassicalLinked({
      channels: [silence(), left, silence()],
      sampleRate: SR,
    });

    expect(peak(first.channels[0])).toBeGreaterThan(0.01);
    expect(firstDifference(first.channels[0], second.channels[1])).toBe(-1);
    // Length-checked before the emptiness assertions below, which a zero-length
    // plane would satisfy by having nothing to look at.
    expect(first.channels.map((plane) => plane.length)).toEqual([LENGTH, LENGTH, LENGTH]);
    expect(second.channels.map((plane) => plane.length)).toEqual([LENGTH, LENGTH, LENGTH]);
    expect(first.channels[1].every((value) => value === 0)).toBe(true);
    expect(first.channels[2].every((value) => value === 0)).toBe(true);
    expect(second.channels[0].every((value) => value === 0)).toBe(true);
    expect(second.channels[2].every((value) => value === 0)).toBe(true);
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left } = pair();
    const withNan = Float32Array.from(left);
    withNan[1000] = Number.NaN;

    expect(() => masteringRepairDenoiseClassicalLinked({ channels: [], sampleRate: SR })).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalLinked({
        channels: [left, left.slice(0, 4096)],
        sampleRate: SR,
      }),
    ).toThrow(/same length/);
    // Past the first channel: every channel is validated, not just channel 0.
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [left, left, withNan], sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [left], sampleRate: Number.NaN }),
    ).toThrow(/sampleRate must be an integer/);
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [left], sampleRate: SR, nFft: 1000 }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [left], sampleRate: SR, hopLength: 0 }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalLinked({
        channels: [left, [1, 2, 3] as unknown as Float32Array],
        sampleRate: SR,
      }),
    ).toThrow(/Float32Array/);
  });
});

describe('masteringRepairDereverbClassicalLinked', () => {
  it('reproduces the mono entry from one channel, sample for sample', () => {
    const { left } = pair();
    const mono = masteringRepairDereverbClassical({ samples: left, sampleRate: SR });
    const linked = masteringRepairDereverbClassicalLinked({ channels: [left], sampleRate: SR });

    expect(linked.channels).toHaveLength(1);
    expect(peak(linked.channels[0])).toBeGreaterThan(0.01);
    expect(firstDifference(linked.channels[0], mono)).toBe(-1);
  });

  it('reproduces the stereo pair from two channels, plane for plane', () => {
    const { left, right } = pair();
    const stereo = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    const linked = masteringRepairDereverbClassicalLinked({
      channels: [left, right],
      sampleRate: SR,
    });

    expect(linked.channels).toHaveLength(2);
    expect(firstDifference(linked.channels[0], stereo.left)).toBe(-1);
    expect(firstDifference(linked.channels[1], stereo.right)).toBe(-1);
    expect(firstDifference(linked.channels[0], linked.channels[1])).not.toBe(-1);
    expect(linked.report).toEqual(stereo.report);
  });

  it('reports ratios that do not move with the channel count', () => {
    // Every field is a ratio or a fraction, so three copies of one channel
    // report exactly what that channel reports alone -- the asymmetry against
    // the denoise floor above.
    const { left } = pair();
    const one = masteringRepairDereverbClassicalLinked({ channels: [left], sampleRate: SR }).report;
    const three = masteringRepairDereverbClassicalLinked({
      channels: [left, left, left],
      sampleRate: SR,
    }).report;

    expect(one.meanReductionDb).toBeGreaterThan(0.1);
    expect(one.suppressedFraction).toBeGreaterThan(0);
    expect(one.detected.lateDecayRatioDb).toBeLessThan(0);
    expect(three.meanReductionDb).toBe(one.meanReductionDb);
    expect(three.suppressedFraction).toBe(one.suppressedFraction);
    expect(three.wpePredictorNorm).toBe(one.wpePredictorNorm);
    expect(three.detected.latePredictability).toBe(one.detected.latePredictability);
    // The one field summed over the channels before the ratio is taken, so it
    // can land a ULP off on material where the accumulation order matters. Five
    // decimal places is six orders below the 4.77 dB a count-following quantity
    // moves by, so this still fails anything that actually tracks the set size.
    expect(three.detected.lateDecayRatioDb).toBeCloseTo(one.detected.lateDecayRatioDb, 5);
  });

  it('keeps the WPE stage set-wide, within what accumulating over three channels costs', () => {
    // With the stage on, the predictor figures are nonzero, so this compares a
    // measurement rather than two unset fields. They are still ratios: the only
    // movement is the float accumulation of three channels' statistics.
    const { left } = pair();
    const run = (count: number) =>
      masteringRepairDereverbClassicalLinked({
        channels: Array.from({ length: count }, () => left),
        sampleRate: SR,
        wpeEnabled: true,
      }).report;

    const one = run(1);
    const three = run(3);

    expect(one.detected.latePredictability).toBeGreaterThan(0);
    expect(one.wpePredictorNorm).toBeGreaterThan(0);
    expect(three.detected.latePredictability).toBeCloseTo(one.detected.latePredictability, 3);
    expect(three.wpePredictorNorm).toBe(one.wpePredictorNorm);
    expect(three.meanReductionDb).toBe(one.meanReductionDb);
    expect(three.suppressedFraction).toBe(one.suppressedFraction);
  });

  it('routes each channel to its own output plane', () => {
    const { left } = pair();
    const first = masteringRepairDereverbClassicalLinked({
      channels: [left, silence(), silence()],
      sampleRate: SR,
    });
    const second = masteringRepairDereverbClassicalLinked({
      channels: [silence(), left, silence()],
      sampleRate: SR,
    });

    expect(peak(first.channels[0])).toBeGreaterThan(0.01);
    expect(firstDifference(first.channels[0], second.channels[1])).toBe(-1);
    // Length-checked before the emptiness assertions below, which a zero-length
    // plane would satisfy by having nothing to look at.
    expect(first.channels.map((plane) => plane.length)).toEqual([LENGTH, LENGTH, LENGTH]);
    expect(second.channels.map((plane) => plane.length)).toEqual([LENGTH, LENGTH, LENGTH]);
    expect(first.channels[1].every((value) => value === 0)).toBe(true);
    expect(first.channels[2].every((value) => value === 0)).toBe(true);
    expect(second.channels[0].every((value) => value === 0)).toBe(true);
    expect(second.channels[2].every((value) => value === 0)).toBe(true);
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left } = pair();
    const withNan = Float32Array.from(left);
    withNan[1000] = Number.NaN;

    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [], sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalLinked({
        channels: [left, left.slice(0, 4096)],
        sampleRate: SR,
      }),
    ).toThrow(/same length/);
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [left, left, withNan], sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [left], sampleRate: Number.NaN }),
    ).toThrow(/sampleRate must be an integer/);
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [left], sampleRate: SR, nFft: 1000 }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [left], sampleRate: SR, hopLength: 0 }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalLinked({
        channels: [left, [1, 2, 3] as unknown as Float32Array],
        sampleRate: SR,
      }),
    ).toThrow(/Float32Array/);
  });
});

describe('the two linked entries on an input shorter than nFft', () => {
  it('splits: denoise refuses one, dereverb pads one', () => {
    // Identical call shape, opposite behaviour. Wiring either entry to the
    // other's core call is what this separates.
    const short = 512;
    const channels = [
      add(tone(700, 0.3, short), noise(99, 0.02, short)),
      add(tone(1900, 0.3, short), noise(98, 0.02, short)),
    ];

    expect(() => masteringRepairDenoiseClassicalLinked({ channels, sampleRate: SR })).toThrow(
      /n_fft/,
    );

    // The rejection is about nFft rather than about a short buffer: the same
    // input goes through once the window fits inside it.
    const fitted = masteringRepairDenoiseClassicalLinked({ channels, sampleRate: SR, nFft: 256 });
    expect(fitted.channels[0]).toHaveLength(short);
    expect(peak(fitted.channels[0])).toBeGreaterThan(0.01);

    const padded = masteringRepairDereverbClassicalLinked({ channels, sampleRate: SR });
    expect(padded.channels).toHaveLength(2);
    expect(padded.channels[0]).toHaveLength(short);
    expect(padded.channels[1]).toHaveLength(short);
    expect(peak(padded.channels[0])).toBeGreaterThan(0.01);
  });
});
