/**
 * The waveform facade refuses a bucket width and a malformed plane list the way
 * its sibling surface does, naming the entry point and the field.
 *
 * Each entry point is first shown to consume the argument: two legitimate values
 * select reports that differ. An entry point that ignored its argument would
 * refuse the values below for reasons that say nothing about it.
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
  it('answers differently for two legitimate plane counts', () => {
    const two = waveformPeaks({ channels: [fixture(), fixture()], samplesPerBucket: 10 });
    const four = waveformPeaks({
      channels: [fixture(), fixture(), fixture(), fixture()],
      samplesPerBucket: 10,
    });
    expect(two.min).toHaveLength(2);
    expect(four.min).toHaveLength(4);
  });

  it('refuses planes of unequal length, naming the entry point', () => {
    expect(() =>
      waveformPeaks({ channels: [fixture(), fixture().subarray(0, 50)], samplesPerBucket: 10 }),
    ).toThrow(/waveformPeaks: channels must all have the same length/);
  });

  it('refuses an empty plane list and a non-array, naming the field', () => {
    expect(() => waveformPeaks({ channels: [] })).toThrow(
      /waveformPeaks: channels must not be empty/,
    );
    expect(() => waveformPeaks({ channels: fixture() as never })).toThrow(
      /waveformPeaks: channels must be an array/,
    );
  });

  it('refuses a fractional bucket width', () => {
    expect(() => waveformPeaks({ channels: [fixture()], samplesPerBucket: 10.5 })).toThrow(
      /waveformPeaks: samplesPerBucket must be an integer/,
    );
  });
});

describe('waveformPeakPyramid', () => {
  it('answers differently for two legitimate level sets', () => {
    const shallow = waveformPeakPyramid({
      channels: [fixture()],
      samplesPerBucketLevels: [5],
    });
    const deep = waveformPeakPyramid({ channels: [fixture()], samplesPerBucketLevels: [5, 10] });
    expect(shallow.length).toBe(1);
    expect(deep.length).toBe(2);
  });

  it('names the level it refused rather than the list', () => {
    expect(() =>
      waveformPeakPyramid({ channels: [fixture()], samplesPerBucketLevels: [10, 7.5] }),
    ).toThrow(/samplesPerBucketLevels\[1\] must be an integer/);
  });

  it('still refuses an empty level set', () => {
    expect(() =>
      waveformPeakPyramid({ channels: [fixture()], samplesPerBucketLevels: [] }),
    ).toThrow(/must not be empty/);
  });
});
