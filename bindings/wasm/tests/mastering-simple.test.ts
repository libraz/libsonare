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
    const x = sine(220, 0.1);
    const omitted = mastering(x, SR, { targetLufs: -14 });
    const zero = mastering(x, SR, { targetLufs: -14, releaseMs: 0 });
    const explicit = mastering(x, SR, { targetLufs: -14, releaseMs: 50 });
    expect(Array.from(zero.samples)).toEqual(Array.from(omitted.samples));
    expect(Array.from(zero.samples)).toEqual(Array.from(explicit.samples));
  });

  it('rejects non-finite targets and accepts a later finite call', () => {
    const x = sine(220, 0.1);
    expect(() => mastering(x, SR, { targetLufs: Number.NaN })).toThrow();
    expect(() => mastering(x, SR, { ceilingDb: Number.POSITIVE_INFINITY })).toThrow();
    const recovered = mastering(x, SR, { targetLufs: -14, ceilingDb: -1 });
    expect(allFinite(recovered.samples)).toBe(true);
  });
});
