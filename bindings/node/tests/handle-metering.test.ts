/**
 * Equivalence tests for the handle-form metering methods on `Audio`.
 *
 * Each method reads the native `SonareAudio*` handle directly instead of a
 * caller `Float32Array`, so the claim under test is that it agrees
 * bit-for-bit with its standalone `metering*` / `ebur128LoudnessRange`
 * counterpart over the same samples, plus the two contract-only behaviors a
 * handle adds: the result is fixed at construction time (mutating the source
 * buffer afterwards changes nothing), and a destroyed handle throws rather
 * than segfaulting.
 */

import { describe, expect, it } from 'vitest';
import {
  Audio,
  ebur128LoudnessRange,
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringRmsDb,
  meteringSilenceRatio,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringTruePeakDb,
} from '../src/index.js';
import { sine } from './_helpers.js';

const SR = 22050;

function makeSine(freq: number, durationSec: number): Float32Array {
  return sine(freq, durationSec, { sampleRate: SR, amp: 0.5 });
}

describe('Audio handle-form metering', () => {
  it('peak / rms / dc / crest agree with the buffer form on a non-degenerate signal', () => {
    const samples = makeSine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const peak = audio.peakDb();
      const rms = audio.rmsDb();
      const dc = audio.dcOffset();
      const crest = audio.crestFactorDb();
      // Non-vacuity: a silent or DC-constant buffer would let peak == rms == 0
      // pass any implementation. This sine is neither.
      expect(peak).toBeGreaterThan(-20);
      expect(rms).toBeGreaterThan(-20);
      expect(Math.abs(crest)).toBeGreaterThan(0.1);
      expect(peak).toBe(meteringPeakDb(samples, SR));
      expect(rms).toBe(meteringRmsDb(samples, SR));
      expect(dc).toBe(meteringDcOffset(samples, SR));
      expect(crest).toBe(meteringCrestFactorDb(samples, SR));
    } finally {
      audio.destroy();
    }
  });

  it('silenceRatio agrees with the buffer form, with a non-degenerate threshold', () => {
    const samples = makeSine(440, 2);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const ratio = audio.silenceRatio();
      expect(ratio).toBeGreaterThanOrEqual(0);
      expect(ratio).toBeLessThanOrEqual(1);
      expect(ratio).toBe(meteringSilenceRatio(samples, SR));
      // A high threshold reports non-trivial silence for a 0.5-amplitude sine;
      // the non-default call and the buffer form must still agree.
      const loudRatio = audio.silenceRatio(-3, 512, 128);
      expect(loudRatio).toBeGreaterThan(0);
      expect(loudRatio).toBe(meteringSilenceRatio(samples, SR, -3, 512, 128));
    } finally {
      audio.destroy();
    }
  });

  it('truePeakDb agrees with the buffer form and exceeds sample peak', () => {
    const samples = makeSine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const tp = audio.truePeakDb();
      expect(tp).toBeGreaterThanOrEqual(audio.peakDb() - 0.1);
      expect(tp).toBe(meteringTruePeakDb(samples, SR));
    } finally {
      audio.destroy();
    }
  });

  it('detectClipping agrees with the buffer form on an actually-clipping signal', () => {
    const samples = new Float32Array(8000).fill(0.1);
    for (let i = 1000; i < 1064; i++) {
      samples[i] = 1.0;
    }
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const report = audio.detectClipping({ threshold: 0.999 });
      // Non-vacuity: a clean signal would report zero regions regardless of
      // whether the handle form is wired correctly.
      expect(report.regions.length).toBeGreaterThanOrEqual(1);
      expect(report.clippedSamples).toBeGreaterThanOrEqual(1);
      const bufferReport = meteringDetectClipping(samples, SR, { threshold: 0.999 });
      expect(report).toEqual(bufferReport);
    } finally {
      audio.destroy();
    }
  });

  it('dynamicRange agrees with the buffer form, and lowPercentile 0 differs from default', () => {
    const loud = makeSine(440, 3.5);
    const quiet = makeSine(440, 3.5);
    for (let i = 0; i < quiet.length; i++) {
      quiet[i] *= 0.05;
    }
    const combined = new Float32Array(loud.length + quiet.length + loud.length);
    combined.set(loud, 0);
    combined.set(quiet, loud.length);
    combined.set(loud, loud.length + quiet.length);
    const audio = Audio.fromBuffer(combined, SR);
    try {
      const report = audio.dynamicRange();
      expect(report.dynamicRangeDb).toBeGreaterThan(0);
      expect(report).toEqual(meteringDynamicRange(combined, SR));

      // Positive control for the ZeroIsSentinel pitfall: 0 is a real 0th
      // percentile request here, not "use the library default" (-1).
      const zeroLow = audio.dynamicRange({ lowPercentile: 0 });
      expect(zeroLow.lowPercentileDb).not.toBeCloseTo(report.lowPercentileDb, 1);
      expect(zeroLow).toEqual(meteringDynamicRange(combined, SR, { lowPercentile: 0 }));
    } finally {
      audio.destroy();
    }
  });

  it('spectrum agrees with the buffer form', () => {
    const samples = makeSine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const report = audio.spectrum();
      expect(report.magnitude.length).toBeGreaterThan(0);
      expect(report).toEqual(meteringSpectrum(samples, SR));
    } finally {
      audio.destroy();
    }
  });

  it('spectrumFrame agrees with the buffer form and frameOffset moves the result', () => {
    const samples = makeSine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const first = audio.spectrumFrame(0);
      const second = audio.spectrumFrame(2048);
      // Non-vacuity: two different offsets into a stationary sine must not
      // collapse to the same frame by construction.
      expect(Array.from(first.magnitude)).not.toEqual(Array.from(second.magnitude));
      expect(first).toEqual(meteringSpectrumFrame(samples, SR, 0));
      expect(second).toEqual(meteringSpectrumFrame(samples, SR, 2048));
    } finally {
      audio.destroy();
    }
  });

  it('ebur128LoudnessRange agrees with the buffer form', () => {
    const samples = makeSine(440, 3);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const lra = audio.ebur128LoudnessRange();
      expect(lra).toBe(ebur128LoudnessRange(samples, SR));
    } finally {
      audio.destroy();
    }
  });

  it('copy semantics: mutating the source buffer changes nothing on the handle', () => {
    const samples = makeSine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);
    try {
      const before = audio.peakDb();
      samples.fill(0.9); // mutate the buffer the handle was constructed from
      const after = audio.peakDb();
      expect(after).toBe(before);
      // The buffer form over the mutated buffer sees the new samples.
      expect(meteringPeakDb(samples, SR)).not.toBe(before);
    } finally {
      audio.destroy();
    }
  });

  it('a destroyed handle throws rather than segfaulting', () => {
    const audio = Audio.fromBuffer(makeSine(440, 0.5), SR);
    audio.destroy();
    expect(() => audio.peakDb()).toThrow('Audio has been destroyed');
    expect(() => audio.detectClipping()).toThrow('Audio has been destroyed');
    expect(() => audio.spectrumFrame(0)).toThrow('Audio has been destroyed');
  });
});
