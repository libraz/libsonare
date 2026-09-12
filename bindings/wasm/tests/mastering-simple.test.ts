/**
 * The simple WASM `mastering()` helper is a thin embind shim over
 * mastering::maximizer::loudness_optimize. This verifies the marshaling of the
 * appended maximizer knobs (releaseMs / applyGainAtInputRate): they are accepted,
 * produce valid length-preserving output, and a zero/omitted release reproduces
 * the library default (so older callers see no behavior change).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, mastering } from '../src/index';

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

describe('simple mastering() knobs (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('accepts releaseMs / applyGainAtInputRate and returns valid output', () => {
    const x = sine(220, 0.1);
    const r = mastering(x, SR, { targetLufs: -14, releaseMs: 250, applyGainAtInputRate: true });
    expect(r.samples.length).toBe(x.length);
    expect(allFinite(r.samples)).toBe(true);
  });

  it('treats releaseMs 0 (or omitted) as the library default', () => {
    // A target the peak headroom cannot reach, so the limiter is doing the work
    // and its release time is visible in the output. Without that the three
    // results below would agree because nothing reads releaseMs at all, and the
    // equality would hold for a shim that dropped the argument entirely.
    const x = sine(220, 0.1);
    const driven = { targetLufs: 0, ceilingDb: -6 };
    const omitted = mastering(x, SR, driven);
    const zero = mastering(x, SR, { ...driven, releaseMs: 0 });
    const explicit = mastering(x, SR, { ...driven, releaseMs: 50 });
    const immediate = mastering(x, SR, { ...driven, releaseMs: 1 });
    expect(Array.from(zero.samples)).toEqual(Array.from(omitted.samples));
    expect(Array.from(zero.samples)).toEqual(Array.from(explicit.samples));
    // The varying control: releaseMs reaches the limiter on this fixture.
    expect(Array.from(zero.samples)).not.toEqual(Array.from(immediate.samples));
  });

  it('refuses the releaseMs values it cannot honour instead of ignoring them', () => {
    // The shim used to apply releaseMs only when it was positive, so a negative
    // or non-finite request silently ran the library default and the caller
    // never learned its value had been discarded. This is the same refusal set
    // the Node facade and the C ABI parameter map reach, which is the point:
    // the value is refused by one shared core validator, not per surface.
    const x = sine(220, 0.1);
    for (const releaseMs of [-1, -0.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => mastering(x, SR, { targetLufs: -14, releaseMs })).toThrow();
    }
    // The controls. A shim that had started refusing every releaseMs would pass
    // every assertion above and fail only here.
    for (const releaseMs of [0, 1, 50, 250]) {
      expect(() => mastering(x, SR, { targetLufs: -14, releaseMs })).not.toThrow();
    }
  });

  it('rejects non-finite targets and accepts a later finite call', () => {
    const x = sine(220, 0.1);
    expect(() => mastering(x, SR, { targetLufs: Number.NaN })).toThrow();
    expect(() => mastering(x, SR, { ceilingDb: Number.POSITIVE_INFINITY })).toThrow();
    const recovered = mastering(x, SR, { targetLufs: -14, ceilingDb: -1 });
    expect(allFinite(recovered.samples)).toBe(true);
  });
});
