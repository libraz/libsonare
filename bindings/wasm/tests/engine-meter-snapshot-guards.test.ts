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
