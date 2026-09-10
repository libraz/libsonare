/**
 * Tests for the remaining offline mastering repair Node wrappers
 * (declip / decrackle / dehum / dereverbClassical / trimSilence).
 *
 * The shape mirrors repair.test.ts: marshaling + options plumbing + validation.
 */

import { describe, expect, it } from 'vitest';
import type { RoomEstimateResult } from '../src/index.js';
import {
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDereverbClassical,
  masteringRepairDereverbConfigForRoom,
  masteringRepairTrimSilence,
} from '../src/index.js';

const SR = 22050;

function sine(freq: number, durationSec: number, amp = 0.3): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('masteringRepairDeclip (Node)', () => {
  it('returns same-length buffer with defaults', () => {
    const samples = sine(440, 0.3);
    // Hard-clip the signal.
    for (let i = 0; i < samples.length; i++) {
      const v = samples[i] ?? 0;
      samples[i] = Math.max(-0.9, Math.min(0.9, v * 2));
    }
    const out = masteringRepairDeclip(samples, SR);
    expect(out).toBeInstanceOf(Float32Array);
    expect(out.length).toBe(samples.length);
  });

  it('accepts explicit options', () => {
    const samples = sine(440, 0.2);
    const out = masteringRepairDeclip(samples, SR, {
      clipThreshold: 0.85,
      lpcOrder: 24,
      iterations: 1,
      lpcBlend: 0.5,
    });
    expect(out.length).toBe(samples.length);
  });
});

describe('masteringRepairDecrackle (Node)', () => {
  // Factory: regenerate per-test so no test can leak mutations into the next.
  function crackledSamples(): Float32Array {
    const samples = sine(440, 0.3);
    for (let i = 500; i < samples.length; i += 1700) {
      samples[i] = i % 2 === 0 ? 0.95 : -0.95;
    }
    return samples;
  }

  it('median mode (default)', () => {
    const samples = crackledSamples();
    const out = masteringRepairDecrackle(samples, SR);
    expect(out.length).toBe(samples.length);
  });

  it('wavelet shrinkage mode', () => {
    const samples = crackledSamples();
    const out = masteringRepairDecrackle(samples, SR, {
      mode: 'waveletShrinkage',
      threshold: 0.4,
      levels: 4,
    });
    expect(out.length).toBe(samples.length);
  });

  it('rejects unknown mode', () => {
    const samples = crackledSamples();
    expect(() => masteringRepairDecrackle(samples, SR, { mode: 'not-a-mode' as never })).toThrow(
      /unknown decrackle mode/i,
    );
  });
});

describe('masteringRepairDehum (Node)', () => {
  // Factory: regenerate per-test so no test can leak mutations into the next.
  function hummingSamples(): Float32Array {
    const signal = sine(440, 0.5, 0.5);
    const hum = sine(50, 0.5, 0.2);
    const samples = new Float32Array(signal.length);
    for (let i = 0; i < signal.length; i++) {
      samples[i] = (signal[i] ?? 0) + (hum[i] ?? 0);
    }
    return samples;
  }

  it('static notch (default)', () => {
    const samples = hummingSamples();
    const out = masteringRepairDehum(samples, SR);
    expect(out.length).toBe(samples.length);
  });

  it('adaptive tracking', () => {
    const samples = hummingSamples();
    const out = masteringRepairDehum(samples, SR, {
      fundamentalHz: 50,
      harmonics: 4,
      q: 20,
      adaptive: true,
      searchRangeHz: 2,
      adaptation: 0.25,
      frameSize: 2048,
      pllBandwidth: 0.01,
    });
    expect(out.length).toBe(samples.length);
  });
});

describe('masteringRepairDereverbClassical (Node)', () => {
  // Factory: regenerate per-test so no test can leak mutations into the next.
  function reverbInput(): Float32Array {
    return sine(440, 0.5, 0.5);
  }
  // Lazily evaluated reference for validation-only assertions that don't
  // exercise the C call (e.g. parameter-rejection branches).
  const samples = reverbInput();

  it('runs with default config', () => {
    const fresh = reverbInput();
    const out = masteringRepairDereverbClassical(fresh, SR);
    expect(out.length).toBe(fresh.length);
  });

  it('runs with WPE enabled', () => {
    const fresh = reverbInput();
    const out = masteringRepairDereverbClassical(fresh, SR, {
      wpeEnabled: true,
      wpeIterations: 2,
      wpeTaps: 3,
      wpeStrength: 0.7,
      nFft: 1024,
      hopLength: 256,
    });
    expect(out.length).toBe(fresh.length);
  });

  it('rejects non-power-of-two nFft', () => {
    expect(() => masteringRepairDereverbClassical(samples, SR, { nFft: 1500 })).toThrow();
  });

  it('rejects hopLength > nFft', () => {
    expect(() =>
      masteringRepairDereverbClassical(samples, SR, { nFft: 1024, hopLength: 2048 }),
    ).toThrow();
  });
});

