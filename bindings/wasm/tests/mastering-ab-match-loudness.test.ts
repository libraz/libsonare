/**
 * `masteringAbMatchLoudness` on the WASM surface.
 *
 * The two takes deliberately differ in BOTH tone frequency and duration, so a
 * call that returned the reference — or the wrong member of the core's pair —
 * fails on length and on the sample-wise gain identity rather than passing on a
 * loudness number that would agree either way. Every reported scalar is checked
 * against an independent re-measurement of the returned audio.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, lufs, masteringAbMatchLoudness, meteringTruePeakDb } from '../dist/index.js';

const SR = 48000;

function sine(freq: number, durationSec: number, amp: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

// Quiet 3 s at 440 Hz against a loud 2 s at 1 kHz: different length, different
// tone, and roughly 20 dB apart.
const source = sine(440, 3.0, 0.05);
const reference = sine(1000, 2.0, 0.5);

describe('masteringAbMatchLoudness (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns the source at the reference loudness, not the reference', () => {
    const result = masteringAbMatchLoudness({ source, reference, sampleRate: SR });

    expect(result.sampleRate).toBe(SR);
    expect(result.samples.length).toBe(source.length);
    expect(result.samples.length).not.toBe(reference.length);
    expect(result.appliedGainDb).toBeGreaterThan(10);

    // The returned audio is the source scaled by the reported gain, sample for
    // sample. A returned reference would fail this even at the same loudness.
    const gain = 10 ** (result.appliedGainDb / 20);
    let maxDeviation = 0;
    for (let i = 0; i < source.length; i++) {
      const expected = (source[i] ?? 0) * gain;
      maxDeviation = Math.max(maxDeviation, Math.abs((result.samples[i] ?? 0) - expected));
    }
    expect(maxDeviation).toBeLessThan(1e-4);

    // Re-measure rather than trusting the reported LUFS: the match is only real
    // if the returned audio now reads at the reference's integrated loudness.
    const matchedLufs = lufs(result.samples, SR).integratedLufs;
    const referenceLufs = lufs(reference, SR).integratedLufs;
    const sourceLufs = lufs(source, SR).integratedLufs;
    expect(matchedLufs).toBeCloseTo(referenceLufs, 1);
    expect(matchedLufs).not.toBeCloseTo(sourceLufs, 1);

    expect(result.referenceLufs).toBeCloseTo(referenceLufs, 2);
    expect(result.sourceLufs).toBeCloseTo(sourceLufs, 2);
    expect(result.matchedTruePeakDbtp).toBeCloseTo(meteringTruePeakDb(result.samples, SR), 2);
  });

  it('reports a non-finite loudness and no gain for a silent take', () => {
    const silent = new Float32Array(SR);
    const result = masteringAbMatchLoudness({ source: silent, reference, sampleRate: SR });

    expect(Number.isFinite(result.sourceLufs)).toBe(false);
    expect(result.appliedGainDb).toBe(0);
    expect(result.samples.length).toBe(silent.length);
  });

  it('rejects an empty buffer and a non-finite sample', () => {
    expect(() =>
      masteringAbMatchLoudness({ source: new Float32Array(0), reference, sampleRate: SR }),
    ).toThrow();

    const withNan = sine(440, 0.5, 0.1);
    withNan[100] = Number.NaN;
    expect(() =>
      masteringAbMatchLoudness({ source: withNan, reference, sampleRate: SR }),
    ).toThrow();

    expect(() => masteringAbMatchLoudness({ source, reference, sampleRate: 0 })).toThrow();
  });
});
