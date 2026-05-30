/**
 * Tests for the remaining offline mastering repair WASM wrappers
 * (declip / decrackle / dehum / dereverbClassical / trimSilence).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
} from '../src/index';

const SR = 22050;

function sine(freq: number, durationSec: number, amp = 0.3): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('masteringRepairDeclip (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns same-length buffer with defaults', () => {
    const samples = sine(440, 0.3, 1.0);
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

describe('masteringRepairDecrackle (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('median mode (default)', () => {
    const samples = sine(440, 0.3);
    for (let i = 500; i < samples.length; i += 1700) {
      samples[i] = i % 2 === 0 ? 0.95 : -0.95;
    }
    const out = masteringRepairDecrackle(samples, SR);
    expect(out.length).toBe(samples.length);
  });

  it('wavelet shrinkage mode', () => {
    const samples = sine(440, 0.3);
    const out = masteringRepairDecrackle(samples, SR, {
      mode: 'waveletShrinkage',
      threshold: 0.4,
      levels: 4,
    });
    expect(out.length).toBe(samples.length);
  });

  it('rejects unknown mode', () => {
    const samples = sine(440, 0.3);
    expect(() => masteringRepairDecrackle(samples, SR, { mode: 'not-a-mode' as never })).toThrow();
  });
});

describe('masteringRepairDehum (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const signal = sine(440, 0.5, 0.5);
  const hum = sine(50, 0.5, 0.2);
  const samples = new Float32Array(signal.length);
  for (let i = 0; i < signal.length; i++) {
    samples[i] = (signal[i] ?? 0) + (hum[i] ?? 0);
  }

  it('static notch (default)', () => {
    const out = masteringRepairDehum(samples, SR);
    expect(out.length).toBe(samples.length);
  });

  it('adaptive tracking', () => {
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

describe('masteringRepairDereverbClassical (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const samples = sine(440, 0.5, 0.5);

  it('runs with default config', () => {
    const out = masteringRepairDereverbClassical(samples, SR);
    expect(out.length).toBe(samples.length);
  });

  it('runs with WPE enabled', () => {
    const out = masteringRepairDereverbClassical(samples, SR, {
      wpeEnabled: true,
      wpeIterations: 2,
      wpeTaps: 3,
      wpeStrength: 0.7,
      nFft: 1024,
      hopLength: 256,
    });
    expect(out.length).toBe(samples.length);
  });

  it('rejects non-power-of-two nFft', () => {
    expect(() => masteringRepairDereverbClassical(samples, SR, { nFft: 1500 })).toThrow();
  });
});

describe('masteringRepairTrimSilence (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

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
    expect(() =>
      masteringRepairTrimSilence(samples, SR, { mode: 'not-a-mode' as never }),
    ).toThrow();
    expect(() => masteringRepairTrimSilence(samples, SR, { paddingSamples: -1 })).toThrow();
  });
});
