import { describe, expect, it } from 'vitest';
import { isMeterSnapshot } from '../src/worklet/guards';

function meterSnapshotWithoutInputPeaks(): Record<string, unknown> {
  return {
    type: 'meter',
    targetId: 0,
    frame: 128,
    peakDbL: -3,
    peakDbR: -6,
    rmsDbL: -18,
    rmsDbR: -18,
    correlation: 1,
  };
}

function meterSnapshot(): Record<string, unknown> {
  return {
    ...meterSnapshotWithoutInputPeaks(),
    inputPeakDbL: -3,
    inputPeakDbR: -6,
  };
}

describe('isMeterSnapshot input peak fields', () => {
  it('accepts snapshots that omit the input peak fields (not measured)', () => {
    expect(isMeterSnapshot(meterSnapshotWithoutInputPeaks())).toBe(true);
  });

  it('accepts finite input peak fields', () => {
    expect(isMeterSnapshot(meterSnapshot())).toBe(true);
  });

  it('accepts a silent (-Infinity) input peak', () => {
    const silent = Number.NEGATIVE_INFINITY;
    expect(
      isMeterSnapshot({ ...meterSnapshot(), inputPeakDbL: silent, inputPeakDbR: silent }),
    ).toBe(true);
  });

  it.each([null, '-3', true, {}, [-3], Number.NaN, Number.POSITIVE_INFINITY])(
    'rejects an inputPeakDbL that is not a level (%p)',
    (inputPeakDbL) => {
      expect(isMeterSnapshot({ ...meterSnapshot(), inputPeakDbL })).toBe(false);
    },
  );

  it.each([null, '-3', true, {}, [-3], Number.NaN, Number.POSITIVE_INFINITY])(
    'rejects an inputPeakDbR that is not a level (%p)',
    (inputPeakDbR) => {
      expect(isMeterSnapshot({ ...meterSnapshot(), inputPeakDbR })).toBe(false);
    },
  );
});

describe('isMeterSnapshot per-plane fields', () => {
  const planes = (n: number, value: number): number[] => Array.from({ length: n }, () => value);
  const wide = (numChannels: number, length: number): Record<string, unknown> => ({
    ...meterSnapshot(),
    numChannels,
    peakDb: planes(length, -3),
    rmsDb: planes(length, -18),
    truePeakDbtp: planes(length, -2.5),
    inputPeakDb: planes(length, -3),
  });

  it('accepts per-plane arrays as long as numChannels', () => {
    expect(isMeterSnapshot(wide(6, 6))).toBe(true);
  });

  it('rejects per-plane arrays whose length is not numChannels', () => {
    expect(isMeterSnapshot(wide(6, 2))).toBe(false);
  });

  it('rejects numChannels without the per-plane arrays', () => {
    expect(isMeterSnapshot({ ...meterSnapshot(), numChannels: 6 })).toBe(false);
  });
});
