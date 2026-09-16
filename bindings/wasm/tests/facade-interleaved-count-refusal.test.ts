/**
 * The facade refuses a channel count and a bucket width, and refuses them the
 * way its sibling surface does.
 *
 * This surface already refused these values, but a step lower down and under a
 * different class: the narrowing reader answered `SonareError: channels must be
 * an integer` with no entry point in the message. Its own neighbours did not —
 * `lufsInterleaved` routes through `assertInterleavedSamples` and answers a
 * `RangeError` naming the function. Two entry points doing the same job refused
 * a caller two different ways, which is what these cases pin shut.
 *
 * Each entry point is first shown to consume the argument: two legitimate counts
 * select reports that differ. An entry point that ignored its count would refuse
 * the values below for reasons that say nothing about the count.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, waveformPeakPyramid, waveformPeaks } from '../src/index';

const LENGTH = 100;

function fixture(): Float32Array {
  const samples = new Float32Array(LENGTH);
  for (let i = 0; i < LENGTH; i++) {
    samples[i] = Math.sin(i * 0.37);
  }
  return samples;
}

beforeAll(async () => {
  await init();
});

describe('waveformPeaks', () => {
  it('answers differently for two legitimate channel counts', () => {
    const samples = fixture();
    const two = waveformPeaks(samples, 2, { samplesPerBucket: 10 });
    const four = waveformPeaks(samples, 4, { samplesPerBucket: 10 });
    expect(two.channels).toBe(2);
    expect(four.channels).toBe(4);
    expect(two.min.length).not.toBe(four.min.length);
  });

  it('refuses a fractional channel count in the facade, naming the entry point', () => {
    // 100 % 2.5 === 0, so the length rule alone lets this through.
    expect(LENGTH % 2.5).toBe(0);
    expect(() => waveformPeaks(fixture(), 2.5, { samplesPerBucket: 10 })).toThrow(
      /waveformPeaks: channels must be a positive integer/,
    );
  });

  it('refuses a channel count at and below zero', () => {
    expect(() => waveformPeaks(fixture(), 0, { samplesPerBucket: 10 })).toThrow(RangeError);
    expect(() => waveformPeaks(fixture(), -1, { samplesPerBucket: 10 })).toThrow(RangeError);
  });

  it('refuses a fractional bucket width', () => {
    expect(() => waveformPeaks(fixture(), 2, { samplesPerBucket: 10.5 })).toThrow(
      /waveformPeaks: samplesPerBucket must be a positive integer/,
    );
  });
});

describe('waveformPeakPyramid', () => {
  it('answers differently for two legitimate level sets', () => {
    const samples = fixture();
    const shallow = waveformPeakPyramid(samples, 2, { samplesPerBucketLevels: [5] });
    const deep = waveformPeakPyramid(samples, 2, { samplesPerBucketLevels: [5, 10] });
    expect(shallow.length).toBe(1);
    expect(deep.length).toBe(2);
  });

  it('names the level it refused rather than the list', () => {
    expect(() => waveformPeakPyramid(fixture(), 2, { samplesPerBucketLevels: [10, 7.5] })).toThrow(
      /samplesPerBucketLevels\[1\] must be a positive integer/,
    );
  });

  it('still refuses an empty level set', () => {
    expect(() => waveformPeakPyramid(fixture(), 2, { samplesPerBucketLevels: [] })).toThrow(
      /must not be empty/,
    );
  });
});
