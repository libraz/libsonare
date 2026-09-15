import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  isSonareError,
  lufs,
  masteringAbMatchLoudness,
  meteringTruePeakDb,
  type SonareError,
} from '../src/index.js';

const SR = 44100;

const sine = (frequency: number, seconds: number, amplitude: number): Float32Array => {
  const samples = new Float32Array(Math.round(SR * seconds));
  for (let i = 0; i < samples.length; i++) {
    samples[i] = amplitude * Math.sin((2 * Math.PI * frequency * i) / SR);
  }
  return samples;
};

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

// The two takes differ in tone AND in duration, so a call that returned the
// reference — or measured the wrong one of the pair — fails on length before
// any tolerance is consulted.
const source = sine(440, 1.0, 0.1);
const reference = sine(997, 1.7, 0.5);

describe('masteringAbMatchLoudness', () => {
  it('returns the source at the reference loudness', () => {
    const matched = masteringAbMatchLoudness({ source, reference, sampleRate: SR });

    expect(matched.samples).toBeInstanceOf(Float32Array);
    expect(matched.samples.length).toBe(source.length);
    expect(matched.samples.length).not.toBe(reference.length);
    expect(matched.sampleRate).toBe(SR);

    // Re-measure the returned audio rather than trusting the reported scalars.
    const referenceLufs = lufs(reference, SR).integratedLufs;
    const matchedLufs = lufs(matched.samples, SR).integratedLufs;
    expect(matchedLufs).toBeCloseTo(referenceLufs, 1);

    // ...and that it is the SOURCE carrying that loudness: a pure gain, so the
    // matched buffer is the source scaled sample for sample.
    const gain = 10 ** (matched.appliedGainDb / 20);
    let maxDeviation = 0;
    for (let i = 0; i < source.length; i++) {
      maxDeviation = Math.max(maxDeviation, Math.abs(matched.samples[i] - source[i] * gain));
    }
    expect(maxDeviation).toBeLessThan(1e-3);
  });

  it('reports the loudness pair and the post-gain true peak it left', () => {
    const matched = masteringAbMatchLoudness({ source, reference, sampleRate: SR });

    expect(matched.referenceLufs).toBeCloseTo(lufs(reference, SR).integratedLufs, 3);
    expect(matched.sourceLufs).toBeCloseTo(lufs(source, SR).integratedLufs, 3);
    expect(matched.appliedGainDb).toBeCloseTo(
      lufs(reference, SR).integratedLufs - lufs(source, SR).integratedLufs,
      2,
    );
    // The gain is not clamped, so the reported peak is the only headroom signal.
    expect(matched.matchedTruePeakDbtp).toBeCloseTo(meteringTruePeakDb(matched.samples, SR), 2);
    expect(matched.matchedTruePeakDbtp).toBeGreaterThan(meteringTruePeakDb(source, SR));
  });

  it('defaults sampleRate to 22050 when the request omits it', () => {
    const matched = masteringAbMatchLoudness({ source, reference });
    expect(matched.sampleRate).toBe(22050);
  });

  it('rejects an invalid sample rate with a SonareError', () => {
    const caught = capture(() => masteringAbMatchLoudness({ source, reference, sampleRate: 0 }));
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
  });

  it('rejects a non-finite sample in the reference', () => {
    const broken = Float32Array.from(reference);
    broken[1000] = Number.NaN;
    const caught = capture(() =>
      masteringAbMatchLoudness({ source, reference: broken, sampleRate: SR }),
    );
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
  });
});
