/**
 * The WASM `masteringNormalizeLoudness()` helper is a thin embind shim over
 * mastering::maximizer::loudness_optimize. This verifies the marshaling of the
 * appended maximizer knobs (releaseMs / applyGainAtInputRate): they are accepted,
 * produce valid length-preserving output, and an omitted release reproduces
 * the library default and an explicit 0 is refused.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringNormalizeLoudness } from '../src/index';
import { unknownKeyText } from './_helpers';

const SR = 44100;

function sine(freq: number, durationSec: number, amp = 0.2): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function allFinite(arr: Float32Array): boolean {
  for (let i = 0; i < arr.length; i++) {
    if (!Number.isFinite(arr[i] ?? Number.NaN)) {
      return false;
    }
  }
  return true;
}

describe('masteringNormalizeLoudness() knobs (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('accepts releaseMs / applyGainAtInputRate and returns valid output', () => {
    const x = sine(220, 0.1);
    const r = masteringNormalizeLoudness(x, SR, {
      targetLufs: -14,
      releaseMs: 250,
      applyGainAtInputRate: true,
    });
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('treats an omitted releaseMs as the library default', () => {
    // A target the peak headroom cannot reach, so the limiter is doing the work
    // and its release time is visible in the output. Without that the
    // results below would agree because nothing reads releaseMs at all, and the
    // equality would hold for a shim that dropped the argument entirely.
    const x = sine(220, 0.1);
    const driven = { targetLufs: 0, ceilingDbtp: -6 };
    const omitted = masteringNormalizeLoudness(x, SR, driven);
    const explicit = masteringNormalizeLoudness(x, SR, { ...driven, releaseMs: 50 });
    const immediate = masteringNormalizeLoudness(x, SR, { ...driven, releaseMs: 1 });
    expect(Array.from(omitted.samples)).toEqual(Array.from(explicit.samples));
    // The varying control: releaseMs reaches the limiter on this fixture.
    expect(Array.from(omitted.samples)).not.toEqual(Array.from(immediate.samples));
  });

  it('refuses the releaseMs values it cannot honour instead of ignoring them', () => {
    // The shim used to apply releaseMs only when it was positive, so a negative
    // or non-finite request silently ran the library default and the caller
    // never learned its value had been discarded. This is the same refusal set
    // the Node facade and the C ABI parameter map reach, which is the point:
    // the value is refused by one shared core validator, not per surface.
    const x = sine(220, 0.1);
    for (const releaseMs of [0, -1, -0.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: -14, releaseMs })).toThrow();
    }
    // The controls. A shim that had started refusing every releaseMs would pass
    // every assertion above and fail only here.
    for (const releaseMs of [1, 50, 250]) {
      expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: -14, releaseMs })).not.toThrow();
    }
  });

  it('rejects non-finite targets and accepts a later finite call', () => {
    const x = sine(220, 0.1);
    expect(() => masteringNormalizeLoudness(x, SR, { targetLufs: Number.NaN })).toThrow();
    expect(() =>
      masteringNormalizeLoudness(x, SR, { ceilingDbtp: Number.POSITIVE_INFINITY }),
    ).toThrow();
    const recovered = masteringNormalizeLoudness(x, SR, { targetLufs: -14, ceilingDbtp: -1 });
    expect(allFinite(recovered.samples)).toBe(true);
  });

  it('takes the true-peak ceiling as ceilingDbtp', () => {
    const x = sine(220, 0.25);
    const peakOf = (ceilingDbtp: number) =>
      Math.max(...masteringNormalizeLoudness(x, SR, { targetLufs: 0, ceilingDbtp }).samples);
    expect(peakOf(-6)).toBeLessThan(peakOf(-1));
    expect(() => masteringNormalizeLoudness(x, SR, { ceilingDb: -1 } as never)).toThrow(
      unknownKeyText('masteringNormalizeLoudness: unknown key ceilingDb'),
    );
  });
});
