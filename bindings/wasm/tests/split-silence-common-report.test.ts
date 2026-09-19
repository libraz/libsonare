/**
 * One interval covering everything is the answer to three different situations
 * and the interval list cannot separate them: no take has a quiet moment at all,
 * the takes each have one but not in the same place, or `topDb` was set too loose
 * to see the ones they have. `splitSilenceCommonWithReport` adds the figures that
 * read them apart, and `silenceCeilingDb` carries the verdict — always read
 * against the `topDb` that was passed.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, splitSilenceCommon, splitSilenceCommonWithReport } from '../dist/index.js';

describe('splitSilenceCommonWithReport', () => {
  const sampleRate = 22050;
  const topDb = 60;
  const frameLength = 2048;
  const hopLength = 512;

  beforeAll(async () => {
    await init();
  });

  /** A one-second buffer with a 440 Hz tone confined to `[startSec, endSec)`, silent elsewhere. */
  function toneWindow(startSec: number, endSec: number): Float32Array {
    const samples = new Float32Array(sampleRate);
    const start = Math.round(startSec * sampleRate);
    const end = Math.round(endSec * sampleRate);
    for (let i = start; i < end; i++) {
      samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate);
    }
    return samples;
  }

  it('says no threshold can open a gap in a part that never stops', () => {
    const continuous = toneWindow(0, 1);
    const { intervals, report } = splitSilenceCommonWithReport({
      signals: [continuous],
      topDb,
      frameLength,
      hopLength,
    });
    expect(intervals.length / 2).toBe(1);
    expect(report.maxSignalIntervals).toBe(1);
    expect(report.minSignalIntervals).toBe(1);
    // A steady tone's frames sit within a hair of each other, so the deepest dip
    // is nowhere near any usable threshold. That is what says "not the setting".
    expect(report.silenceCeilingDb).toBeLessThan(6);
  });

  it('puts the ceiling at or above topDb when takes go quiet but never together', () => {
    const early = toneWindow(0, 0.5);
    const late = toneWindow(0.5, 1);
    const { intervals, report } = splitSilenceCommonWithReport({
      signals: [early, late],
      topDb,
      frameLength,
      hopLength,
    });
    expect(intervals.length / 2).toBe(1);
    // The silence is there at this very setting, so a single interval means the
    // takes do not share it. The interval counts do NOT say so -- a take that
    // sounds once and stops counts 1, the same as a take with no silence at all.
    expect(report.silenceCeilingDb).toBeGreaterThanOrEqual(topDb);
    expect(report.maxSignalIntervals).toBe(1);
    expect(report.minSignalIntervals).toBe(1);
  });

  it('reports a ceiling under topDb for a shared dip, and that figure cuts', () => {
    // A dip 40 dB down in both takes, at the same place. At topDb 60 the frames
    // stay above the line and nothing is cut; the ceiling says 60 was too loose,
    // and a threshold under it splits the very same input.
    const take = toneWindow(0, 1);
    for (let i = Math.round(0.4 * sampleRate); i < Math.round(0.6 * sampleRate); i++) {
      take[i] *= 0.01;
    }
    const other = take.slice();

    const loose = splitSilenceCommonWithReport({
      signals: [take, other],
      topDb,
      frameLength,
      hopLength,
    });
    expect(loose.intervals.length / 2).toBe(1);
    expect(loose.report.maxSignalIntervals).toBe(1);
    // Below the threshold in use, which is the signature of this case and what
    // separates it from the one above, where the ceiling sits at or over it.
    expect(loose.report.silenceCeilingDb).toBeLessThan(topDb);
    expect(loose.report.silenceCeilingDb).toBeGreaterThan(30);

    // The figure is actionable, not merely different: a threshold below it cuts.
    const tight = splitSilenceCommonWithReport({
      signals: [take, other],
      topDb: loose.report.silenceCeilingDb - 5,
      frameLength,
      hopLength,
    });
    expect(tight.intervals.length / 2).toBe(2);
    expect(tight.report.minSignalIntervals).toBe(2);
  });

  it('returns 0 for an all-silent take, where the peak has no ratio', () => {
    const { intervals, report } = splitSilenceCommonWithReport({
      signals: [new Float32Array(sampleRate), new Float32Array(sampleRate)],
      topDb,
      frameLength,
      hopLength,
    });
    expect(intervals.length).toBe(0);
    expect(report.silenceCeilingDb).toBe(0);
    expect(report.maxSignalIntervals).toBe(0);
    expect(report.minSignalIntervals).toBe(0);
  });

  it('separates takes that differ in how broken up they are', () => {
    const once = toneWindow(0, 0.2);
    const twice = toneWindow(0, 0.2);
    for (let i = Math.round(0.6 * sampleRate); i < Math.round(0.8 * sampleRate); i++) {
      twice[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate);
    }
    const { report } = splitSilenceCommonWithReport({
      signals: [once, twice],
      topDb,
      frameLength,
      hopLength,
    });
    expect(report.minSignalIntervals).toBe(1);
    expect(report.maxSignalIntervals).toBe(2);
  });

  it('reports the same intervals the plain entry point does', () => {
    const cases: Float32Array[][] = [
      [toneWindow(0, 0.2)],
      [toneWindow(0, 0.2), toneWindow(0.5, 0.7)],
      [toneWindow(0, 0.5), toneWindow(0.5, 1)],
      [new Float32Array(sampleRate)],
    ];
    for (const signals of cases) {
      const plain = Array.from(splitSilenceCommon({ signals, topDb, frameLength, hopLength }));
      const withReport = splitSilenceCommonWithReport({
        signals,
        topDb,
        frameLength,
        hopLength,
      });
      expect(withReport.intervals).toBeInstanceOf(Int32Array);
      expect(Array.from(withReport.intervals)).toEqual(plain);
    }
  });

  it('falls back to the same defaults the plain entry point does', () => {
    const signals = [toneWindow(0, 0.2), toneWindow(0.5, 0.7)];
    const omitted = splitSilenceCommonWithReport({ signals });
    const spelled = splitSilenceCommonWithReport({
      signals,
      topDb: 60.0,
      frameLength: 2048,
      hopLength: 512,
    });
    expect(Array.from(omitted.intervals)).toEqual(Array.from(spelled.intervals));
    expect(omitted.report).toEqual(spelled.report);
    expect(Array.from(omitted.intervals)).toEqual(Array.from(splitSilenceCommon({ signals })));
  });

  it('adds no refusal of its own', () => {
    const a = toneWindow(0, 0.2);
    expect(() => splitSilenceCommonWithReport({ signals: [] })).toThrow();
    expect(() => splitSilenceCommonWithReport({ signals: [a, new Float32Array(0)] })).toThrow();
    expect(() =>
      splitSilenceCommonWithReport({ signals: [a, null as unknown as Float32Array] }),
    ).toThrow(/must be a Float32Array/);
  });
});
