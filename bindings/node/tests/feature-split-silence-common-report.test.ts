/**
 * One interval covering everything is the answer to three different situations,
 * and the interval list cannot separate them. Each case below builds one of the
 * three and reads the figures that are supposed to tell it apart from the other
 * two — the verdict always comes from `silenceCeilingDb` read against the
 * `topDb` that was passed, never from the interval counts alone.
 */

import { describe, expect, it } from 'vitest';
import type { SilenceCommonReport } from '../src/index.js';
import { splitSilenceCommon, splitSilenceCommonWithReport } from '../src/index.js';

const SR = 22050;
const DURATION_SAMPLES = SR; // one second
const TOP_DB = 60;
const FRAME_LENGTH = 2048;
const HOP_LENGTH = 512;

/** A one-second buffer sounding only in [startSample, endSample). */
function toneBurst(startSample: number, endSample: number, frequency = 440): Float32Array {
  const samples = new Float32Array(DURATION_SAMPLES);
  for (let i = startSample; i < endSample; i++) {
    samples[i] = Math.sin((2 * Math.PI * frequency * i) / SR);
  }
  return samples;
}

function reportOf(
  signals: Float32Array[],
  topDb: number,
): { intervals: number; report: SilenceCommonReport } {
  const result = splitSilenceCommonWithReport({
    signals,
    topDb,
    frameLength: FRAME_LENGTH,
    hopLength: HOP_LENGTH,
  });
  return { intervals: result.intervals.length / 2, report: result.report };
}

describe('splitSilenceCommonWithReport', () => {
  it('says "not the setting" for a part that never stops', () => {
    const tone = toneBurst(0, DURATION_SAMPLES);
    const { intervals, report } = reportOf([tone], TOP_DB);
    expect(intervals).toBe(1);
    expect(report.maxSignalIntervals).toBe(1);
    expect(report.minSignalIntervals).toBe(1);
    // A steady tone's frames sit within a hair of each other, so the deepest dip
    // is nowhere near any usable threshold. That is what says "not the setting".
    expect(report.silenceCeilingDb).toBeLessThan(6);
  });

  it('puts the ceiling at or above topDb for takes that never go quiet together', () => {
    // Each take is silent for half its length and the halves do not overlap, so
    // the union covers everything while each take has silence of its own.
    const half = Math.floor(DURATION_SAMPLES / 2);
    const early = toneBurst(0, half);
    const late = toneBurst(half, DURATION_SAMPLES);
    const { intervals, report } = reportOf([early, late], TOP_DB);
    expect(intervals).toBe(1);
    // The silence is there at this very setting, so a single interval means the
    // takes do not share it -- the alignment case. The interval counts do NOT
    // say so: a take that sounds once and stops counts 1, the same as a take
    // with no silence at all, which is why the ceiling carries the verdict.
    expect(report.silenceCeilingDb).toBeGreaterThanOrEqual(TOP_DB);
    expect(report.maxSignalIntervals).toBe(1);
    expect(report.minSignalIntervals).toBe(1);
  });

  it('puts the ceiling below topDb for a shared dip the threshold could not see', () => {
    // A dip 40 dB down in both takes, at the same place. At topDb 60 the frames
    // stay above the line and nothing is cut.
    const take = toneBurst(0, DURATION_SAMPLES);
    const quietGain = 10 ** (-40 / 20);
    for (let i = Math.floor(0.4 * SR); i < Math.floor(0.6 * SR); i++) {
      take[i] *= quietGain;
    }
    const other = take.slice();

    const loose = reportOf([take, other], TOP_DB);
    expect(loose.intervals).toBe(1);
    expect(loose.report.maxSignalIntervals).toBe(1);
    // Below the threshold in use, which is what separates this case from the one
    // above, where the ceiling sits at or over it.
    expect(loose.report.silenceCeilingDb).toBeLessThan(TOP_DB);
    expect(loose.report.silenceCeilingDb).toBeGreaterThan(30);

    // The figure is actionable, not merely different: a threshold below it cuts
    // the very same input.
    const tight = reportOf([take, other], loose.report.silenceCeilingDb - 5);
    expect(tight.intervals).toBe(2);
    expect(tight.report.minSignalIntervals).toBe(2);
  });

  it('returns the intervals the plain entry point does, which takes no report', () => {
    const half = Math.floor(DURATION_SAMPLES / 2);
    const signals = [toneBurst(0, Math.floor(0.2 * SR)), toneBurst(half, Math.floor(0.7 * SR))];
    const request = {
      signals,
      topDb: TOP_DB,
      frameLength: FRAME_LENGTH,
      hopLength: HOP_LENGTH,
    };
    const withReport = splitSilenceCommonWithReport(request);
    expect(withReport.intervals).toEqual(splitSilenceCommon(request));
    // Consumed positive control: the shared fixture does produce more than one
    // interval, so the equality above is comparing something.
    expect(withReport.intervals.length).toBeGreaterThan(2);
  });

  it('refuses the same inputs the plain entry point does, naming the field', () => {
    expect(() => splitSilenceCommonWithReport({ signals: [] })).toThrow(/signals/);
    expect(() =>
      splitSilenceCommonWithReport({
        signals: [toneBurst(0, 100), [1, 2, 3] as unknown as Float32Array],
      }),
    ).toThrow(/signals/);
  });

  it('applies the same framing domain as the plain entry point', () => {
    const tone = toneBurst(0, DURATION_SAMPLES);
    expect(() => splitSilenceCommonWithReport({ signals: [tone], frameLength: 0 })).toThrow(
      /frameLength/,
    );
    expect(() => splitSilenceCommonWithReport({ signals: [tone], hopLength: -1 })).toThrow(
      /hopLength/,
    );
  });
});
