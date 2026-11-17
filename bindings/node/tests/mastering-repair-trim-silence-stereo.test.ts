import { describe, expect, it } from 'vitest';
import {
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  type TrimSilenceOptions,
} from '../src/index.js';

const SR = 22050;
const LENGTH = 4410;

/**
 * A run of alternating +/-amp, so every sample in it has |x| exactly `amp`.
 *
 * A tone would put samples arbitrarily close to zero next to each edge, which
 * makes the peak scan's first/last indices depend on where the zero crossings
 * happened to land. With this the scan's answer is the run's own bounds, so the
 * range assertions below are observations of the union rule rather than of a
 * rounding direction.
 */
function run(begin: number, end: number, amp: number, into?: Float32Array): Float32Array {
  const out = into ?? new Float32Array(LENGTH);
  for (let i = begin; i < end; i += 1) {
    out[i] = i % 2 === 0 ? amp : -amp;
  }
  return out;
}

/** Left carries [1000, 2000), right [1500, 3000) -- each channel owns one edge. */
function offsetPair(): { left: Float32Array; right: Float32Array } {
  return { left: run(1000, 2000, 0.5), right: run(1500, 3000, 0.5) };
}

/**
 * A quiet floor at 0.01 under a loud body at 0.5, the same in both channels.
 *
 * The two levels are what makes each knob observable: 0.01 sits above the
 * default peak threshold of -60 dB and below -20 dB, and its -40 dBFS window RMS
 * sits above a -60 gate and below a -20 one. So one fixture answers both modes.
 */
function tieredPair(): { left: Float32Array; right: Float32Array } {
  const build = () => {
    const out = run(0, 1000, 0.01);
    return run(1000, 2000, 0.5, out);
  };
  return { left: build(), right: build() };
}

function trim(
  pair: { left: Float32Array; right: Float32Array },
  options: TrimSilenceOptions = {},
): ReturnType<typeof masteringRepairTrimSilenceStereo> {
  return masteringRepairTrimSilenceStereo({ ...pair, ...options, sampleRate: SR });
}

