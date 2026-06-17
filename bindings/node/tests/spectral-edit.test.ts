/**
 * Tests for the offline spectralEdit Node wrapper.
 *
 * Verifies marshaling (Float32Array out, ops/options plumbing, string-enum
 * mapping), identity behaviour with no ops, that attenuating a band lowers that
 * band's energy on a two-tone signal, and input validation.
 */

import { describe, expect, it } from 'vitest';
import { spectralEdit } from '../src/index.js';

const SR = 22050;

function sine(freq: number, durationSec: number, amp = 0.5): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function twoTone(lowFreq: number, highFreq: number, durationSec: number): Float32Array {
  const low = sine(lowFreq, durationSec, 0.4);
  const high = sine(highFreq, durationSec, 0.4);
  const out = new Float32Array(low.length);
  for (let i = 0; i < out.length; i++) {
    out[i] = (low[i] ?? 0) + (high[i] ?? 0);
  }
  return out;
}

/** Goertzel single-bin power estimate at `freq` (a cheap band energy probe). */
function tonePower(samples: Float32Array, freq: number): number {
  const w = (2 * Math.PI * freq) / SR;
  const cosw = Math.cos(w);
  const coeff = 2 * cosw;
  let s0 = 0;
  let s1 = 0;
  let s2 = 0;
  for (let i = 0; i < samples.length; i++) {
    s0 = (samples[i] ?? 0) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

describe('spectralEdit (Node)', () => {
  it('identity (no ops) preserves length and stays finite', () => {
    const samples = sine(440, 0.3);
    const out = spectralEdit(samples, SR);
    expect(out).toBeInstanceOf(Float32Array);
    expect(out.length).toBe(samples.length);
    for (let i = 0; i < out.length; i += 97) {
      expect(Number.isFinite(out[i] ?? 0)).toBe(true);
    }
  });

  it('attenuating a band reduces that band energy on a two-tone signal', () => {
    const lowHz = 300;
    const highHz = 4000;
    const samples = twoTone(lowHz, highHz, 0.5);

    const beforeHigh = tonePower(samples, highHz);
    const beforeLow = tonePower(samples, lowHz);

    const out = spectralEdit(
      samples,
      SR,
      [
        {
          startSample: 0,
          endSample: samples.length,
          lowHz: 2000,
          highHz: 6000,
          gainDb: -40,
          mode: 'attenuate',
        },
      ],
      { nFft: 2048, hopLength: 512, window: 'hann' },
    );

    expect(out.length).toBe(samples.length);

    const afterHigh = tonePower(out, highHz);
    const afterLow = tonePower(out, lowHz);

    // The targeted high band drops substantially.
    expect(afterHigh).toBeLessThan(beforeHigh * 0.5);
    // The untouched low band is largely preserved.
    expect(afterLow).toBeGreaterThan(beforeLow * 0.5);
  });

  it('supports mute and gain modes', () => {
    const samples = twoTone(300, 4000, 0.3);
    const muted = spectralEdit(samples, SR, [{ lowHz: 2000, highHz: 6000, mode: 'mute' }]);
    expect(muted.length).toBe(samples.length);
    expect(tonePower(muted, 4000)).toBeLessThan(tonePower(samples, 4000) * 0.5);

    const gained = spectralEdit(samples, SR, [
      { lowHz: 2000, highHz: 6000, gainDb: 3, mode: 'gain' },
    ]);
    expect(gained.length).toBe(samples.length);
  });

  it('rejects an unknown mode string', () => {
    const samples = sine(440, 0.1);
    expect(() => spectralEdit(samples, SR, [{ mode: 'not-a-mode' as never }])).toThrow(
      /unknown mode/i,
    );
  });

  it('rejects an unknown window string', () => {
    const samples = sine(440, 0.1);
    expect(() => spectralEdit(samples, SR, [], { window: 'triangle' as never })).toThrow(
      /unknown window/i,
    );
  });

  it('rejects a non-array ops argument', () => {
    const samples = sine(440, 0.1);
    // biome-ignore lint/suspicious/noExplicitAny: deliberately wrong-typed arg
    expect(() => spectralEdit(samples, SR, 'nope' as any)).toThrow();
  });
});
