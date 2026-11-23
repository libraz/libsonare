import { describe, expect, it } from 'vitest';
import { isMeterSnapshot } from '../src/worklet/guards';

function meterSnapshot(inputPeakDb: unknown[] = [-3, -6]): Record<string, unknown> {
  return {
    type: 'meter',
    targetId: 0,
    frame: 128,
    numChannels: 2,
    peakDb: [-3, -6],
    rmsDb: [-18, -18],
    truePeakDbtp: [-2.5, -5.5],
    inputPeakDb,
    correlation: 1,
  };
}

describe('isMeterSnapshot per-plane values', () => {
  it('accepts unmeasured input peak planes as undefined', () => {
    expect(isMeterSnapshot(meterSnapshot([undefined, undefined]))).toBe(true);
  });

  it('accepts finite and silent input peak planes', () => {
    expect(isMeterSnapshot(meterSnapshot())).toBe(true);
    expect(
      isMeterSnapshot(meterSnapshot([Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY])),
    ).toBe(true);
  });

  it.each([null, '-3', true, {}, [-3], Number.NaN, Number.POSITIVE_INFINITY])(
    'rejects an input peak plane that is not a level (%p)',
    (inputPeak) => {
      expect(isMeterSnapshot(meterSnapshot([inputPeak, -6]))).toBe(false);
    },
  );

  it('rejects per-plane arrays whose lengths differ from numChannels', () => {
    expect(isMeterSnapshot({ ...meterSnapshot(), peakDb: [-3], rmsDb: [-18, -18] })).toBe(false);
  });

  it('requires all four per-plane arrays', () => {
    const withoutInputPeak = meterSnapshot();
    delete withoutInputPeak.inputPeakDb;
    expect(isMeterSnapshot(withoutInputPeak)).toBe(false);
  });

  it('rejects a plane array containing NaN', () => {
    expect(isMeterSnapshot({ ...meterSnapshot(), truePeakDbtp: [Number.NaN, -5.5] })).toBe(false);
  });
});
