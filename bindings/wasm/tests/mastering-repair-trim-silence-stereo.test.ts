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

// Above QUIET_AMP and below LOUD_AMP, so the peak scan starts at LOUD_BEGIN
// with it and at QUIET_BEGIN without it.
const MID_THRESHOLD = 0.05;

// 20 ms: the default 400 ms puts a 4410-sample radius on a 22050-sample buffer,
// which reaches from either silent stretch into the signal and leaves almost
// nothing to trim.
const SHORT_WINDOW_MS = 20;

function fillBlock(out: Float32Array, begin: number, end: number, amp: number): void {
  for (let i = begin; i < end; i++) out[i] = i % 2 === 0 ? amp : -amp;
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

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);

    // Exact, not merely shorter: the peak scan keeps [QUIET_BEGIN, LOUD_END),
    // 12000 of the 22050 samples in. Sizing the output from the INPUT length
    // would read 10050 samples past the end of the pair.
    expect(result.left.length).toBe(LOUD_END - QUIET_BEGIN);
    expect(result.right.length).toBe(result.left.length);
    expect(result.left.length).toBeLessThan(left.length);

    expect(result.report.range.first).toBe(QUIET_BEGIN);
    expect(result.report.range.lastExclusive).toBe(LOUD_END);
    expect(result.report.removedHeadSamples).toBe(QUIET_BEGIN);
    expect(result.report.removedTailSamples).toBe(FRAMES - LOUD_END);

    // The kept samples are the input's, unshifted.
    expect(result.left[0]).toBeCloseTo(left[QUIET_BEGIN] as number, 6);
    expect(result.left[result.left.length - 1]).toBeCloseTo(left[LOUD_END - 1] as number, 6);
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

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(0);
    expect(result.right.length).toBe(0);

    // With nothing kept the range is (length, length), so the whole buffer
    // counts as removed HEAD and the tail is 0. Arbitrary as a split; the sum
    // is still the input length, which is what a caller reporting how much
    // went reads.
    expect(result.report.range.first).toBe(FRAMES);
    expect(result.report.range.lastExclusive).toBe(FRAMES);
    expect(result.report.removedHeadSamples).toBe(FRAMES);
    expect(result.report.removedTailSamples).toBe(0);
    expect(result.report.removedHeadSamples + result.report.removedTailSamples).toBe(FRAMES);
  });

  it('applies the UNION of the two per-channel scans and reports all three ranges', () => {
    // Disjoint bursts: neither channel alone spans what the pair keeps, so a
    // union is the only range that can produce this output.
    const left = burstChannel(1000, 3000);
    const right = burstChannel(9000, 11000);
    const result = masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR });

    expect(result.leftRange.first).toBe(1000);
    expect(result.leftRange.lastExclusive).toBe(3000);
    expect(result.rightRange.first).toBe(9000);
    expect(result.rightRange.lastExclusive).toBe(11000);

    // One report, two ranges -- neither the leftReport/rightReport pair of the
    // first four repair stereo entries nor the lone report of the denoise and
    // dereverb pair.
    expect('leftReport' in result).toBe(false);
    expect('rightReport' in result).toBe(false);

    expect(result.report.range.first).toBe(Math.min(result.leftRange.first, result.rightRange.first));
    expect(result.report.range.lastExclusive).toBe(
      Math.max(result.leftRange.lastExclusive, result.rightRange.lastExclusive),
    );
    expect(result.report.range.first).toBe(1000);
    expect(result.report.range.lastExclusive).toBe(11000);
    expect(result.left.length).toBe(10000);
    expect(result.right.length).toBe(10000);

    // The union reached past each channel's own signal, so each output carries
    // a stretch of the other channel's silence -- the point of unioning.
    expect(result.left[result.left.length - 1]).toBe(0);
    expect(result.right[0]).toBe(0);
  });

  it('lets a silent channel contribute nothing: its range is empty and the union is the other', () => {
    const left = burstChannel(5000, 7000);
    const right = new Float32Array(FRAMES);
    const result = masteringRepairTrimSilenceStereo({ left, right, sampleRate: SR });

    // An empty range is first >= lastExclusive, and a whole-buffer silence
    // reports it as (length, length).
    expect(result.rightRange.first).toBe(FRAMES);
    expect(result.rightRange.lastExclusive).toBe(FRAMES);
    expect(result.rightRange.first).toBeGreaterThanOrEqual(result.rightRange.lastExclusive);

    // Not the min/max of the two ranges: taking those would give
    // (5000, 22050) and keep 17050 samples of nothing.
    expect(result.report.range.first).toBe(result.leftRange.first);
    expect(result.report.range.lastExclusive).toBe(result.leftRange.lastExclusive);
    expect(result.left.length).toBe(2000);
  });

  it('reads threshold only in peak mode', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const base = { left, right, sampleRate: SR };

    // Peak: the knob is live. 12000 at the default 0.001 against 6000 at 0.05
    // -- a factor of two, not a boundary sample or two.
    const loose = masteringRepairTrimSilenceStereo({ ...base, mode: 'peak' as const });
    const tight = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'peak' as const,
      threshold: MID_THRESHOLD,
    });
    expect(loose.left.length).toBe(LOUD_END - QUIET_BEGIN); // 12000
    expect(tight.left.length).toBe(LOUD_END - LOUD_BEGIN); // 6000
    expect(tight.report.range.first).toBe(LOUD_BEGIN);

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
      threshold: MID_THRESHOLD,
    });
    expect(gatedTight.report.range.first).toBe(gatedLoose.report.range.first);
    expect(gatedTight.report.range.lastExclusive).toBe(gatedLoose.report.range.lastExclusive);
    expect(gatedTight.left).toEqual(gatedLoose.left);

    // The control for that equality: gated mode did trim, so the two agreeing
    // is threshold being unread rather than every option being unread.
    // Measured 12423 of 22050 kept, range (1797, 14220).
    expect(gatedLoose.left.length).toBeGreaterThan(0);
    expect(gatedLoose.left.length).toBeLessThan(FRAMES);
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
    expect(open.report.range.first).toBeLessThan(QUIET_END);
    expect(closed.report.range.first).toBeGreaterThan(QUIET_END);
    expect(closed.left.length).toBeLessThan(open.left.length);

    // Peak: both fields inert, to the exact sample.
    const peakOpen = masteringRepairTrimSilenceStereo({ ...base, mode: 'peak' as const });
    const peakClosed = masteringRepairTrimSilenceStereo({
      ...base,
      mode: 'peak' as const,
      gateLufs: -20,
      windowMs: 5,
    });
    expect(peakClosed.report.range.first).toBe(peakOpen.report.range.first);
    expect(peakClosed.report.range.lastExclusive).toBe(peakOpen.report.range.lastExclusive);
    expect(peakClosed.left).toEqual(peakOpen.left);
  });

  it('refuses a negative paddingSamples rather than absorbing it', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    const base = { left, right, sampleRate: SR, threshold: MID_THRESHOLD };

    // The core field is unsigned, so -1 crossing the boundary unchanged lands
    // at SIZE_MAX -- past the validator's SIZE_MAX/2 bound, not below zero.
    // Substituting 0 or the default instead would make a caller's mistake
    // indistinguishable from a deliberate no-padding call.
    // Matched by message, not merely by throwing: an absent export throws too,
    // and a bare `toThrow()` would be satisfied by that.
    expect(() => masteringRepairTrimSilenceStereo({ ...base, paddingSamples: -1 })).toThrow(
      /paddingSamples must be non-negative/,
    );
    expect(() => masteringRepairTrimSilenceStereo({ ...base, paddingSamples: -100000 })).toThrow(
      /paddingSamples must be non-negative/,
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
    expect(none.report.range.first).toBe(LOUD_BEGIN);
    expect(padded.report.range.first).toBe(LOUD_BEGIN - 500);
    expect(padded.report.range.lastExclusive).toBe(LOUD_END + 500);
    expect(padded.left.length).toBe(none.left.length + 1000);
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
    expect(result.report.range.first).toBe(0);
    expect(result.report.range.lastExclusive).toBe(FRAMES);
    expect(result.left.length).toBe(FRAMES);
    expect(result.report.removedHeadSamples).toBe(0);
    expect(result.report.removedTailSamples).toBe(0);
  });

  it('does not pad a pass that kept nothing', () => {
    const silence = new Float32Array(FRAMES);
    const result = masteringRepairTrimSilenceStereo({
      left: silence,
      right: silence.slice(),
      sampleRate: SR,
      paddingSamples: 1000,
    });
    expect(result.left.length).toBe(0);
    expect(result.right.length).toBe(0);
  });

  it('accepts the positional call form identically to the request form', () => {
    const left = burstChannel(1000, 3000);
    const right = burstChannel(9000, 11000);

    const positional = masteringRepairTrimSilenceStereo(left, right, SR, {
      threshold: MID_THRESHOLD,
      paddingSamples: 200,
    });
    const request = masteringRepairTrimSilenceStereo({
      left,
      right,
      sampleRate: SR,
      threshold: MID_THRESHOLD,
      paddingSamples: 200,
    });

    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
    expect(positional.report).toEqual(request.report);
    expect(positional.leftRange).toEqual(request.leftRange);
    expect(positional.rightRange).toEqual(request.rightRange);

    // The equality above is worthless if both forms trimmed nothing.
    expect(request.left.length).toBeLessThan(FRAMES);
    expect(request.leftRange.first).not.toBe(request.rightRange.first);
  });

  it('rejects an unknown mode name', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right, SR, {
        mode: 'gate' as unknown as 'peak',
      }),
    ).toThrow(/unknown trim silence mode/);
  });

  it('rejects mismatched channel lengths', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right.slice(0, right.length - 1), SR),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairTrimSilenceStereo(new Float32Array(0), new Float32Array(0), SR),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();

    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() => masteringRepairTrimSilenceStereo(badLeft, right, SR)).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() => masteringRepairTrimSilenceStereo(left, badRight, SR)).toThrow();
  });

  it('rejects a NaN and a wrong-typed sample rate', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() => masteringRepairTrimSilenceStereo(left, right, Number.NaN)).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right, 'not-a-number' as unknown as number),
    ).toThrow();
  });

  it('rejects a non-finite threshold and a non-positive windowMs', () => {
    const left = twoLevelChannel();
    const right = twoLevelChannel();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right, SR, { threshold: Number.NaN }),
    ).toThrow();
    expect(() => masteringRepairTrimSilenceStereo(left, right, SR, { threshold: -1 })).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right, SR, {
        mode: 'lufsGated',
        windowMs: 0,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairTrimSilenceStereo(left, right, SR, {
        mode: 'lufsGated',
        gateLufs: Number.NaN,
      }),
    ).toThrow();
  });
});
