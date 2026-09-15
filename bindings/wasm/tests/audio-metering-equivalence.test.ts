/**
 * Equivalence tests for the Audio class's metering methods against the
 * standalone buffer-form functions they delegate to.
 *
 * WASM's Audio holds a plain Float32Array, not a native handle (unlike
 * Node/Python), so these methods call the identical buffer-form function
 * with the held samples and sample rate — the assertions below confirm the
 * delegation rather than a second, independently-behaving code path.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  ebur128LoudnessRange,
  init,
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
} from '../src/index';

const SR = 22050;

function sine(freq: number, durationSec: number, amplitude = 0.5, dcBias = 0): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = dcBias + amplitude * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('Audio metering methods (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('peakDb/rmsDb/dcOffset/crestFactorDb match the buffer-form functions on a non-degenerate signal', () => {
    const samples = sine(440, 1, 0.7, 0.05);
    const audio = Audio.fromBuffer(samples, SR);

    const peak = audio.peakDb();
    const rms = audio.rmsDb();
    const dc = audio.dcOffset();
    const crest = audio.crestFactorDb();

    // Non-vacuity: a silent or DC-free buffer would let a broken delegate
    // pass by returning the same degenerate value on both sides.
    expect(peak).toBeGreaterThan(-10);
    expect(rms).toBeGreaterThan(-20);
    expect(Math.abs(dc)).toBeGreaterThan(0.01);
    expect(crest).toBeGreaterThan(0.5);

    expect(peak).toBe(meteringPeakDb(samples, SR));
    expect(rms).toBe(meteringRmsDb(samples, SR));
    expect(dc).toBe(meteringDcOffset(samples, SR));
    expect(crest).toBe(meteringCrestFactorDb(samples, SR));
  });

  it('silenceRatio matches with explicit and default frame/hop arguments', () => {
    const samples = sine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);

    expect(audio.silenceRatio()).toBe(meteringSilenceRatio(samples, SR, -45, 1024, 256));
    expect(audio.silenceRatio(-30, 512, 128)).toBe(
      meteringSilenceRatio(samples, SR, -30, 512, 128),
    );
  });

  it('truePeakDb matches, including the 0 -> default-oversample normalization', () => {
    const samples = sine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);

    expect(audio.truePeakDb()).toBe(meteringTruePeakDb(samples, SR, 4));
    expect(audio.truePeakDb(0)).toBe(meteringTruePeakDb(samples, SR, 0));
    expect(audio.truePeakDb(8)).toBe(meteringTruePeakDb(samples, SR, 8));
  });

  it('detectClipping matches on a genuinely clipped signal, and threshold 0 selects the library default', () => {
    const samples = new Float32Array(8000).fill(0.1);
    for (let i = 1000; i < 1064; i++) {
      samples[i] = 1.0;
    }
    const audio = Audio.fromBuffer(samples, SR);

    const report = audio.detectClipping();
    // Non-vacuity: an unclipped signal would pass a broken delegate with an
    // empty region list on both sides.
    expect(report.regions.length).toBeGreaterThanOrEqual(1);
    expect(report.clippedSamples).toBeGreaterThanOrEqual(1);
    expect(report).toEqual(meteringDetectClipping(samples, SR));

    // threshold: 0 is the sentinel for "library default" (0.999), not a
    // literal zero threshold - it must match the omitted-threshold call.
    expect(audio.detectClipping({ threshold: 0 })).toEqual(audio.detectClipping());
    // A genuinely out-of-range threshold, including negative, is rejected
    // rather than promoted to the default.
    expect(() => audio.detectClipping({ threshold: -1 })).toThrow();
  });

  it('dynamicRange matches, and lowPercentile 0 is a real 0th-percentile request distinct from the default', () => {
    const loud = sine(440, 3.5);
    const quiet = sine(440, 3.5, 0.05);
    const combined = new Float32Array(loud.length + quiet.length + loud.length);
    combined.set(loud, 0);
    combined.set(quiet, loud.length);
    combined.set(loud, loud.length + quiet.length);
    const audio = Audio.fromBuffer(combined, SR);

    const report = audio.dynamicRange();
    expect(report.windowRmsDb.length).toBeGreaterThanOrEqual(2);
    expect(report).toEqual(meteringDynamicRange(combined, SR));

    // Positive control for the ZeroIsSentinel pitfall: lowPercentile 0 must
    // differ from the default (-1 -> 0.10), proving 0 was not silently
    // promoted to the default.
    const zeroPercentile = audio.dynamicRange({ lowPercentile: 0 });
    expect(zeroPercentile.lowPercentileDb).not.toBe(report.lowPercentileDb);
    expect(zeroPercentile).toEqual(meteringDynamicRange(combined, SR, { lowPercentile: 0 }));
  });

  it('spectrum matches the buffer-form Welch-averaged spectrum', () => {
    const samples = sine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);

    const report = audio.spectrum();
    expect(report.magnitude.length).toBeGreaterThan(0);
    expect(report).toEqual(meteringSpectrum(samples, SR));
  });

  it('spectrumFrame matches per frame, and different offsets produce different frames', () => {
    const samples = sine(440, 2);
    const audio = Audio.fromBuffer(samples, SR);

    const frameA = audio.spectrumFrame(0);
    const frameB = audio.spectrumFrame(8192);
    expect(frameA).toEqual(meteringSpectrumFrame(samples, SR, 0));
    expect(frameB).toEqual(meteringSpectrumFrame(samples, SR, 8192));

    // Non-vacuity: moving the offset over a non-stationary window must
    // actually change the frame, or a delegate that ignores frameOffset
    // would pass unnoticed.
    expect(Array.from(frameB.db)).not.toEqual(Array.from(frameA.db));
  });

  it('ebur128LoudnessRange matches the standalone function', () => {
    const loud = sine(440, 4);
    const quiet = sine(440, 4, 0.05);
    const combined = new Float32Array(loud.length + quiet.length);
    combined.set(loud, 0);
    combined.set(quiet, loud.length);
    const audio = Audio.fromBuffer(combined, SR);

    const lra = audio.ebur128LoudnessRange();
    expect(lra).toBeGreaterThan(0);
    expect(lra).toBe(ebur128LoudnessRange(combined, SR));
  });

  it('the handle-copy contract: mutating the source buffer after construction does not change results', () => {
    const samples = sine(440, 1, 0.7, 0.05);
    const mutable = samples.slice();
    const audio = Audio.fromBuffer(mutable, SR);
    const before = audio.peakDb();

    mutable.fill(0);

    expect(audio.peakDb()).toBe(before);
    expect(audio.peakDb()).not.toBe(meteringPeakDb(mutable, SR));
  });
});
