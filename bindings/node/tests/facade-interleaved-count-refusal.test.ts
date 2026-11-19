/**
 * A bucket width and a plane list are refused here or nowhere.
 *
 * The addon narrows the bucket width into a C integer with `node_narrow_int`,
 * which deliberately truncates rather than refuses, so a fractional width has to
 * be refused by the facade. The channels arrive planar; the facade interleaves
 * them, so a malformed plane list is refused before the addon sees it.
 *
 * Each entry point is first shown to consume the argument: two legitimate values
 * select reports that differ. An entry point that ignored its argument would
 * refuse the values below for reasons that say nothing about it.
 */

import { describe, expect, it } from 'vitest';
import {
  meteringSpectrumFrame,
  meteringTruePeakDbtp,
  meteringVectorscope,
  pcen,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index.js';

const LENGTH = 100;

function fixture(): Float32Array {
  const samples = new Float32Array(LENGTH);
  for (let i = 0; i < LENGTH; i++) {
    samples[i] = Math.sin(i * 0.37);
  }
  return samples;
}

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

  it('answers differently for two legitimate bucket widths', () => {
    const narrow = waveformPeaks({ channels: [fixture(), fixture()], samplesPerBucket: 5 });
    const wide = waveformPeaks({ channels: [fixture(), fixture()], samplesPerBucket: 10 });
    expect(narrow.bucketCount).not.toBe(wide.bucketCount);
  });

  it('refuses a fractional bucket width', () => {
    expect(() => waveformPeaks({ channels: [fixture()], samplesPerBucket: 10.5 })).toThrow(
      /samplesPerBucket must be an integer/,
    );
  });
});

describe('waveformPeakPyramid', () => {
  it('answers differently for two legitimate level sets', () => {
    const shallow = waveformPeakPyramid({ channels: [fixture()], samplesPerBucketLevels: [5] });
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

/**
 * A field whose `0` selects the library default in the addon is the worst case of
 * truncation: anything the narrowing lands inside `(-1, 1)` selects the default and
 * reports success, which is a category change rather than a rounded magnitude. The
 * facade refuses an explicit `0` and any fraction.
 */
describe('a sentinel-bearing count', () => {
  const tone = (() => {
    const samples = new Float32Array(4096);
    for (let i = 0; i < samples.length; i++) {
      samples[i] = Math.sin(i * 0.05) * 0.9;
    }
    return samples;
  })();
  const right = tone.map((v) => v * 0.8);

  it('meteringTruePeakDbtp honours its oversample factor', () => {
    // Without this the refusal below could be of an argument nothing reads.
    expect(meteringTruePeakDbtp(tone, 22050, { oversampleFactor: 4 })).not.toBe(
      meteringTruePeakDbtp(tone, 22050, { oversampleFactor: 16 }),
    );
  });

  it('meteringTruePeakDbtp refuses a factor that would truncate onto its 0 default', () => {
    expect(() => meteringTruePeakDbtp(tone, 22050, { oversampleFactor: 0.5 })).toThrow(
      /oversampleFactor must be an integer in \[0, 9007199254740991\]/,
    );
  });

  it('meteringVectorscope decimates to the count it is given, and refuses 0 by name', () => {
    const request = { left: tone, right, sampleRate: 22050 };
    expect(meteringVectorscope({ ...request, maxPoints: 64 }).mid.length).toBe(64);
    expect(meteringVectorscope(request).mid.length).toBe(tone.length);
    expect(() => meteringVectorscope({ ...request, maxPoints: 0 })).toThrow(
      /maxPoints must not be 0/,
    );
  });

  it('meteringVectorscope refuses a negative or fractional point budget', () => {
    for (const maxPoints of [-1, 0.5]) {
      expect(() =>
        meteringVectorscope({ left: tone, right, sampleRate: 22050, maxPoints }),
      ).toThrow(/maxPoints must be an integer in \[0, 9007199254740991\]/);
    }
  });

  it('meteringSpectrumFrame refuses a fractional start sample', () => {
    expect(() => meteringSpectrumFrame(tone, 22050, { startSample: 100.5 })).toThrow(
      /startSample must be an integer in \[0, 9007199254740991\]/,
    );
  });
});

describe('pcen', () => {
  it('reads the matrix shape it is given', () => {
    expect(pcen({ values: { data: new Float32Array(100), rows: 2, cols: 50 } }).data.length).toBe(
      100,
    );
  });

  it('refuses a fractional bin count that divides the buffer once truncated', () => {
    // The addon checks `nBins * nFrames === length` AFTER narrowing, so 2.5 x 50
    // over 100 values passes that rule as 2 x 50.
    expect(() => pcen({ values: { data: new Float32Array(100), rows: 2.5, cols: 50 } })).toThrow(
      /rows must be an integer/,
    );
  });
});
