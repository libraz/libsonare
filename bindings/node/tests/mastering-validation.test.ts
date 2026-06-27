/**
 * The offline mastering entry points validate their input in the C++ core
 * (MasteringChain::process_mono/process_stereo), so the Node wrappers inherit
 * the same empty / out-of-range-rate / non-finite rejection as the C ABI and
 * Python instead of silently producing an empty or garbage result.
 */

import { describe, expect, it } from 'vitest';
import { masterAudio, masteringChain, masteringChainStereo } from '../src/index';

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
});
