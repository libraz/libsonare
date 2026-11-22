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
import { present } from './_helpers';

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

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);
    expect(result.channels[0].length).toBe(left.length);
    expect(result.channels[1].length).toBe(right.length);
    expect(result.channels[0]).not.toEqual(result.channels[1]);

    // One shared report. The four earlier repair stereo entries carry a
    // leftReport/rightReport pair; pattern-matching them here would be wrong,
    // so their absence is asserted rather than assumed.
    expect(result.reports[0]).toBeDefined();
    expect('leftReport' in result).toBe(false);
    expect('rightReport' in result).toBe(false);

    expect(result.reports[0].detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
    // Compact: one entry per band holding a bin, named by bandFloorBand.
    const detectedBands = result.reports[0].detected.bandFloorBand;
    expect(detectedBands).toBeInstanceOf(Int32Array);
    expect(detectedBands.length).toBe(result.reports[0].detected.bandFloorDbfs.length);
    expect(detectedBands.length).toBeGreaterThan(0);
    expect(detectedBands.length).toBeLessThanOrEqual(32);
    expect(
      Array.from(detectedBands).every(
        (b, i) => b >= 0 && b < 32 && (i === 0 || b > detectedBands[i - 1]),
      ),
    ).toBe(true);
    expect(Number.isFinite(result.reports[0].detected.floorDbfs)).toBe(true);
    expect(
      Array.from(result.reports[0].detected.bandFloorDbfs).every((v) => Number.isFinite(v)),
    ).toBe(true);
    expect(result.reports[0].meanReductionDb).toBeGreaterThan(0);
    expect(result.reports[0].maxReductionDb).toBeGreaterThanOrEqual(
      result.reports[0].meanReductionDb,
    );
  });

  it('measures a pair-level absolute floor: two equal channels read 3 dB over one', () => {
    const { left } = buildNoisyChannels();
    const silent = new Float32Array(left.length);

    const pair = masteringRepairDenoiseClassicalStereo({ left, right: left, sampleRate: SR });
    const single = masteringRepairDenoiseClassicalStereo({ left, right: silent, sampleRate: SR });

    // The estimator runs on the channel-summed power, so doubling the content
    // moves the floor by 10*log10(2) = 3.0103 dB. The power ratio is exactly
    // two, but the dB conversion is float32, which is what the tolerance below
    // is for. This is why a stereo floor is comparable only against another
    // stereo floor.
    expect(
      present(pair.reports[0].detected.floorDbfs) - present(single.reports[0].detected.floorDbfs),
    ).toBeCloseTo(10 * Math.log10(2), 4);
  });

  it('floors on reductionDb outside spectralSubtraction, and never inside it', () => {
    const { left, right } = buildNoisyChannels();
    // A shallow floor so it binds somewhere; at the 26 dB default this fixture
    // need not reach it at all, which would make the assertion below vacuous.
    const options = { left, right, sampleRate: SR, reductionDb: 3 };

    const logMmse = masteringRepairDenoiseClassicalStereo(options);
    expect(logMmse.reports[0].floorLimitedFraction).toBeGreaterThan(0);
    expect(logMmse.reports[0].maxReductionDb).toBeCloseTo(3, 3);

    // Not a measurement of this material: spectralSubtraction floors on
    // spectralFloor instead and never fills this field, so 0 here is the mode.
    const spectral = masteringRepairDenoiseClassicalStereo({
      ...options,
      mode: 'spectralSubtraction',
    });
    expect(spectral.reports[0].floorLimitedFraction).toBe(0);
  });

  it('reads overSubtraction and spectralFloor only in spectralSubtraction mode', () => {
    const { left, right } = buildNoisyChannels();
    const base = { left, right, sampleRate: SR };
    const knobs = { overSubtraction: 6, spectralFloor: 0.5 };

    // At the default logMmse the two Berouti knobs are dead: same samples, bit
    // for bit.
    const defaultMode = masteringRepairDenoiseClassicalStereo(base);
    const defaultModeWithKnobs = masteringRepairDenoiseClassicalStereo({ ...base, ...knobs });
    expect(defaultModeWithKnobs.channels[0]).toEqual(defaultMode.channels[0]);
    expect(defaultModeWithKnobs.channels[1]).toEqual(defaultMode.channels[1]);

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
    expect(spectralWithKnobs.channels[0]).not.toEqual(spectral.channels[0]);
  });

  it('pads an input shorter than nFft, as the dereverb pair does', () => {
    const { left, right } = buildNoisyChannels();
    const shortLeft = left.slice(0, 512);
    const shortRight = right.slice(0, 512);
    const result = masteringRepairDenoiseClassicalStereo({
      left: shortLeft,
      right: shortRight,
      sampleRate: SR,
      nFft: 1024,
    });
    expect(result.channels[0].length).toBe(512);
    expect(result.channels[1].length).toBe(512);
  });

  it('refuses the positional call form', () => {
    const { left, right } = buildNoisyChannels();
    const call = masteringRepairDenoiseClassicalStereo as unknown as (
      ...args: unknown[]
    ) => unknown;
    expect(() => call(left, right, SR)).toThrow(TypeError);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right: right.slice(0, right.length - 1),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildNoisyChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left: badLeft, right, sampleRate: SR }),
    ).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right: badRight, sampleRate: SR }),
    ).toThrow();
  });

  it('rejects a NaN sample rate', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: Number.NaN }),
    ).toThrow();
  });

  it('rejects a wrong-typed sample rate', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right,
        sampleRate: 'not-a-number' as unknown as number,
      }),
    ).toThrow();
  });

  it('rejects a non-power-of-two nFft and a non-positive hopLength', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right,
        sampleRate: SR,
        nFft: 1500,
        hopLength: 256,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right,
        sampleRate: SR,
        nFft: 1024,
        hopLength: 0,
      }),
    ).toThrow();
  });

  it('rejects an unknown mode or noise estimator', () => {
    const { left, right } = buildNoisyChannels();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right,
        sampleRate: SR,
        mode: 'not-a-mode' as never,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalStereo({
        left,
        right,
        sampleRate: SR,
        noiseEstimator: 'not-an-estimator' as never,
      }),
    ).toThrow();
  });
});