describe('masteringRepairTrimSilenceStereo', () => {
  it('cuts both channels to the union of their own ranges and reports all three', () => {
    const pair = offsetPair();
    const result = trim(pair);

    // Each channel decided one edge: left the head at 1000, right the tail at
    // 3000. A scan on a downmix, or on either channel alone, lands elsewhere.
    expect(result.ranges[0]).toEqual({ first: 1000, lastExclusive: 2000 });
    expect(result.ranges[1]).toEqual({ first: 1500, lastExclusive: 3000 });
    expect(result.reports[0].range).toEqual({ first: 1000, lastExclusive: 3000 });
    expect(result.reports[0].range.first).toBe(
      Math.min(result.ranges[0].first, result.ranges[1].first),
    );
    expect(result.reports[0].range.lastExclusive).toBe(
      Math.max(result.ranges[0].lastExclusive, result.ranges[1].lastExclusive),
    );

    // The OUTPUT length, which is the union's width and not the input's length.
    expect(result.channels[0]).toHaveLength(2000);
    expect(result.channels[1]).toHaveLength(2000);
    expect(result.channels[0].length).not.toBe(LENGTH);

    // The kept window is the input's, sample for sample, in both channels.
    expect(Array.from(result.channels[0])).toEqual(Array.from(pair.left.subarray(1000, 3000)));
    expect(Array.from(result.channels[1])).toEqual(Array.from(pair.right.subarray(1000, 3000)));
    // Independent regions, so a channel handed back as a copy of the other fails.
    expect(Array.from(result.channels[0])).not.toEqual(Array.from(result.channels[1]));

    expect(result.reports[0].removedHeadSamples).toBe(1000);
    expect(result.reports[0].removedTailSamples).toBe(LENGTH - 3000);
    expect(result.reports[0].removedHeadSamples + result.reports[0].removedTailSamples).toBe(
      LENGTH - result.channels[0].length,
    );
  });

  it('lets a silent channel contribute nothing, rather than widening the union to the buffer', () => {
    // A naive min/max over the two ranges would read the silent channel's
    // (LENGTH, LENGTH) as a tail edge and keep everything to the end.
    const pair = { left: run(1000, 2000, 0.5), right: new Float32Array(LENGTH) };
    const result = trim(pair);

    expect(result.ranges[1]).toEqual({ first: LENGTH, lastExclusive: LENGTH });
    expect(result.ranges[0]).toEqual({ first: 1000, lastExclusive: 2000 });
    expect(result.reports[0].range).toEqual(result.ranges[0]);
    expect(result.channels[0]).toHaveLength(1000);
    expect(result.channels[1]).toHaveLength(1000);
    // The silent channel is still cut to the shared range, not dropped.
    expect(Array.from(result.channels[1])).toEqual(new Array(1000).fill(0));
  });

  it('returns two empty arrays for a pair with no signal at all, as a success', () => {
    const pair = { left: new Float32Array(LENGTH), right: new Float32Array(LENGTH) };
    let result: ReturnType<typeof masteringRepairTrimSilenceStereo> | undefined;
    expect(() => {
      result = trim(pair);
    }).not.toThrow();
    if (result === undefined) {
      throw new Error('unreachable');
    }

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);
    expect(result.channels[0]).toHaveLength(0);
    expect(result.channels[1]).toHaveLength(0);

    // Nothing kept is reported as the range (LENGTH, LENGTH), which counts the
    // whole buffer as removed head and leaves removed tail at 0. The split
    // between the ends is arbitrary there; the total is not.
    expect(result.reports[0].range).toEqual({ first: LENGTH, lastExclusive: LENGTH });
    expect(result.ranges[0]).toEqual({ first: LENGTH, lastExclusive: LENGTH });
    expect(result.ranges[1]).toEqual({ first: LENGTH, lastExclusive: LENGTH });
    expect(result.reports[0].removedHeadSamples).toBe(LENGTH);
    expect(result.reports[0].removedTailSamples).toBe(0);
    expect(result.reports[0].removedHeadSamples + result.reports[0].removedTailSamples).toBe(
      LENGTH,
    );
  });

  it('reads thresholdDb only in peak mode', () => {
    const pair = tieredPair();
    // -60 dB admits the 0.01 floor, -20 dB does not, so the head edge moves from
    // the floor's start to the body's.
    const open = trim(pair, { mode: 'peak', thresholdDb: -60 });
    const closed = trim(pair, { mode: 'peak', thresholdDb: -20 });
    expect(open.reports[0].range).toEqual({ first: 0, lastExclusive: 2000 });
    expect(closed.reports[0].range).toEqual({ first: 1000, lastExclusive: 2000 });

    // The same two values in the gated mode, which never reads the field.
    const gatedOpen = trim(pair, { mode: 'lufsGated', windowMs: 10, thresholdDb: -60 });
    const gatedClosed = trim(pair, { mode: 'lufsGated', windowMs: 10, thresholdDb: -1 });
    expect(gatedClosed.reports[0].range).toEqual(gatedOpen.reports[0].range);
    expect(Array.from(gatedClosed.channels[0])).toEqual(Array.from(gatedOpen.channels[0]));
  });

  it('reads gateLufs and windowMs only in the gated mode', () => {
    const pair = tieredPair();
    // The floor's window RMS is 0.01 => -40 dBFS, so a -60 gate admits it and a
    // -20 gate does not. windowMs 10 gives a +/-110-sample window, short enough
    // that the two regions are judged separately: measured (0, 2110) at -60 and
    // (898, 2102) at -20. Both edges sit ~110 samples outside the body, which is
    // the window half-width bleeding across each boundary.
    const low = trim(pair, { mode: 'lufsGated', windowMs: 10, gateLufs: -60 });
    const high = trim(pair, { mode: 'lufsGated', windowMs: 10, gateLufs: -20 });
    expect(low.reports[0].range.first).toBe(0);
    expect(high.reports[0].range.first).toBeGreaterThan(0);
    // The body still had to survive the tighter gate, or this would be
    // comparing an empty result against a full one.
    expect(high.reports[0].range.first).toBeLessThan(1000);
    expect(high.reports[0].range.lastExclusive).toBeGreaterThan(1000);
    expect(high.channels[0].length).toBeLessThan(low.channels[0].length);

    // windowMs sizes that window and does nothing else, so it moves the gated
    // edges too: at +/-1102 samples the floor and the body land in one window
    // and the -20 gate admits from 0, measured (0, 3014) against (898, 2102).
    const wide = trim(pair, { mode: 'lufsGated', windowMs: 100, gateLufs: -20 });
    expect(wide.reports[0].range).not.toEqual(high.reports[0].range);
    expect(wide.reports[0].range.first).toBeLessThan(high.reports[0].range.first);

    // Neither field is read in peak mode: same two gates, same two windows.
    const peakLow = trim(pair, { mode: 'peak', gateLufs: -60, windowMs: 10 });
    const peakHigh = trim(pair, { mode: 'peak', gateLufs: -20, windowMs: 100 });
    expect(peakHigh.reports[0].range).toEqual(peakLow.reports[0].range);
    expect(peakLow.reports[0].range).toEqual({ first: 0, lastExclusive: 2000 });
  });

  it('still validates thresholdDb in the gated mode, where nothing reads it', () => {
    // "Read only by peak" is about the scan, not about validation: the config
    // validator checks every public field whichever mode is selected.
    const pair = tieredPair();
    expect(() => trim(pair, { mode: 'lufsGated', thresholdDb: 1 })).toThrow();
    expect(() => trim(pair, { mode: 'peak', gateLufs: Number.NaN })).toThrow();
    expect(() => trim(pair, { mode: 'peak', windowMs: 0 })).toThrow();
  });

  it('widens the kept range by paddingSamples, clamped to the buffer', () => {
    const pair = offsetPair();
    const bare = trim(pair);
    const padded = trim(pair, { paddingSamples: 256 });

    // Padding is applied per channel scan, so each per-channel range widens and
    // the union widens with them.
    expect(padded.ranges[0]).toEqual({ first: 1000 - 256, lastExclusive: 2000 + 256 });
    expect(padded.ranges[1]).toEqual({ first: 1500 - 256, lastExclusive: 3000 + 256 });
    expect(padded.reports[0].range).toEqual({ first: 744, lastExclusive: 3256 });
    expect(padded.channels[0]).toHaveLength(bare.channels[0].length + 512);

    // Clamped rather than wrapped: a padding wider than the buffer keeps the
    // whole of it and reports nothing removed.
    const flooded = trim(pair, { paddingSamples: LENGTH * 4 });
    expect(flooded.reports[0].range).toEqual({ first: 0, lastExclusive: LENGTH });
    expect(flooded.channels[0]).toHaveLength(LENGTH);
    expect(flooded.reports[0].removedHeadSamples).toBe(0);
    expect(flooded.reports[0].removedTailSamples).toBe(0);

    // A pass that kept nothing is not padded.
    const empty = trim(
      { left: new Float32Array(LENGTH), right: new Float32Array(LENGTH) },
      { paddingSamples: 256 },
    );
    expect(empty.channels[0]).toHaveLength(0);
  });

  it('refuses a negative paddingSamples by name instead of folding it to a default', () => {
    const pair = offsetPair();
    // The field is a size_t, so -1 would arrive as SIZE_MAX -- above the core's
    // own padding bound, where it reads as an out-of-range padding rather
    // than as the negative that was written. Either way it must not become 0.
    // Measured: RangeError "paddingSamples must be a finite non-negative
    // integer ...", so the field is named rather than the core's own bound.
    for (const paddingSamples of [-1, -0.5, -LENGTH]) {
      expect(() => trim(pair, { paddingSamples })).toThrow(/paddingSamples/);
    }
    // Not a rounding artefact of the refusal: 0 is accepted and is the default.
    expect(trim(pair, { paddingSamples: 0 }).reports[0].range).toEqual(trim(pair).reports[0].range);
    // A non-number is refused by name too, rather than silently defaulting.
    expect(() => trim(pair, { paddingSamples: '256' as unknown as number })).toThrow(
      /paddingSamples/,
    );
  });

  it('agrees with the mono trimmer on a pair whose channels are identical', () => {
    const pair = tieredPair();
    const stereo = trim(pair, { mode: 'peak', thresholdDb: -20 });
    const mono = masteringRepairTrimSilence(pair.left, SR, {
      mode: 'peak',
      thresholdDb: -20,
    }).samples;
    expect(Array.from(stereo.channels[0])).toEqual(Array.from(mono));
    expect(Array.from(stereo.channels[1])).toEqual(Array.from(mono));
  });

  it('refuses bad arguments with a catchable error', () => {
    const pair = offsetPair();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left: pair.left,
        right: pair.right.slice(0, 10),
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left: undefined as unknown as Float32Array,
        right: pair.right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left: pair.left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() => trim(pair, { mode: 'sometimes' as unknown as 'peak' })).toThrow();
    expect(() => masteringRepairTrimSilenceStereo({ ...pair, sampleRate: Number.NaN })).toThrow();
    expect(() => masteringRepairTrimSilenceStereo({ ...pair, sampleRate: 0 })).toThrow();
  });
});
