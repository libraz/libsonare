import { describe, expect, it } from 'vitest';
import { splitSilence, splitSilenceCommon } from '../src/index.js';

const SR = 22050;
const DURATION_SAMPLES = SR; // one second
const TOP_DB = 60;
const FRAME_LENGTH = 2048;
const HOP_LENGTH = 512;

/** A one-second buffer that sounds only in [startSample, endSample). */
function toneBurst(startSample: number, endSample: number, frequency = 440): Float32Array {
  const samples = new Float32Array(DURATION_SAMPLES);
  for (let i = startSample; i < endSample; i++) {
    samples[i] = Math.sin((2 * Math.PI * frequency * i) / SR);
  }
  return samples;
}

function toPairs(intervals: Int32Array): Array<[number, number]> {
  const pairs: Array<[number, number]> = [];
  for (let i = 0; i < intervals.length; i += 2) {
    pairs.push([intervals[i], intervals[i + 1]]);
  }
  return pairs;
}

/** Sorts and merges touching/overlapping pairs, mirroring the C ABI's own merge rule. */
function mergeTouchingPairs(pairs: Array<[number, number]>): Array<[number, number]> {
  const sorted = [...pairs].sort((a, b) => a[0] - b[0]);
  const merged: Array<[number, number]> = [];
  for (const [start, end] of sorted) {
    const last = merged[merged.length - 1];
    if (last !== undefined && start <= last[1]) {
      last[1] = Math.max(last[1], end);
    } else {
      merged.push([start, end]);
    }
  }
  return merged;
}

function flattenPairs(pairs: Array<[number, number]>): Int32Array {
  const flat = new Int32Array(pairs.length * 2);
  pairs.forEach(([start, end], i) => {
    flat[i * 2] = start;
    flat[i * 2 + 1] = end;
  });
  return flat;
}

describe('splitSilenceCommon', () => {
  // Two non-touching takes: A sounds early, B sounds later, so their split-
  // silence intervals never merge and the union/intersection distinction is
  // directly observable.
  const signalA = toneBurst(0, Math.floor(0.2 * SR));
  const signalB = toneBurst(Math.floor(0.5 * SR), Math.floor(0.7 * SR));

  it('matches splitSilence exactly for a single signal', () => {
    const expected = splitSilence(signalA, TOP_DB, FRAME_LENGTH, HOP_LENGTH);
    const actual = splitSilenceCommon({
      signals: [signalA],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    expect(actual).toEqual(expected);
  });

  it('returns the union of each signal, not their intersection', () => {
    const pairsA = toPairs(splitSilence(signalA, TOP_DB, FRAME_LENGTH, HOP_LENGTH));
    const pairsB = toPairs(splitSilence(signalB, TOP_DB, FRAME_LENGTH, HOP_LENGTH));
    const expected = flattenPairs(mergeTouchingPairs([...pairsA, ...pairsB]));

    const actual = splitSilenceCommon({
      signals: [signalA, signalB],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    expect(actual).toEqual(expected);

    // B's own sounding interval survives untouched even though A is silent
    // there -- direct evidence this is a union, not an intersection.
    const actualPairs = toPairs(actual);
    for (const pair of pairsB) {
      expect(actualPairs).toContainEqual(pair);
    }
  });

  it('is order-independent', () => {
    const forward = splitSilenceCommon({
      signals: [signalA, signalB],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    const reversed = splitSilenceCommon({
      signals: [signalB, signalA],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    expect(reversed).toEqual(forward);
  });

  it('needs no padding for a shorter signal cut before its sound starts', () => {
    // Ends before signalB's onset, so this contributes nothing but silence.
    const truncatedBeforeSound = signalB.slice(0, Math.floor(0.4 * SR));
    const expected = splitSilence(signalA, TOP_DB, FRAME_LENGTH, HOP_LENGTH);
    const actual = splitSilenceCommon({
      signals: [signalA, truncatedBeforeSound],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    expect(actual).toEqual(expected);
  });

  it('succeeds with an empty result when every signal is silent', () => {
    const silence = new Float32Array(DURATION_SAMPLES);
    const actual = splitSilenceCommon({
      signals: [silence, silence],
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    });
    expect(actual).toBeInstanceOf(Int32Array);
    expect(actual.length).toBe(0);
  });

  it('rejects an empty signals array, naming the field', () => {
    expect(() =>
      splitSilenceCommon({
        signals: [],
        topDb: TOP_DB,
        frameLength: FRAME_LENGTH,
        hopLength: HOP_LENGTH,
      }),
    ).toThrow(/signals/);
  });

  it('rejects a non-Float32Array element, naming the field', () => {
    expect(() =>
      splitSilenceCommon({
        signals: [signalA, [1, 2, 3] as unknown as Float32Array],
        topDb: TOP_DB,
        frameLength: FRAME_LENGTH,
        hopLength: HOP_LENGTH,
      }),
    ).toThrow(/signals/);
  });
});
