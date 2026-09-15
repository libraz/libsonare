/**
 * Tests for the stereo declick WASM wrapper (`masteringRepairDeclickStereo`).
 *
 * The property that separates this entry point from calling the mono
 * `masteringRepairDeclick` on each channel independently is the linked
 * selection: a run either channel's own detection selects is repaired in
 * BOTH channels. The fixture plants one click per channel at disjoint
 * positions, with different tones and independent noise so neither channel is
 * the other scaled or copied -- an implementation that declicks one channel
 * and returns it twice would pass every assertion that only checks "the
 * clicks are gone".
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDeclick, masteringRepairDeclickStereo } from '../src/index';

const SR = 48000;
const FRAMES = SR;
const BED_AMP = 0.15;
const NOISE_AMP = 0.01;
const CLICK_AMP = 0.6;
const CLICK_WIDTH = 2;
const LEFT_CLICK_POS = 5053;
const RIGHT_CLICK_POS = 30462;

// threshold/neighborRatio proven, by the C++ corpus fixture in
// tests/mastering/repair_stereo_impulse_test.cpp, to catch a click as small
// as 3.4x the bed amplitude -- comfortably below the 4x ratio planted here.
const DECLICK_CONFIG = {
  threshold: 0.35,
  neighborRatio: 2.0,
  maxClickSamples: 8,
  lpcOrder: 20,
  residualRatio: 8.0,
};

function sine(freq: number, frames: number, amp: number, phase: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR + phase);
  }
  return out;
}

function withNoise(samples: Float32Array, amp: number, seed: number): Float32Array {
  let state = seed >>> 0;
  const out = new Float32Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const u = (state >>> 8) / (1 << 24);
    out[i] = (samples[i] ?? 0) + (u - 0.5) * amp;
  }
  return out;
}

function plantClick(samples: Float32Array, position: number, amplitude: number): void {
  for (let i = 0; i < CLICK_WIDTH; i++) {
    samples[position + i] = (samples[position + i] ?? 0) + amplitude;
  }
}

// Different tones AND independent noise: left and right are neither identical
// nor a scaled copy of one another. Each carries exactly one planted click,
// at a position the other channel does not share.
function buildChannels(): { left: Float32Array; right: Float32Array } {
  const left = withNoise(sine(440, FRAMES, BED_AMP, 0.0), NOISE_AMP, 1);
  const right = withNoise(sine(660, FRAMES, BED_AMP, 0.35), NOISE_AMP, 2);
  plantClick(left, LEFT_CLICK_POS, CLICK_AMP);
  plantClick(right, RIGHT_CLICK_POS, -CLICK_AMP);
  return { left, right };
}

describe('masteringRepairDeclickStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('repairs a run selected by only one channel in both channels, and reports the link', () => {
    const { left, right } = buildChannels();
    const result = masteringRepairDeclickStereo({
      left,
      right,
      sampleRate: SR,
      ...DECLICK_CONFIG,
    });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(result.right.length).toBe(right.length);
    // Not the same data: an implementation that declicks one channel and
    // returns it for both would make these equal.
    expect(result.left).not.toEqual(result.right);

    // Each channel's own detection sees only its own planted click.
    expect(result.leftReport.detected.count).toBe(1);
    expect(result.rightReport.detected.count).toBe(1);

    // The union of both channels' selections is repaired in BOTH -- a
    // per-channel declick would stop at detected.count and never set
    // linkedRuns.
    expect(result.leftReport.repairedRuns).toBeGreaterThan(result.leftReport.detected.count);
    expect(result.rightReport.repairedRuns).toBeGreaterThan(result.rightReport.detected.count);
    expect(result.leftReport.linkedRuns).toBeGreaterThan(0);
    expect(result.rightReport.linkedRuns).toBeGreaterThan(0);
  });

  it('differs from the mono declick on the left channel alone, at the right channel click position', () => {
    const { left, right } = buildChannels();
    const stereo = masteringRepairDeclickStereo({
      left,
      right,
      sampleRate: SR,
      ...DECLICK_CONFIG,
    });
    const mono = masteringRepairDeclick(left, SR, DECLICK_CONFIG);

    // The mono pass never saw the right channel's click, so it leaves this
    // region of the left channel untouched; the stereo pass repairs it
    // because the right channel's own detection selected it. A stereo entry
    // point that amounted to two independent mono passes would make these
    // equal.
    let maxDiff = 0;
    for (let i = 0; i < CLICK_WIDTH; i++) {
      const stereoValue = stereo.left[RIGHT_CLICK_POS + i] ?? 0;
      const monoValue = mono[RIGHT_CLICK_POS + i] ?? 0;
      maxDiff = Math.max(maxDiff, Math.abs(stereoValue - monoValue));
    }
    expect(maxDiff).toBeGreaterThan(1e-4);
  });

  it('accepts the positional call form identically to the request form', () => {
    const { left, right } = buildChannels();
    const positional = masteringRepairDeclickStereo(left, right, SR, DECLICK_CONFIG);
    const request = masteringRepairDeclickStereo({
      left,
      right,
      sampleRate: SR,
      ...DECLICK_CONFIG,
    });
    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildChannels();
    expect(() =>
      masteringRepairDeclickStereo(left, right.slice(0, right.length - 1), SR, DECLICK_CONFIG),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDeclickStereo(new Float32Array(0), new Float32Array(0), SR, DECLICK_CONFIG),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() => masteringRepairDeclickStereo(badLeft, right, SR, DECLICK_CONFIG)).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() => masteringRepairDeclickStereo(left, badRight, SR, DECLICK_CONFIG)).toThrow();
  });

  it('rejects a non-positive maxClickSamples', () => {
    const { left, right } = buildChannels();
    expect(() =>
      masteringRepairDeclickStereo(left, right, SR, { ...DECLICK_CONFIG, maxClickSamples: 0 }),
    ).toThrow();
  });
});
