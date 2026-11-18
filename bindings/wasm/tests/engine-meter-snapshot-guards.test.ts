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
  it('rejects snapshots that omit the new input peak fields', () => {
    expect(isMeterSnapshot(meterSnapshotWithoutInputPeaks())).toBe(false);
  });

  it('accepts finite input peak fields', () => {
    expect(isMeterSnapshot(meterSnapshot())).toBe(true);
  });

  it.each([Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY])(
    'rejects non-finite inputPeakDbL values (%p)',
    (inputPeakDbL) => {
      expect(isMeterSnapshot({ ...meterSnapshot(), inputPeakDbL })).toBe(false);
    },
  );

  it.each([Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY])(
    'rejects non-finite inputPeakDbR values (%p)',
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
