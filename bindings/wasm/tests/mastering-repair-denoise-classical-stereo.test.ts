/**
 * Tests for the stereo denoise WASM wrapper
 * (`masteringRepairDenoiseClassicalStereo`).
 *
 * The shape differs from the four repair stereo entries that came before it:
 * one shared `report`, not a `leftReport`/`rightReport` pair. The gain mask is
 * built from the channel-summed power and applied unchanged to both channels,
 * so a per-channel pair would be two copies of one measurement. That summed
 * power also makes `report.detected` the one pair-level, absolute measurement
 * in the result, which the 3 dB case below witnesses directly.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDenoiseClassicalStereo } from '../src/index';

const SR = 22050;
// Half a second: 43 hops at the default 256-sample hop, enough frames for the
// quantile estimator to have a quietest decile to work from, and short enough
// to stay well inside the default test timeout.
const FRAMES = SR / 2;

// Different tones per channel, so an implementation that denoised one channel
// and returned it for both would be visible in the output.
const LEFT_TONE_HZ = 440;
const RIGHT_TONE_HZ = 660;

function sine(freq: number, frames: number, amp: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

// Deterministic LCG noise: the same fixture every run, so a measured level can
// be compared against another run's.
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

function buildNoisyChannels(): { left: Float32Array; right: Float32Array } {
  return {
    left: withNoise(sine(LEFT_TONE_HZ, FRAMES, 0.5), 0.4, 1),
    right: withNoise(sine(RIGHT_TONE_HZ, FRAMES, 0.5), 0.4, 7),
  };
}

describe('masteringRepairDenoiseClassicalStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('denoises both channels under one mask and reports it once', () => {
    const { left, right } = buildNoisyChannels();
    const result = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(result.right.length).toBe(right.length);
    expect(result.left).not.toEqual(result.right);

    // One shared report. The four earlier repair stereo entries carry a
    // leftReport/rightReport pair; pattern-matching them here would be wrong,
    // so their absence is asserted rather than assumed.
    expect(result.report).toBeDefined();
    expect('leftReport' in result).toBe(false);
    expect('rightReport' in result).toBe(false);

    expect(result.report.detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
    expect(result.report.detected.bandFloorDbfs.length).toBe(32);
    expect(Number.isFinite(result.report.detected.floorDbfs)).toBe(true);
    expect(Array.from(result.report.detected.bandFloorDbfs).every((v) => Number.isFinite(v))).toBe(
      true,
    );
    expect(result.report.meanReductionDb).toBeGreaterThan(0);
    expect(result.report.maxReductionDb).toBeGreaterThanOrEqual(result.report.meanReductionDb);
  });

  it('measures a pair-level absolute floor: two equal channels read 3 dB over one', () => {
    const { left } = buildNoisyChannels();
    const silent = new Float32Array(left.length);

    const pair = masteringRepairDenoiseClassicalStereo({ left, right: left, sampleRate: SR });
    const single = masteringRepairDenoiseClassicalStereo({ left, right: silent, sampleRate: SR });

    // The estimator runs on the channel-summed power, so doubling the content
    // moves the floor by exactly 10*log10(2) = 3.0103 dB. This is why a stereo
    // floor is comparable only against another stereo floor.
    expect(pair.report.detected.floorDbfs - single.report.detected.floorDbfs).toBeCloseTo(
      10 * Math.log10(2),
      4,
    );
  });

  it('floors on reductionDb outside spectralSubtraction, and never inside it', () => {
    const { left, right } = buildNoisyChannels();
    // A shallow floor so it binds somewhere; at the 26 dB default this fixture
    // need not reach it at all, which would make the assertion below vacuous.
    const options = { left, right, sampleRate: SR, reductionDb: 3 };

    const logMmse = masteringRepairDenoiseClassicalStereo(options);
    expect(logMmse.report.floorLimitedFraction).toBeGreaterThan(0);
    expect(logMmse.report.maxReductionDb).toBeCloseTo(3, 3);

    // Not a measurement of this material: spectralSubtraction floors on
    // spectralFloor instead and never fills this field, so 0 here is the mode.
    const spectral = masteringRepairDenoiseClassicalStereo({
      ...options,
      mode: 'spectralSubtraction',
    });
    expect(spectral.report.floorLimitedFraction).toBe(0);
  });

  it('reads overSubtraction and spectralFloor only in spectralSubtraction mode', () => {
    const { left, right } = buildNoisyChannels();
    const base = { left, right, sampleRate: SR };
    const knobs = { overSubtraction: 6, spectralFloor: 0.5 };

    // At the default logMmse the two Berouti knobs are dead: same samples, bit
    // for bit.
    const defaultMode = masteringRepairDenoiseClassicalStereo(base);
    const defaultModeWithKnobs = masteringRepairDenoiseClassicalStereo({ ...base, ...knobs });
    expect(defaultModeWithKnobs.left).toEqual(defaultMode.left);
    expect(defaultModeWithKnobs.right).toEqual(defaultMode.right);

    // The control: in the mode that does read them, the same two values move
    // the output, so the equality above is about the mode and not about the
    // values being too small to matter.
    const spectral = masteringRepairDenoiseClassicalStereo({
      ...base,
      mode: 'spectralSubtraction',
    });
    const spectralWithKnobs = masteringRepairDenoiseClassicalStereo({
      ...base,
      mode: 'spectralSubtraction',
      ...knobs,
    });
    expect(spectralWithKnobs.left).not.toEqual(spectral.left);
  });

  it('rejects an input shorter than nFft, unlike the dereverb pair', () => {
    const { left, right } = buildNoisyChannels();
    const shortLeft = left.slice(0, 512);
    const shortRight = right.slice(0, 512);
    expect(() =>
      masteringRepairDenoiseClassicalStereo(shortLeft, shortRight, SR, { nFft: 1024 }),
    ).toThrow();
  });

  it('accepts the positional call form identically to the request form', () => {
    const { left, right } = buildNoisyChannels();
    const positional = masteringRepairDenoiseClassicalStereo(left, right, SR);
    const request = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });
    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
    expect(positional.report.detected.floorDbfs).toBe(request.report.detected.floorDbfs);
    expect(positional.report.meanReductionDb).toBe(request.report.meanReductionDb);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right.slice(0, right.length - 1), SR),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDenoiseClassicalStereo(new Float32Array(0), new Float32Array(0), SR),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildNoisyChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() => masteringRepairDenoiseClassicalStereo(badLeft, right, SR)).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() => masteringRepairDenoiseClassicalStereo(left, badRight, SR)).toThrow();
  });

  it('rejects a NaN sample rate', () => {
    const { left, right } = buildNoisyChannels();
    expect(() => masteringRepairDenoiseClassicalStereo(left, right, Number.NaN)).toThrow();
  });

  it('rejects a wrong-typed sample rate', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right, 'not-a-number' as unknown as number),
    ).toThrow();
  });

  it('rejects a non-power-of-two nFft and a non-positive hopLength', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right, SR, { nFft: 1500, hopLength: 256 }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right, SR, { nFft: 1024, hopLength: 0 }),
    ).toThrow();
  });

  it('rejects an unknown mode or noise estimator', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right, SR, { mode: 'not-a-mode' as never }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo(left, right, SR, {
        noiseEstimator: 'not-an-estimator' as never,
      }),
    ).toThrow();
  });
});
