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
  ebur128LoudnessRangeLu,
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
  meteringTruePeakDbtp,
} from '../src/index';
import { textMatcher, unknownKeyText } from './_helpers';

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

    expect(audio.silenceRatio()).toBe(
      meteringSilenceRatio(samples, SR, { thresholdDb: -45, frameLength: 1024, hopLength: 256 }),
    );
    expect(audio.silenceRatio(-30, 512, 128)).toBe(
      meteringSilenceRatio(samples, SR, { thresholdDb: -30, frameLength: 512, hopLength: 128 }),
    );
  });

  it('truePeakDb matches, and an explicit 0 factor is refused rather than read as the default', () => {
    const samples = sine(440, 1);
    const audio = Audio.fromBuffer(samples, SR);

    expect(audio.truePeakDbtp()).toBe(meteringTruePeakDbtp(samples, SR, { oversampleFactor: 4 }));
    expect(() => audio.truePeakDbtp(0)).toThrow(RangeError);
    expect(audio.truePeakDbtp(8)).toBe(meteringTruePeakDbtp(samples, SR, { oversampleFactor: 8 }));
  });

  it('detectClipping matches on a genuinely clipped signal, and a level above 0 dBFS is refused', () => {
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

    // 0 dBFS is a real level, not a sentinel; a level above it is out of domain.
    expect(() => audio.detectClipping({ thresholdDb: 0 })).not.toThrow();
    expect(() => audio.detectClipping({ thresholdDb: 1 })).toThrow(RangeError);
    const refusal = 'meteringDetectClipping: thresholdDb must be finite and in [-120, 0]';
    expect(() => meteringDetectClipping(samples, SR, { thresholdDb: 1 })).toThrow(refusal);
    // Below the numerical floor every sample would read as clipped.
    expect(() => meteringDetectClipping(samples, SR, { thresholdDb: -910 })).toThrow(
      new RangeError(refusal),
    );
    expect(() =>
      meteringDetectClipping(samples, SR, { thresholdDb: Number.NEGATIVE_INFINITY }),
    ).toThrow(new RangeError(refusal));
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

    // Positive control: lowPercentile 0 must
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
    expect(frameA).toEqual(meteringSpectrumFrame(samples, SR, { startSample: 0 }));
    expect(frameB).toEqual(meteringSpectrumFrame(samples, SR, { startSample: 8192 }));

    // Non-vacuity: moving the offset over a non-stationary window must
    // actually change the frame, or a delegate that ignores startSample
    // would pass unnoticed.
    expect(Array.from(frameB.db)).not.toEqual(Array.from(frameA.db));
  });

  it('ebur128LoudnessRangeLu matches the standalone function', () => {
    const loud = sine(440, 4);
    const quiet = sine(440, 4, 0.05);
    const combined = new Float32Array(loud.length + quiet.length);
    combined.set(loud, 0);
    combined.set(quiet, loud.length);
    const audio = Audio.fromBuffer(combined, SR);

    const lra = audio.ebur128LoudnessRangeLu();
    expect(lra).toBeGreaterThan(0);
    expect(lra).toBe(ebur128LoudnessRangeLu(combined, SR));
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

/**
 * The metering methods answer an unknown key, a `validate` key and a zero `nFft`
 * exactly as their facades do, each text naming the `Audio` member; the Node suite pins the same table.
 */
const METERING_REFUSALS: [string, (audio: Audio) => unknown, string, string | RegExp][] = [
  [
    'spectrum unknown key',
    (a) => a.spectrum({ bogus: 1 } as never),
    'TypeError',
    unknownKeyText('Audio.spectrum: unknown key bogus'),
  ],
  [
    'spectrum validate',
    (a) => a.spectrum({ validate: false } as never),
    'TypeError',
    unknownKeyText('Audio.spectrum: unknown key validate'),
  ],
  [
    'spectrum nFft 0',
    (a) => a.spectrum({ nFft: 0 }),
    'RangeError',
    'Audio.spectrum: nFft must not be 0; omit it for the default',
  ],
  [
    'spectrumFrame unknown key',
    (a) => a.spectrumFrame(0, { bogus: 1 } as never),
    'TypeError',
    unknownKeyText('Audio.spectrumFrame: unknown key bogus'),
  ],
  [
    'spectrumFrame validate',
    (a) => a.spectrumFrame(0, { validate: false } as never),
    'TypeError',
    unknownKeyText('Audio.spectrumFrame: unknown key validate'),
  ],
  [
    'spectrumFrame nFft 0',
    (a) => a.spectrumFrame(0, { nFft: 0 }),
    'RangeError',
    'Audio.spectrumFrame: nFft must not be 0; omit it for the default',
  ],
  [
    'dynamicRange unknown key',
    (a) => a.dynamicRange({ bogus: 1 } as never),
    'TypeError',
    unknownKeyText('Audio.dynamicRange: unknown key bogus'),
  ],
  [
    'dynamicRange validate',
    (a) => a.dynamicRange({ validate: false } as never),
    'TypeError',
    unknownKeyText('Audio.dynamicRange: unknown key validate'),
  ],
  [
    'detectClipping unknown key',
    (a) => a.detectClipping({ bogus: 1 } as never),
    'TypeError',
    unknownKeyText('Audio.detectClipping: unknown key bogus'),
  ],
  [
    'detectClipping validate',
    (a) => a.detectClipping({ validate: false } as never),
    'TypeError',
    unknownKeyText('Audio.detectClipping: unknown key validate'),
  ],
];

describe('the metering methods refuse as their facades do (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it.each(METERING_REFUSALS)('%s', (_label, run, errorClass, message) => {
    const audio = Audio.fromBuffer(sine(440, 0.5), SR);
    let caught: unknown;
    try {
      run(audio);
    } catch (error) {
      caught = error;
    }
    expect(caught, 'expected the call to throw').toBeInstanceOf(Error);
    expect({
      errorClass: (caught as object).constructor.name,
      message: (caught as Error).message,
    }).toEqual({ errorClass, message: textMatcher(message) });
  });
});
