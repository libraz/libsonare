/**
 * The offline mastering entry points validate their input in the C++ core
 * (MasteringChain::process_mono/process_stereo), so the Node wrappers inherit
 * the same empty / out-of-range-rate / non-finite rejection as the C ABI and
 * Python instead of silently producing an empty or garbage result.
 */

import { describe, expect, it } from 'vitest';
import {
  masterAudio,
  masterAudioStereo,
  masteringApplyProcessor,
  masteringNormalizeLoudness,
} from '../src/index.js';

const SR = 44100;

function sine(n: number, freq = 220, amp = 0.5): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('mastering offline input validation (inherited from the C++ core)', () => {
  it('rejects empty input', () => {
    expect(() => masterAudio(new Float32Array(0), SR)).toThrow();
    expect(() => masterAudio(new Float32Array(0), SR, { preset: 'pop' })).toThrow();
  });

  it('rejects an out-of-range sample rate', () => {
    const x = sine(2048);
    expect(() => masterAudio(x, 100)).toThrow();
    expect(() => masterAudio(x, 10_000_000)).toThrow();
  });

  it('rejects non-finite samples', () => {
    const x = sine(2048);
    x[10] = Number.NaN;
    expect(() => masterAudio(x, SR)).toThrow();
    const y = sine(2048);
    y[5] = Number.POSITIVE_INFINITY;
    expect(() => masterAudio(y, SR)).toThrow();
  });

  it('rejects bad input on the stereo path (both channels checked)', () => {
    const l = sine(2048);
    const r = sine(2048);
    r[20] = Number.NaN;
    expect(() => masterAudioStereo({ left: l, right: r, sampleRate: SR })).toThrow();
  });

  it('still masters valid input', () => {
    const x = sine(4096);
    const result = masterAudio(x, SR, { preset: 'pop' });
    expect(result.samples.length).toBe(x.length);
  });

  it('reports output true-peak, LRA, and per-stage gain reductions', () => {
    const x = sine(4096, 220, 0.3);
    const result = masterAudio(x, SR, {
      overrides: {
        dynamics: { compressor: { thresholdDb: -30, ratio: 4 } },
        loudness: { targetLufs: -14, ceilingDbtp: -1 },
      },
    });
    expect(Number.isFinite(result.outputTruePeakDbtp)).toBe(true);
    expect(result.outputTruePeakDbtp).toBeLessThanOrEqual(0);
    expect(Number.isFinite(result.outputLoudnessRangeLu)).toBe(true);
    expect(result.outputLoudnessRangeLu).toBeGreaterThanOrEqual(0);

    const grStages = result.stageGainReductions.map((r) => r.stage);
    expect(grStages).toContain('dynamics.compressor');
    for (const reduction of result.stageGainReductions) {
      expect(result.stages).toContain(reduction.stage);
      expect(reduction.gainReductionDb).toBeLessThanOrEqual(0);
    }

    // masterAudio (preset path) exposes the same fields.
    const preset = masterAudio(x, SR, { preset: 'pop' });
    expect(Number.isFinite(preset.outputTruePeakDbtp)).toBe(true);
    expect(Number.isFinite(preset.outputLoudnessRangeLu)).toBe(true);
    expect(Array.isArray(preset.stageGainReductions)).toBe(true);
  });
});

describe('masteringNormalizeLoudness() accepts the appended maximizer knobs', () => {
  it('accepts releaseMs / applyGainAtInputRate and returns valid output', () => {
    const x = sine(4096, 220, 0.2);
    const result = masteringNormalizeLoudness(x, SR, {
      targetLufs: -14,
      releaseMs: 250,
      applyGainAtInputRate: true,
    });
    expect(result.samples.length).toBe(x.length);
    expect(result.samples.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('refuses an explicit releaseMs of 0 and runs the library default when omitted', () => {
    // A target the peak headroom cannot reach, so the limiter is doing the work
    // and its release time is visible in the output. Without that the three
    // results below would agree because nothing reads releaseMs at all, and the
    // equality would hold for a wrapper that dropped the option entirely.
    const x = sine(4096, 220, 0.2);
    const driven = { targetLufs: 0, ceilingDbtp: -6 };
    const omitted = masteringNormalizeLoudness(x, SR, driven);
    expect(() => masteringNormalizeLoudness(x, SR, { ...driven, releaseMs: 0 })).toThrow(
      RangeError,
    );
    const explicit = masteringNormalizeLoudness(x, SR, { ...driven, releaseMs: 50 });
    const immediate = masteringNormalizeLoudness(x, SR, { ...driven, releaseMs: 1 });
    expect(omitted.samples).toEqual(explicit.samples);
    // The varying control: releaseMs reaches the limiter on this fixture.
    expect(omitted.samples).not.toEqual(immediate.samples);
  });

  it('refuses the releaseMs values it cannot honour instead of ignoring them', () => {
    // The wrapper used to apply releaseMs only when it was positive, so a
    // negative or non-finite request silently ran the library default and the
    // caller never learned its value had been discarded. The facade now sees the
    // value, which is why these throw; 0 is refused too, because omitting the
    // field is how the default is asked for.
    const x = sine(4096, 220, 0.2);
    const refused = [0, -1, -0.5, Number.NaN, Number.POSITIVE_INFINITY];
    for (const releaseMs of refused) {
      expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: -14, releaseMs })).toThrow();
    }
    // The controls. A wrapper that had started refusing every releaseMs would
    // pass every assertion above and fail only here.
    for (const releaseMs of [1, 50, 250]) {
      expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: -14, releaseMs })).not.toThrow();
    }
  });

  it('rejects non-finite targets and accepts a later finite call', () => {
    const x = sine(4096, 220, 0.2);
    expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: Number.NaN })).toThrow();
    expect(() =>
      masteringNormalizeLoudness(x, SR, { ceilingDbtp: Number.POSITIVE_INFINITY }),
    ).toThrow();
    const recovered = masteringNormalizeLoudness(x, SR, { targetLufs: -14, ceilingDbtp: -1 });
    expect(recovered.samples.every((value) => Number.isFinite(value))).toBe(true);
  });
});

describe('named mastering parameter validation', () => {
  it('rejects non-finite direct parameters before processing', () => {
    const x = sine(2048);
    const processor = 'dynamics.compressor' as const;
    const apply = (samples: Float32Array, sampleRate: number, params: object) =>
      masteringApplyProcessor({ processor, samples, sampleRate, params: params as never });
    expect(() => apply(x, 0, { ratio: 2 })).toThrow();
    const invalidSamples = x.slice();
    invalidSamples[4] = Number.NaN;
    expect(() => apply(invalidSamples, SR, { ratio: 2 })).toThrow();
    expect(() => apply(x, SR, { ratio: Number.NaN })).toThrow();
    expect(() => apply(x, SR, { ratio: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => apply(x, SR, { ratio: 2 })).not.toThrow();
  });
});
