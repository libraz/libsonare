import { describe, expect, it } from 'vitest';
import { normalize, trim } from '../src/index.js';

const SAMPLE_RATE = 22050;

function trimInput(): Float32Array {
  const samples = new Float32Array(12000);
  for (let i = 2400; i < 7200; i++) {
    samples[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE);
  }
  return samples;
}

describe('normalize and trim option extensions', () => {
  it('preserves peak normalization by default', () => {
    const samples = new Float32Array([0.1, -0.25, 0.2, -0.05]);
    expect(normalize(samples, SAMPLE_RATE)).toEqual(
      normalize(samples, SAMPLE_RATE, { targetDb: 0, mode: 'peak' }),
    );
    expect(normalize({ samples, sampleRate: SAMPLE_RATE })).toEqual(
      normalize({ samples, sampleRate: SAMPLE_RATE, mode: 'peak' }),
    );
  });

  it('uses RMS normalization when requested', () => {
    const samples = new Float32Array([0.1, -0.25, 0.2, -0.05]);
    const peak = normalize(samples, SAMPLE_RATE);
    const rms = normalize({ samples, sampleRate: SAMPLE_RATE, targetDb: -10, mode: 'rms' });
    expect(Array.from(rms)).not.toEqual(Array.from(peak));
  });

  it('preserves default trim framing and forwards custom framing', () => {
    const samples = trimInput();
    expect(trim(samples, SAMPLE_RATE, { thresholdDb: -60 })).toEqual(
      trim(samples, SAMPLE_RATE, { thresholdDb: -60, frameLength: 2048, hopLength: 512 }),
    );
    expect(
      trim(samples, SAMPLE_RATE, { thresholdDb: -60, frameLength: 256, hopLength: 64 }).length,
    ).not.toBe(trim(samples, SAMPLE_RATE, { thresholdDb: -60 }).length);
  });

  it('rejects invalid normalize modes and trim framing before native calls', () => {
    const samples = trimInput();
    expect(() => normalize({ samples, sampleRate: SAMPLE_RATE, mode: true as never })).toThrow(
      TypeError,
    );
    expect(() => normalize({ samples, sampleRate: SAMPLE_RATE, mode: 'RMS' as never })).toThrow(
      RangeError,
    );
    expect(() => normalize(samples, SAMPLE_RATE, { targetDb: 0, mode: null as never })).toThrow(
      TypeError,
    );
    expect(() =>
      trim(samples, SAMPLE_RATE, { thresholdDb: -60, frameLength: 0, hopLength: 512 }),
    ).toThrow(RangeError);
    // A fractional frame length is a number out of domain, which is the same
    // side of the split as the `0` above rather than the `null` mode before it.
    expect(() =>
      trim(samples, SAMPLE_RATE, { thresholdDb: -60, frameLength: 1.5, hopLength: 512 }),
    ).toThrow(RangeError);
    expect(() =>
      trim(samples, SAMPLE_RATE, { thresholdDb: -60, frameLength: 1024, hopLength: 0 }),
    ).toThrow(RangeError);
  });
});
