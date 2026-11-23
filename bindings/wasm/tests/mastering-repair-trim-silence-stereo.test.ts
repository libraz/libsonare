/**
 * Tests for the stereo silence trimmer WASM wrapper
 * (`masteringRepairTrimSilenceStereo`).
 *
 * The one repair stereo entry whose output is SHORTER than its input, and the
 * only one that can succeed with nothing in it at all. It also carries a third
 * report shape: one `report` for the union that was applied plus the two
 * per-channel scans it was formed from.
 *
 * The fixtures alternate sign at every sample rather than carrying a tone, so
 * `|x|` is exactly the block amplitude at every index and an RMS over any
 * window inside a block is exactly that amplitude too. Every boundary below is
 * therefore an exact sample index rather than the first index at which a sine
 * happened to clear the threshold.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairTrimSilenceStereo } from '../src/index';

const SR = 22050;
const FRAMES = SR; // 1 s

// Two amplitudes a decade and a half apart, so one peak threshold can sit
// between them and neither is near the other.
const QUIET_AMP = 0.005; // -46.02 dBFS
const LOUD_AMP = 0.5; // -6.02 dBFS

const QUIET_BEGIN = 2000;
const QUIET_END = 4000;
const LOUD_BEGIN = 8000;
const LOUD_END = 14000;

// Above QUIET_AMP (-46 dB) and below LOUD_AMP (-6 dB), so the peak scan starts at LOUD_BEGIN
// with it and at QUIET_BEGIN without it.
const MID_THRESHOLD_DB = -26;

// 20 ms: the default 400 ms puts a 4410-sample radius on a 22050-sample buffer,
// which reaches from either silent stretch into the signal and leaves almost
// nothing to trim.
const SHORT_WINDOW_MS = 20;

function fillBlock(out: Float32Array, begin: number, end: number, amp: number): void {
  for (let i = begin; i < end; i++) {
    out[i] = i % 2 === 0 ? amp : -amp;
  }
}

/** A quiet block, then a loud one, with silence around and between them. */
function twoLevelChannel(): Float32Array {
  const out = new Float32Array(FRAMES);
  fillBlock(out, QUIET_BEGIN, QUIET_END, QUIET_AMP);
  fillBlock(out, LOUD_BEGIN, LOUD_END, LOUD_AMP);
  return out;
}

/** One loud block at [begin, end), silence elsewhere. */
function burstChannel(begin: number, end: number): Float32Array {
  const out = new Float32Array(FRAMES);
  fillBlock(out, begin, end, LOUD_AMP);
  return out;
}

describe('masteringRepairTrimSilenceStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns a SHORTER pair than it was given, both channels the same length', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const result = masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR });

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);

    // Exact, not merely shorter: the peak scan keeps [QUIET_BEGIN, LOUD_END),
    // 12000 of the 22050 samples in. Sizing the output from the INPUT length
    // would read 10050 samples past the end of the pair.
    expect(result.channels[0].length).toBe(LOUD_END - QUIET_BEGIN);
    expect(result.channels[1].length).toBe(result.channels[0].length);
    expect(result.channels[0].length).toBeLessThan(left.length);

    expect(result.reports[0].range.first).toBe(QUIET_BEGIN);
    expect(result.reports[0].range.lastExclusive).toBe(LOUD_END);
    expect(result.reports[0].removedHeadSamples).toBe(QUIET_BEGIN);
    expect(result.reports[0].removedTailSamples).toBe(FRAMES - LOUD_END);

    // The kept samples are the input's, unshifted.
    expect(result.channels[0][0]).toBeCloseTo(left[QUIET_BEGIN] as number, 6);
    expect(result.channels[0][result.channels[0].length - 1]).toBeCloseTo(
      left[LOUD_END - 1] as number,
      6,
    );
  });

  it('succeeds with two EMPTY arrays when neither channel carries signal', () => {
    const silence = new Float32Array(FRAMES);

    // Success, not an error and not a null result -- the entry hands back a
    // NULL pointer pair with length 0 underneath, which the copy must not read.
    const result = masteringRepairTrimSilenceStereo({
      left: silence,
      right: silence.slice(),
      sampleRate: SR,
    });

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);
    expect(result.channels[0].length).toBe(0);
    expect(result.channels[1].length).toBe(0);

    // With nothing kept the range is (length, length), so the whole buffer
    // counts as removed HEAD and the tail is 0. Arbitrary as a split; the sum
    // is still the input length, which is what a caller reporting how much
    // went reads.
    expect(result.reports[0].range.first).toBe(FRAMES);
    expect(result.reports[0].range.lastExclusive).toBe(FRAMES);
    expect(result.reports[0].removedHeadSamples).toBe(FRAMES);
    expect(result.reports[0].removedTailSamples).toBe(0);
    expect(result.reports[0].removedHeadSamples + result.reports[0].removedTailSamples).toBe(
      FRAMES,
    );
  });

  it('applies the UNION of the two per-channel scans and reports all three ranges', () => {
    // Disjoint bursts: neither channel alone spans what the pair keeps, so a
    // union is the only range that can produce this output.
    const left = burstChannel(1000, 3000);
    const right = burstChannel(9000, 11000);
    const result = masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR });

    expect(result.ranges[0].first).toBe(1000);
    expect(result.ranges[0].lastExclusive).toBe(3000);
    expect(result.ranges[1].first).toBe(9000);
    expect(result.ranges[1].lastExclusive).toBe(11000);

    // One report, two ranges -- neither the leftReport/rightReport pair of the
    // first four repair stereo entries nor the lone report of the denoise and
    // dereverb pair.
    expect('leftReport' in result).toBe(false);
    expect('rightReport' in result).toBe(false);

    expect(result.reports[0].range.first).toBe(
      Math.min(result.ranges[0].first, result.ranges[1].first),
    );
    expect(result.reports[0].range.lastExclusive).toBe(
      Math.max(result.ranges[0].lastExclusive, result.ranges[1].lastExclusive),
    );
    expect(result.reports[0].range.first).toBe(1000);
    expect(result.reports[0].range.lastExclusive).toBe(11000);
    expect(result.channels[0].length).toBe(10000);
    expect(result.channels[1].length).toBe(10000);

    // The union reached past each channel's own signal, so each output carries
    // a stretch of the other channel's silence -- the point of unioning.
    expect(result.channels[0][result.channels[0].length - 1]).toBe(0);
    expect(result.channels[1][0]).toBe(0);
  });

  it('lets a silent channel contribute nothing: its range is empty and the union is the other', () => {
    const left = burstChannel(5000, 7000);
    const right = new Float32Array(FRAMES);
    const result = masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR });

    // An empty range is first >= lastExclusive, and a whole-buffer silence
    // reports it as (length, length).
    expect(result.ranges[1].first).toBe(FRAMES);
    expect(result.ranges[1].lastExclusive).toBe(FRAMES);
    expect(result.ranges[1].first).toBeGreaterThanOrEqual(result.ranges[1].lastExclusive);

    // Not the min/max of the two ranges: taking those would give
    // (5000, 22050) and keep 17050 samples of nothing.
    expect(result.reports[0].range.first).toBe(result.ranges[0].first);
    expect(result.reports[0].range.lastExclusive).toBe(result.ranges[0].lastExclusive);
    expect(result.channels[0].length).toBe(2000);
  });

  it('reads thresholdDb only in peak mode', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const base = { left, right, sampleRate: SR };

    // Peak: the knob is live. 12000 at the default -60 dB against 6000 at -26 dB
    // -- a factor of two, not a boundary sample or two.
    const loose = masteringRepairTrimSilenceStereo({ ...base, mode: 'peak' as const });
    const tight = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'peak' as const,
      thresholdDb: MID_THRESHOLD_DB,
    });
    expect(loose.channels[0].length).toBe(LOUD_END - QUIET_BEGIN); // 12000
    expect(tight.channels[0].length).toBe(LOUD_END - LOUD_BEGIN); // 6000
    expect(tight.reports[0].range.first).toBe(LOUD_BEGIN);

    // LUFS-gated: the same knob is inert. Exact equality, because the field is
    // never read rather than read and rounded away.
    const gatedLoose = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'lufsGated' as const,
      windowMs: SHORT_WINDOW_MS,
    });
    const gatedTight = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'lufsGated' as const,
      windowMs: SHORT_WINDOW_MS,
      thresholdDb: MID_THRESHOLD_DB,
    });
    expect(gatedTight.reports[0].range.first).toBe(gatedLoose.reports[0].range.first);
    expect(gatedTight.reports[0].range.lastExclusive).toBe(
      gatedLoose.reports[0].range.lastExclusive,
    );
    expect(gatedTight.channels[0]).toEqual(gatedLoose.channels[0]);

    // The control for that equality: gated mode did trim, so the two agreeing
    // is threshold being unread rather than every option being unread.
    // Measured 12423 of 22050 kept, range (1797, 14220).
    expect(gatedLoose.channels[0].length).toBeGreaterThan(0);
    expect(gatedLoose.channels[0].length).toBeLessThan(FRAMES);
  });

  it('reads gateLufs and windowMs only in lufs-gated mode', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const base = { left, right, sampleRate: SR, windowMs: SHORT_WINDOW_MS };

    // Gated: -60 admits the quiet block (-46 dBFS), -20 does not, so the kept
    // range collapses onto the loud one. QUIET_END is what separates the two,
    // and neither lands near it: measured range.first 1797 at -60 against 7797
    // at -20, a 2203- and a 3797-sample margin either side of 4000. Lengths
    // 12423 against 6406.
    const open = masteringRepairTrimSilenceStereo({ ...base, mode: 'lufsGated' as const });
    const closed = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'lufsGated' as const,
      gateLufs: -20,
    });
    expect(open.reports[0].range.first).toBeLessThan(QUIET_END);
    expect(closed.reports[0].range.first).toBeGreaterThan(QUIET_END);
    expect(closed.channels[0].length).toBeLessThan(open.channels[0].length);

    // Peak: both fields inert, to the exact sample.
    const peakOpen = masteringRepairTrimSilenceStereo({ ...base, mode: 'peak' as const });
    const peakClosed = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'peak' as const,
      gateLufs: -20,
      windowMs: 5,
    });
    expect(peakClosed.reports[0].range.first).toBe(peakOpen.reports[0].range.first);
    expect(peakClosed.reports[0].range.lastExclusive).toBe(peakOpen.reports[0].range.lastExclusive);
    expect(peakClosed.channels[0]).toEqual(peakOpen.channels[0]);
  });

  it('refuses a negative paddingSamples rather than absorbing it', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const base = { left, right, sampleRate: SR, thresholdDb: MID_THRESHOLD_DB };

    // The core field is unsigned, so -1 crossing the boundary unchanged lands
    // at SIZE_MAX -- past the validator's padding bound, not below zero.
    // Substituting 0 or the default instead would make a caller's mistake
    // indistinguishable from a deliberate no-padding call.
    // Matched by message, not merely by throwing: an absent export throws too,
    // and a bare `toThrow()` would be satisfied by that.
    expect(() => masteringRepairTrimSilenceStereo({ ...base, paddingSamples: -1 })).toThrow(
      /paddingSamples must be an integer in \[0, 960000\]$/,
    );
    expect(() => masteringRepairTrimSilenceStereo({ ...base, paddingSamples: -100000 })).toThrow(
      /paddingSamples must be an integer in \[0, 960000\]$/,
    );

    // The other end: a count past the 32-bit range is refused before the
    // narrowing rather than saturating to INT_MAX and reading as a real one.
    expect(() => masteringRepairTrimSilenceStereo({ ...base, paddingSamples: 2 ** 31 })).toThrow(
      /paddingSamples/,
    );

    // The controls: 0 is accepted and is the default, and a positive count
    // genuinely widens -- so the refusals above are of the value rather than of
    // the option.
    const none = masteringRepairTrimSilenceStereo({ ...base, paddingSamples: 0 });
    const padded = masteringRepairTrimSilenceStereo({ ...base, paddingSamples: 500 });
    expect(none.reports[0].range.first).toBe(LOUD_BEGIN);
    expect(padded.reports[0].range.first).toBe(LOUD_BEGIN - 500);
    expect(padded.reports[0].range.lastExclusive).toBe(LOUD_END + 500);
    expect(padded.channels[0].length).toBe(none.channels[0].length + 1000);
  });

  it('clamps padding to the buffer instead of reaching past either end', () => {
    const left = burstChannel(10, FRAMES - 10);
    const right = left.slice();
    const result = masteringRepairTrimSilenceStereo({
      left,
      right,
      sampleRate: SR,
      paddingSamples: FRAMES,
    });
    expect(result.reports[0].range.first).toBe(0);
    expect(result.reports[0].range.lastExclusive).toBe(FRAMES);
    expect(result.channels[0].length).toBe(FRAMES);
    expect(result.reports[0].removedHeadSamples).toBe(0);
    expect(result.reports[0].removedTailSamples).toBe(0);
  });

  it('does not pad a pass that kept nothing', () => {
    const silence = new Float32Array(FRAMES);
    const result = masteringRepairTrimSilenceStereo({
      left: silence,
      right: silence.slice(),
      sampleRate: SR,
      paddingSamples: 1000,
    });
    expect(result.channels[0].length).toBe(0);
    expect(result.channels[1].length).toBe(0);
  });

  it('refuses the positional call form', () => {
    const left = burstChannel(1000, 3000);
    const right = burstChannel(9000, 11000);
    const call = masteringRepairTrimSilenceStereo as unknown as (...args: unknown[]) => unknown;
    expect(() => call(left, right, SR)).toThrow(TypeError);
  });

  it('rejects an unknown mode name', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left,
        right,
        sampleRate: SR,
        mode: 'gate' as unknown as 'peak',
      }),
    ).toThrow(/unknown trim silence mode/);
  });

  it('rejects mismatched channel lengths', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left,
        right: right.slice(0, right.length - 1),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();

    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() =>
      masteringRepairTrimSilenceStereo({ left: badLeft, right, sampleRate: SR }),
    ).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() =>
      masteringRepairTrimSilenceStereo({ left, right: badRight, sampleRate: SR }),
    ).toThrow();
  });

  it('rejects a NaN and a wrong-typed sample rate', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo({ left, right, sampleRate: Number.NaN }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left,
        right,
        sampleRate: 'not-a-number' as unknown as number,
      }),
    ).toThrow();
  });

  it('rejects a non-finite or positive thresholdDb and a non-positive windowMs', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR, thresholdDb: Number.NaN }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR, thresholdDb: 1 }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left,
        right,
        sampleRate: SR,
        mode: 'lufsGated',
        windowMs: 0,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo({
        left,
        right,
        sampleRate: SR,
        mode: 'lufsGated',
        gateLufs: Number.NaN,
      }),
    ).toThrow();
  });
});
