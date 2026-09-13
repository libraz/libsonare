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

  it('takes a window ordinal and reaches the same window the name does', () => {
    // Both spellings are accepted on every surface, so a generated binding that
    // emits the ordinal must not fall through to a different window. The name
    // and the ordinal have to produce a bit-identical signal, and the members
    // have to separate from each other, or the comparison proves nothing.
    const samples = sine(440, 0.2);
    const ops = [
      {
        mode: 'attenuate' as const,
        startFrame: 2,
        endFrame: 20,
        lowBin: 60,
        highBin: 120,
        gainDb: -18,
      },
    ];
    const render = (window: unknown) =>
      spectralEdit(samples, SR, ops, { nFft: 1024, hopLength: 256, window: window as never });
    const members: [string, number][] = [
      ['hann', 0],
      ['hamming', 1],
      ['blackman', 2],
      ['rectangular', 3],
    ];
    const byName = new Map(members.map(([name]) => [name, render(name)]));
    for (let i = 0; i < members.length; i++) {
      for (let j = i + 1; j < members.length; j++) {
        expect(
          byName.get(members[i][0]),
          `${members[i][0]} and ${members[j][0]} render identically, so this comparison cannot see which window was used`,
        ).not.toEqual(byName.get(members[j][0]));
      }
    }
    for (const [name, ordinal] of members) {
      expect(render(ordinal), `ordinal ${ordinal} did not reach ${name}`).toEqual(byName.get(name));
    }
  });

  it('rejects a window ordinal outside the enum, and a fractional one', () => {
    const samples = sine(440, 0.1);
    const render = (window: unknown) => spectralEdit(samples, SR, [], { window: window as never });
    // The bound is the enum's own, so a member added later widens the accepted
    // set rather than changing what the first rejected value means.
    for (const value of [4, 5, -1]) {
      expect(() => render(value), `ordinal ${value}`).toThrow(/unknown window/i);
    }
    // An ordinal is a member, not a magnitude: truncating 1.5 would select a
    // window the caller never named.
    for (const value of [0.5, 1.5]) {
      expect(() => render(value), `ordinal ${value}`).toThrow(/must be an integer/i);
    }
    for (const value of [2 ** 32, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => render(value), `ordinal ${value}`).toThrow(RangeError);
    }
    expect(() => render(true)).toThrow(/window name or a window ordinal/i);
  });

  it('rejects a non-array ops argument', () => {
    const samples = sine(440, 0.1);
    // biome-ignore lint/suspicious/noExplicitAny: deliberately wrong-typed arg
    expect(() => spectralEdit(samples, SR, 'nope' as any)).toThrow();
  });
});
