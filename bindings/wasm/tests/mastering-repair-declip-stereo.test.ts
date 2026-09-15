/**
 * Tests for the stereo declip WASM wrapper (`masteringRepairDeclipStereo`).
 *
 * Declip's linking rule is not the declicker's: a channel with no clipped
 * sample in a union run is left untouched there, so a plateau clipped in
 * only one channel produces no linking at all. Linking needs both channels
 * clipped in the SAME region with DIFFERENT extents -- the fixture plants
 * overlapping plateaus of unequal width so the narrower channel's own clip
 * is a strict subset of the wider one, which is what makes it reach past its
 * own clipped samples.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDeclip, masteringRepairDeclipStereo } from '../src/index';

const SR = 48000;
const FRAMES = SR;
const BED_AMP = 0.3;
const CLIP_VALUE = 0.95;
const START = 5000;
const SHORT_LEN = 6; // left's own clipped extent
const LONG_LEN = 20; // right's own clipped extent -- the union run's width

// clipThreshold sits strictly between BED_AMP and CLIP_VALUE so the bed never
// registers as clipped and the plateau always does.
const DECLIP_CONFIG = { clipThreshold: 0.9 };

function sine(freq: number, frames: number, amp: number, phase: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR + phase);
  }
  return out;
}

function plantPlateau(samples: Float32Array, start: number, length: number, value: number): void {
  for (let i = 0; i < length; i++) {
    samples[start + i] = value;
  }
}

// Different tones so left and right are neither identical nor a scaled copy
// of one another, and both channels carry a plateau starting at the same
// position -- left's is a strict prefix of right's, so their union is
// right's own extent.
function buildChannels(): { left: Float32Array; right: Float32Array } {
  const left = sine(440, FRAMES, BED_AMP, 0.0);
  const right = sine(660, FRAMES, BED_AMP, 0.35);
  plantPlateau(left, START, SHORT_LEN, CLIP_VALUE);
  plantPlateau(right, START, LONG_LEN, CLIP_VALUE);
  return { left, right };
}

describe('masteringRepairDeclipStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reconstructs the narrower channel past its own clipped extent, and reports the link', () => {
    const { left, right } = buildChannels();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...DECLIP_CONFIG });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(result.right.length).toBe(right.length);
    // Not the same data: an implementation that declipped one channel and
    // returned it for both would make these equal.
    expect(result.left).not.toEqual(result.right);

    // Each channel's own detection sees only its own plateau -- both
    // non-zero, so neither report is witnessing an empty run.
    expect(result.leftReport.detected.runCount).toBe(1);
    expect(result.leftReport.detected.sampleCount).toBe(SHORT_LEN);
    expect(result.rightReport.detected.runCount).toBe(1);
    expect(result.rightReport.detected.sampleCount).toBe(LONG_LEN);

    // The union run is right's own extent (LONG_LEN): left reconstructs the
    // whole of it, reaching past its own SHORT_LEN clip, so only left links.
    expect(result.leftReport.repairedSamples).toBe(LONG_LEN);
    expect(result.rightReport.repairedSamples).toBe(LONG_LEN);
    expect(result.leftReport.linkedRuns).toBe(1);
    expect(result.rightReport.linkedRuns).toBe(0);
  });

  it('differs from the mono declip on the left channel alone, past its own clipped extent', () => {
    const { left, right } = buildChannels();
    const stereo = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...DECLIP_CONFIG });
    const mono = masteringRepairDeclip(left, SR, DECLIP_CONFIG);

    // The mono pass never saw the right channel's wider plateau, so it
    // leaves [START + SHORT_LEN, START + LONG_LEN) at the raw clip value;
    // the stereo pass reconstructs it because the union run -- driven by
    // the right channel -- extends that far. A stereo entry point that
    // amounted to two independent mono passes would make these equal.
    let maxDiff = 0;
    for (let i = SHORT_LEN; i < LONG_LEN; i++) {
      const stereoValue = stereo.left[START + i] ?? 0;
      const monoValue = mono[START + i] ?? 0;
      maxDiff = Math.max(maxDiff, Math.abs(stereoValue - monoValue));
    }
    expect(maxDiff).toBeGreaterThan(1e-4);
  });

  it('accepts the positional call form identically to the request form', () => {
    const { left, right } = buildChannels();
    const positional = masteringRepairDeclipStereo(left, right, SR, DECLIP_CONFIG);
    const request = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...DECLIP_CONFIG });
    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildChannels();
    expect(() =>
      masteringRepairDeclipStereo(left, right.slice(0, right.length - 1), SR, DECLIP_CONFIG),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDeclipStereo(new Float32Array(0), new Float32Array(0), SR, DECLIP_CONFIG),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() => masteringRepairDeclipStereo(badLeft, right, SR, DECLIP_CONFIG)).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() => masteringRepairDeclipStereo(left, badRight, SR, DECLIP_CONFIG)).toThrow();
  });

  it('rejects a clipThreshold outside (0, 1]', () => {
    const { left, right } = buildChannels();
    expect(() => masteringRepairDeclipStereo(left, right, SR, { clipThreshold: 0 })).toThrow();
    expect(() => masteringRepairDeclipStereo(left, right, SR, { clipThreshold: 1.5 })).toThrow();
  });
});