describe('masteringRepairDereverbConfigForRoom (Node)', () => {
  // Bands 125/250/500/1k/2k/4k: only the middle pair counts, so the outliers on
  // either side are far from the answer on purpose.
  const MID_BANDS = [9, 9, 1, 2, 9, 9];

  function roomEstimate(volume: number, rt60Bands: number[] = MID_BANDS): RoomEstimateResult {
    return {
      volume,
      length: 0,
      width: 0,
      height: 0,
      drrDb: 0,
      confidence: 0,
      absorptionBands: new Float32Array(0),
      rt60Bands: new Float32Array(rt60Bands),
    };
  }

  it('sets the two fields a measurement determines', () => {
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(2500));
    expect(config.t60Sec).toBeCloseTo(1.5, 5);
    expect(config.lateDelayMs).toBeCloseTo(50, 5); // sqrt(2500)
  });

  it('leaves the taste fields as they went in', () => {
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(2500), {
      attenuation: 0.9,
      threshold: 0.02,
      overSubtraction: 1.4,
      spectralFloor: 0.05,
      nFft: 2048,
      hopLength: 512,
    });
    expect(config.attenuation).toBeCloseTo(0.9, 5);
    expect(config.threshold).toBeCloseTo(0.02, 5);
    expect(config.overSubtraction).toBeCloseTo(1.4, 5);
    expect(config.spectralFloor).toBeCloseTo(0.05, 5);
    expect(config.nFft).toBe(2048);
    expect(config.hopLength).toBe(512);
  });

  it('follows the volume for the mixing time', () => {
    expect(masteringRepairDereverbConfigForRoom(roomEstimate(100)).lateDelayMs).toBeCloseTo(10, 5);
    expect(masteringRepairDereverbConfigForRoom(roomEstimate(20000)).lateDelayMs).toBeCloseTo(
      141.42,
      1,
    );
  });

  it('configures only the time when the volume is unusable', () => {
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(0), { lateDelayMs: 33 });
    expect(config.t60Sec).toBeCloseTo(1.5, 5);
    expect(config.lateDelayMs).toBeCloseTo(33, 5);
  });

  it('configures only the delay when no band converged', () => {
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(900, []), { t60Sec: 0.7 });
    expect(config.t60Sec).toBeCloseTo(0.7, 5);
    expect(config.lateDelayMs).toBeCloseTo(30, 5);
  });

  it('skips a band that did not converge rather than averaging it in', () => {
    const config = masteringRepairDereverbConfigForRoom(
      roomEstimate(400, [0, 0, Number.NaN, 2, 0, 0]),
    );
    expect(config.t60Sec).toBeCloseTo(2, 5);
  });

  it('rejects a missing estimate', () => {
    expect(() =>
      // @ts-expect-error deliberately passing the null the C ABI rejects
      masteringRepairDereverbConfigForRoom(null),
    ).toThrow();
    expect(() =>
      // @ts-expect-error deliberately passing no estimate at all
      masteringRepairDereverbConfigForRoom(undefined),
    ).toThrow();
  });

  it('resolves an omitted field to the library default, never to zero', () => {
    // The C ABI takes every config field literally -- it has no zero-is-default
    // rule -- so an estimate alone must still yield a config that is ready to run.
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(2500));
    expect(config.threshold).toBeCloseTo(0.05, 5);
    expect(config.attenuation).toBeCloseTo(0.5, 5);
    expect(config.nFft).toBe(1024);
    expect(config.hopLength).toBe(256);
    expect(config.overSubtraction).toBeCloseTo(1, 5);
    expect(config.spectralFloor).toBeCloseTo(0.08, 5);
    expect(config.wpeEnabled).toBe(false);
    expect(config.wpeIterations).toBe(2);
    expect(config.wpeTaps).toBe(3);
    expect(config.wpeStrength).toBeCloseTo(0.7, 5);
  });

  it('takes an explicit zero literally', () => {
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(2500), {
      attenuation: 0,
      spectralFloor: 0,
    });
    expect(config.attenuation).toBe(0);
    expect(config.spectralFloor).toBe(0);
  });

  it('agrees between the request and positional forms', () => {
    const estimate = roomEstimate(2500);
    const positional = masteringRepairDereverbConfigForRoom(estimate, { attenuation: 0.9 });
    const request = masteringRepairDereverbConfigForRoom({ estimate, attenuation: 0.9 });
    expect(request).toEqual(positional);
  });

  it('produces a config the dereverberator accepts', () => {
    const samples = sine(440, 0.3, 0.5);
    const config = masteringRepairDereverbConfigForRoom(roomEstimate(2500));
    const out = masteringRepairDereverbClassical(samples, SR, config);
    expect(out.length).toBe(samples.length);
  });
});

describe('masteringRepairTrimSilence (Node)', () => {
  function withSilence(): Float32Array {
    const pad = 1200;
    const sig = sine(440, 0.2, 0.5);
    const out = new Float32Array(pad + sig.length + pad);
    out.set(sig, pad);
    return out;
  }

  it('peak mode (default) shortens the buffer', () => {
    const samples = withSilence();
    const out = masteringRepairTrimSilence(samples, SR);
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(samples.length);
  });

  it('LUFS-gated mode with padding', () => {
    const samples = withSilence();
    const out = masteringRepairTrimSilence(samples, SR, {
      mode: 'lufsGated',
      gateLufs: -40,
      windowMs: 400,
      paddingSamples: 600,
    });
    expect(out.length).toBeGreaterThan(0);
  });

  it('rejects unknown mode and negative padding', () => {
    const samples = withSilence();
    expect(() => masteringRepairTrimSilence(samples, SR, { mode: 'not-a-mode' as never })).toThrow(
      /unknown trim silence mode/i,
    );
    expect(() => masteringRepairTrimSilence(samples, SR, { paddingSamples: -1 })).toThrow(
      /paddingSamples must be non-negative/i,
    );
  });
});
