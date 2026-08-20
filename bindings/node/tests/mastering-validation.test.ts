/**
 * The offline mastering entry points validate their input in the C++ core
 * (MasteringChain::process_mono/process_stereo), so the Node wrappers inherit
 * the same empty / out-of-range-rate / non-finite rejection as the C ABI and
 * Python instead of silently producing an empty or garbage result.
 */

import { describe, expect, it } from 'vitest';
import {
  masterAudio,
  mastering,
  masteringChain,
  masteringChainStereo,
  masteringProcess,
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
    expect(() => masteringChain(new Float32Array(0), SR)).toThrow();
    expect(() => masterAudio(new Float32Array(0), SR)).toThrow();
  });

  it('rejects an out-of-range sample rate', () => {
    const x = sine(2048);
    expect(() => masteringChain(x, 100)).toThrow();
    expect(() => masteringChain(x, 10_000_000)).toThrow();
  });

  it('rejects non-finite samples', () => {
    const x = sine(2048);
    x[10] = Number.NaN;
    expect(() => masteringChain(x, SR)).toThrow();
    const y = sine(2048);
    y[5] = Number.POSITIVE_INFINITY;
    expect(() => masteringChain(y, SR)).toThrow();
  });

  it('rejects bad input on the stereo path (both channels checked)', () => {
    const l = sine(2048);
    const r = sine(2048);
    r[20] = Number.NaN;
    expect(() => masteringChainStereo(l, r, SR)).toThrow();
  });

  it('still masters valid input', () => {
    const x = sine(4096);
    const result = masteringChain(x, SR);
    expect(result.samples.length).toBe(x.length);
  });

  it('reports output true-peak, LRA, and per-stage gain reductions', () => {
    const x = sine(4096, 220, 0.3);
    const result = masteringChain(x, SR, {
      dynamics: { compressor: { thresholdDb: -30, ratio: 4 } },
      loudness: { targetLufs: -14, ceilingDb: -1 },
    });
    expect(Number.isFinite(result.outputTruePeakDbtp)).toBe(true);
    expect(result.outputTruePeakDbtp).toBeLessThanOrEqual(0);
    expect(Number.isFinite(result.outputLra)).toBe(true);
    expect(result.outputLra).toBeGreaterThanOrEqual(0);

    const grStages = result.stageGainReductions.map((r) => r.stage);
    expect(grStages).toContain('dynamics.compressor');
    for (const reduction of result.stageGainReductions) {
      expect(result.stages).toContain(reduction.stage);
      expect(reduction.gainReductionDb).toBeLessThanOrEqual(0);
    }

    // masterAudio (preset path) exposes the same fields.
    const preset = masterAudio(x, SR, 'pop');
    expect(Number.isFinite(preset.outputTruePeakDbtp)).toBe(true);
    expect(Number.isFinite(preset.outputLra)).toBe(true);
    expect(Array.isArray(preset.stageGainReductions)).toBe(true);
  });
});

describe('simple mastering() accepts the appended maximizer knobs', () => {
  it('accepts releaseMs / applyGainAtInputRate and returns valid output', () => {
    const x = sine(4096, 220, 0.2);
    const result = mastering(x, SR, {
      targetLufs: -14,
      releaseMs: 250,
      applyGainAtInputRate: true,
    });
    expect(result.samples.length).toBe(x.length);
    expect(result.samples.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('treats releaseMs 0 (or omitted) as the library default', () => {
    const x = sine(4096, 220, 0.2);
    const omitted = mastering(x, SR, { targetLufs: -14 });
    const zero = mastering(x, SR, { targetLufs: -14, releaseMs: 0 });
    const explicit = mastering(x, SR, { targetLufs: -14, releaseMs: 50 });
    expect(zero.samples).toEqual(omitted.samples);
    expect(zero.samples).toEqual(explicit.samples);
  });

  it('rejects non-finite targets and accepts a later finite call', () => {
    const x = sine(4096, 220, 0.2);
    expect(() => mastering(x, SR, { targetLufs: Number.NaN })).toThrow();
    expect(() => mastering(x, SR, { ceilingDb: Number.POSITIVE_INFINITY })).toThrow();
    const recovered = mastering(x, SR, { targetLufs: -14, ceilingDb: -1 });
    expect(recovered.samples.every((value) => Number.isFinite(value))).toBe(true);
  });
});

describe('named mastering parameter validation', () => {
  it('rejects non-finite direct parameters before processing', () => {
    const x = sine(2048);
    expect(() => masteringProcess('dynamics.compressor', x, 0, { ratio: 2 })).toThrow();
    const invalidSamples = x.slice();
    invalidSamples[4] = Number.NaN;
    expect(() =>
      masteringProcess('dynamics.compressor', invalidSamples, SR, { ratio: 2 }),
    ).toThrow();
    expect(() => masteringProcess('dynamics.compressor', x, SR, { ratio: Number.NaN })).toThrow();
    expect(() =>
      masteringProcess('dynamics.compressor', x, SR, { ratio: Number.POSITIVE_INFINITY }),
    ).toThrow();
    expect(() => masteringProcess('dynamics.compressor', x, SR, { ratio: 2 })).not.toThrow();
  });
});
