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

// clipThresholdDb (0.9 linear) sits strictly between BED_AMP and CLIP_VALUE so the bed never
// registers as clipped and the plateau always does.
const DECLIP_CONFIG = { clipThresholdDb: -0.9151 };

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

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);
    expect(result.channels[0].length).toBe(left.length);
    expect(result.channels[1].length).toBe(right.length);
    // Not the same data: an implementation that declipped one channel and
    // returned it for both would make these equal.
    expect(result.channels[0]).not.toEqual(result.channels[1]);

    // Each channel's own detection sees only its own plateau -- both
    // non-zero, so neither report is witnessing an empty run.
    expect(result.reports[0].detected.runCount).toBe(1);
    expect(result.reports[0].detected.clippedSamples).toBe(SHORT_LEN);
    expect(result.reports[1].detected.runCount).toBe(1);
    expect(result.reports[1].detected.clippedSamples).toBe(LONG_LEN);

    // The union run is right's own extent (LONG_LEN), and it holds clipped
    // samples of both channels, so each channel either reconstructs it or
    // leaves it clipped -- once. Only left reaches past its own clip, so only
    // left links.
    for (const report of result.reports) {
      expect(report.lpcReconstructedRuns + report.unreconstructedRuns).toBe(1);
    }
    expect(result.reports[0].linkedRuns).toBe(1);
    expect(result.reports[1].linkedRuns).toBe(0);
  });

  it('differs from the mono declip on the left channel alone, past its own clipped extent', () => {
    const { left, right } = buildChannels();
    const stereo = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...DECLIP_CONFIG });
    const mono = masteringRepairDeclip(left, SR, DECLIP_CONFIG).samples;

    // The mono pass never saw the right channel's wider plateau, so it
    // leaves [START + SHORT_LEN, START + LONG_LEN) at the raw clip value;
    // the stereo pass reconstructs it because the union run -- driven by
    // the right channel -- extends that far. A stereo entry point that
    // amounted to two independent mono passes would make these equal.
    let maxDiff = 0;
    for (let i = SHORT_LEN; i < LONG_LEN; i++) {
      const stereoValue = stereo.channels[0][START + i] ?? 0;
      const monoValue = mono[START + i] ?? 0;
      maxDiff = Math.max(maxDiff, Math.abs(stereoValue - monoValue));
    }
    expect(maxDiff).toBeGreaterThan(1e-4);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildChannels();
    expect(() =>
      masteringRepairDeclipStereo({
        left,
        right: right.slice(0, right.length - 1),
        sampleRate: SR,
        ...DECLIP_CONFIG,
      }),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDeclipStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
        ...DECLIP_CONFIG,
      }),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() =>
      masteringRepairDeclipStereo({ left: badLeft, right, sampleRate: SR, ...DECLIP_CONFIG }),
    ).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() =>
      masteringRepairDeclipStereo({ left, right: badRight, sampleRate: SR, ...DECLIP_CONFIG }),
    ).toThrow();
  });

  it('rejects a clipThresholdDb above 0 dBFS', () => {
    const { left, right } = buildChannels();
    expect(() =>
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, clipThresholdDb: 0.5 }),
    ).toThrow(RangeError);
    expect(() =>
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, clipThresholdDb: 3 }),
    ).toThrow(RangeError);
  });
});
