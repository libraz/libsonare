/**
 * A channel count and a bucket width are refused here or nowhere.
 *
 * The addon narrows both into a C integer with `node_narrow_int`, which
 * deliberately truncates rather than refuses — `sonare_wrap_options.h` says so,
 * and says a fractional check is an addition the facade has to make. So `2.5`
 * divides a 100-sample buffer exactly, passes the length rule, reaches the addon
 * as `2`, and comes back as a successful two-channel report. Nothing downstream
 * can tell that answer from one the caller asked for.
 *
 * Each entry point is first shown to consume the argument: two legitimate counts
 * select reports that differ. An entry point that ignored its count would refuse
 * the values below for reasons that say nothing about the count.
 *
 * The low end and the fraction are both driven. A high-end value is not the
 * interesting case here: the narrowing truncates toward zero, so what it folds
 * onto a legal value is a fraction, not an overflow.
 */

import { describe, expect, it } from 'vitest';
import {
  meteringSpectrumFrame,
  meteringTruePeakDb,
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
  it('answers differently for two legitimate channel counts', () => {
    const samples = fixture();
    const two = waveformPeaks(samples, 2, { samplesPerBucket: 10 });
    const four = waveformPeaks(samples, 4, { samplesPerBucket: 10 });
    expect(two.channels).toBe(2);
    expect(four.channels).toBe(4);
    expect(two.min.length).not.toBe(four.min.length);
  });

  it('refuses a fractional channel count that divides the buffer', () => {
    // 100 % 2.5 === 0, so the length rule alone lets this through.
    expect(LENGTH % 2.5).toBe(0);
    expect(() => waveformPeaks(fixture(), 2.5, { samplesPerBucket: 10 })).toThrow(
      /channels must be an integer/,
    );
  });

  it('refuses a channel count at and below zero, naming the sign rather than the type', () => {
    // The other half of the split the fractional case above asserts: 2.5 is
    // positive and is told it must be an integer, while these are integers and
    // are told they must be positive. One message for both would name a
    // property one of the two values already has.
    expect(() => waveformPeaks(fixture(), 0, { samplesPerBucket: 10 })).toThrow(
      /channels must be a positive integer/,
    );
    expect(() => waveformPeaks(fixture(), -1, { samplesPerBucket: 10 })).toThrow(
      /channels must be a positive integer/,
    );
  });

  it('answers differently for two legitimate bucket widths', () => {
    const samples = fixture();
    const narrow = waveformPeaks(samples, 2, { samplesPerBucket: 5 });
    const wide = waveformPeaks(samples, 2, { samplesPerBucket: 10 });
    expect(narrow.min.length).not.toBe(wide.min.length);
  });

  it('refuses a fractional bucket width', () => {
    expect(() => waveformPeaks(fixture(), 2, { samplesPerBucket: 10.5 })).toThrow(
      /samplesPerBucket must be an integer/,
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

  it('refuses a fractional channel count that divides the buffer', () => {
    expect(() => waveformPeakPyramid(fixture(), 2.5, { samplesPerBucketLevels: [10] })).toThrow(
      /channels must be an integer/,
    );
  });

  it('names the level it refused rather than the list', () => {
    expect(() => waveformPeakPyramid(fixture(), 2, { samplesPerBucketLevels: [10, 7.5] })).toThrow(
      /samplesPerBucketLevels\[1\] must be an integer/,
    );
  });

  it('still refuses an empty level set', () => {
    expect(() => waveformPeakPyramid(fixture(), 2, { samplesPerBucketLevels: [] })).toThrow(
      /must not be empty/,
    );
  });
});

/**
 * A field whose `0` means "use the library default" is the worst case of the
 * truncation above, and the addon's own header says so: anything the narrowing
 * lands inside `(-1, 1)` selects the default and reports success, which is a
 * category change rather than a rounded magnitude. Each value below was
 * measured selecting the sentinel before these checks existed.
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

  it('meteringTruePeakDb honours its oversample factor', () => {
    // Without this the refusal below could be of an argument nothing reads.
    expect(meteringTruePeakDb(tone, 22050, 4)).not.toBe(meteringTruePeakDb(tone, 22050, 16));
  });

  it('meteringTruePeakDb refuses a factor that would truncate onto its 0 default', () => {
    expect(() => meteringTruePeakDb(tone, 22050, 0.5)).toThrow(
      /oversampleFactor must be a non-negative integer/,
    );
  });

  it('meteringVectorscope decimates to the count it is given, and 0 still means all', () => {
    expect(meteringVectorscope(tone, right, 22050, { maxPoints: 64 }).mid.length).toBe(64);
    expect(meteringVectorscope(tone, right, 22050, { maxPoints: 0 }).mid.length).toBe(tone.length);
  });

  it('meteringVectorscope refuses a negative or fractional point budget', () => {
    for (const maxPoints of [-1, 0.5]) {
      expect(() => meteringVectorscope(tone, right, 22050, { maxPoints })).toThrow(
        /maxPoints must be a non-negative integer/,
      );
    }
  });

  it('meteringSpectrumFrame refuses a fractional frame offset', () => {
    expect(() => meteringSpectrumFrame(tone, 22050, 100.5)).toThrow(
      /frameOffset must be a non-negative integer/,
    );
  });
});

describe('pcen', () => {
  it('reads the matrix shape it is given', () => {
    expect(pcen(new Float32Array(100), 2, 50).length).toBe(100);
  });

  it('refuses a fractional bin count that divides the buffer once truncated', () => {
    // The addon checks `nBins * nFrames === length` AFTER narrowing, so 2.5 x 50
    // over 100 values passes that rule as 2 x 50.
    expect(() => pcen(new Float32Array(100), 2.5, 50)).toThrow(/nBins must be an integer/);
  });
});
