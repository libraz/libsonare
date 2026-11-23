/**
 * Tests for the offline spectralEdit Node wrapper.
 *
 * Verifies marshaling (Float32Array out, ops/options plumbing, string-enum
 * mapping), identity behaviour with no ops, that attenuating a band lowers that
 * band's energy on a two-tone signal, and input validation.
 */

import { describe, expect, it } from 'vitest';
import { spectralEdit } from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers.js';

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
    const out = spectralEdit(samples, SR, { ops: [] });
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

    const out = spectralEdit(samples, SR, {
      ops: [
        {
          startSample: 0,
          endSample: samples.length,
          lowHz: 2000,
          highHz: 6000,
          gainDb: -40,
          mode: 'attenuate',
        },
      ],
      nFft: 2048,
      hopLength: 512,
      window: 'hann',
    });

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
    const muted = spectralEdit(samples, SR, { ops: [{ lowHz: 2000, highHz: 6000, mode: 'mute' }] });
    expect(muted.length).toBe(samples.length);
    expect(tonePower(muted, 4000)).toBeLessThan(tonePower(samples, 4000) * 0.5);

    const gained = spectralEdit(samples, SR, {
      ops: [{ lowHz: 2000, highHz: 6000, gainDb: 3, mode: 'gain' }],
    });
    expect(gained.length).toBe(samples.length);
  });

  it('rejects an unknown mode string', () => {
    const samples = sine(440, 0.1);
    expect(() => spectralEdit(samples, SR, { ops: [{ mode: 'not-a-mode' as never }] })).toThrow(
      /op\.mode must be one of/,
    );
  });

  it('rejects an unknown window string', () => {
    const samples = sine(440, 0.1);
    expect(() => spectralEdit(samples, SR, { ops: [], window: 'triangle' as never })).toThrow(
      /window must be one of/,
    );
  });

  it('renders every window name distinctly', () => {
    // The members have to separate from each other, or a name that fell through
    // to another window would pass unnoticed.
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
      spectralEdit(samples, SR, { ops, nFft: 1024, hopLength: 256, window: window as never });
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
  });

  it('refuses a window ordinal rather than aliasing it to a name', () => {
    const samples = sine(440, 0.1);
    for (const ordinal of [0, 1, 2, 3]) {
      expect(
        () => spectralEdit(samples, SR, { ops: [], window: ordinal as never }),
        `ordinal ${ordinal}`,
      ).toThrow(TypeError);
    }
  });

  it('states the window ordinal domain in the enum words at the addon', () => {
    const samples = sine(440, 0.1);
    // The facade takes names only, so the ordinal reader is reached through the addon.
    // biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose.
    const native = addon as any;
    expectRefusal(
      () => native.spectralEdit(samples, SR, [], { window: 4 }),
      RangeError,
      /^spectralEdit: window must be a name or an ordinal in \[0, 4\)$/,
    );
  });

  it('refuses a non-string window of any kind', () => {
    const samples = sine(440, 0.1);
    const render = (window: unknown) =>
      spectralEdit(samples, SR, { ops: [], window: window as never });
    for (const value of [4, -1, 0.5, 1.5, 2 ** 32, Number.NaN, Number.POSITIVE_INFINITY, true]) {
      expect(() => render(value), `window ${value}`).toThrow();
    }
  });

  it('rejects a non-array ops argument', () => {
    const samples = sine(440, 0.1);
    // biome-ignore lint/suspicious/noExplicitAny: deliberately wrong-typed arg
    expect(() => spectralEdit(samples, SR, { ops: 'nope' as any })).toThrow();
  });

  it.each([6, 1000, 2])('refuses nFft %i naming nFft and the power-of-two rule', (nFft) => {
    expect(() => spectralEdit(sine(440, 0.1), SR, { ops: [{ mode: 'mute' }], nFft })).toThrow(
      /nFft must be a power of two/,
    );
  });

  it('refuses a high edge that is not a positive frequency instead of reading it as Nyquist', () => {
    const samples = sine(440, 0.1);
    for (const highHz of [0, -5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(
        () => spectralEdit(samples, SR, { ops: [{ mode: 'mute' }, { highHz, mode: 'mute' }] }),
        `highHz ${highHz}`,
      ).toThrow(
        new RangeError(
          'spectralEdit: ops[1].highHz must be finite and greater than 0; omit it for Nyquist',
        ),
      );
    }
    const kept = spectralEdit(samples, SR, { ops: [{ highHz: 1e6, mode: 'gain', gainDb: 0 }] });
    expect(kept.length).toBe(samples.length);
  });

  it.each([
    ['nFft', 'nFft must be a power of two in [4, 262144]'],
    ['hopLength', 'hopLength must be an integer in [1, 2147483647]'],
    ['healRadiusFrames', 'healRadiusFrames must be an integer in [1, 2147483647]'],
  ] as const)('refuses %s 0 instead of reading it as the default', (field, text) => {
    expect(() => spectralEdit(sine(440, 0.1), SR, { ops: [], [field]: 0 })).toThrow(
      new RangeError(`spectralEdit: ${text}`),
    );
  });
});
