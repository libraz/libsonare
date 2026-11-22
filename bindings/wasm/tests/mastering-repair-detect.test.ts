/**
 * Tests for the eight repair detection WASM wrappers — `masteringRepairDetectClicks`,
 * `…DetectNoiseFloor`, `…DetectClipping`, `…DetectCrackle`, `…DetectHum`, `…DetectReverb`,
 * `…DetectTrimRange` and `…DetectTrimRangeStereo`.
 *
 * These measure without repairing and allocate nothing, so there is no ownership question
 * here and no output buffer to compare. What there IS to get wrong is which config field
 * reaches the answer and which does not. A buffer shorter than `nFft` is padded by both the
 * noise-floor and the reverb detector, and the noise-floor case is checked against the same
 * buffer padded by hand.
 *
 * Every "the knob is inert" assertion is paired with a control showing the detection was
 * non-empty, so equality reads as the field being unread rather than as nothing having been
 * measured.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringAudioProfile,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
} from '../src/index';
import { present } from './_helpers';

const SR = 48000;
const TRIM_SR = 22050;

function sine(freq: number, frames: number, amp: number, phase = 0, rate = SR): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / rate + phase);
  }
  return out;
}

// Deterministic LCG noise: the same fixture every run, so one run's measured level can be
// compared against another's.
function withNoise(samples: Float32Array, amp: number, seed: number): Float32Array {
  let state = seed >>> 0;
  const out = new Float32Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const u = (state >>> 8) / (1 << 24);
    out[i] = (samples[i] ?? 0) + (u - 0.5) * amp;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Click fixture. threshold/neighborRatio as proven by the stereo declick
// fixture: a click 4x the bed amplitude, planted over an independent noise bed.
// ---------------------------------------------------------------------------
const CLICK_FRAMES = SR / 2;
const CLICK_BED_AMP = 0.15;
const CLICK_AMP = 0.6;
const CLICK_POS = 5053;
const CLICK_WIDTH = 2;
const DECLICK_CONFIG = {
  threshold: 0.35,
  neighborRatio: 2.0,
  maxClickSamples: 8,
  lpcOrder: 20,
  residualRatio: 8.0,
};

function clickBed(): Float32Array {
  return withNoise(sine(440, CLICK_FRAMES, CLICK_BED_AMP), 0.01, 1);
}

function clickedSignal(): Float32Array {
  const out = clickBed();
  for (let i = 0; i < CLICK_WIDTH; i++) {
    out[CLICK_POS + i] = (out[CLICK_POS + i] ?? 0) + CLICK_AMP;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Clipping fixture: one plateau, on a bed strictly below the threshold.
// ---------------------------------------------------------------------------
const CLIP_FRAMES = SR / 4;
const CLIP_BED_AMP = 0.3;
const CLIP_VALUE = 0.95;
const CLIP_START = 5000;
const CLIP_LEN = 20;
const CLIP_THRESHOLD_DB = 20 * Math.log10(0.9);

function clippedSignal(): Float32Array {
  const out = sine(440, CLIP_FRAMES, CLIP_BED_AMP);
  for (let i = 0; i < CLIP_LEN; i++) {
    out[CLIP_START + i] = CLIP_VALUE;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Flat-top fixture: a tone clipped hard against +/-1, then turned down so
// nothing reaches clipThresholdDb anymore. clippedSamples alone would read this as
// clean; only the flat runs are still evidence of the clipping. The paired
// full-scale sine below never clips against a fixed threshold in the time
// domain -- it only reads that way at clipThresholdDb -- and its apex never
// repeats a sample, so no flat run forms there.
// ---------------------------------------------------------------------------
const FLAT_RATE = 48000;
const FLAT_FRAMES = 48000;
const FLAT_FREQ = 220;

function clippedTone(): Float32Array {
  const out = new Float32Array(FLAT_FRAMES);
  for (let i = 0; i < FLAT_FRAMES; i++) {
    const t = i / FLAT_RATE;
    out[i] = Math.min(1, Math.max(-1, 1.8 * Math.sin(2 * Math.PI * FLAT_FREQ * t)));
  }
  return out;
}

function attenuatedTone(): Float32Array {
  const out = clippedTone();
  for (let i = 0; i < out.length; i++) {
    out[i] = (out[i] ?? 0) * 0.25;
  }
  return out;
}

function fullScaleSine(): Float32Array {
  return sine(FLAT_FREQ, FLAT_FRAMES, 1, 0, FLAT_RATE);
}

// ---------------------------------------------------------------------------
// Crackle fixture: isolated spikes over a quiet tone. A spike's window median
// is its larger neighbour, so its deviation is SPIKE_VALUE - BED_AMP = 0.45 --
// above LOW_THRESHOLD and below HIGH_THRESHOLD.
// ---------------------------------------------------------------------------
const CRACKLE_FRAMES = 2000;
const SPIKE_VALUE = 0.5;
const SPIKE_POSITIONS = [100, 300, 500, 700, 900];
const CRACKLE_BED_AMP = 0.05;
const LOW_THRESHOLD = 0.3;
const HIGH_THRESHOLD = 0.6;

function crackledSignal(): Float32Array {
  const out = sine(440, CRACKLE_FRAMES, CRACKLE_BED_AMP, 0, TRIM_SR);
  SPIKE_POSITIONS.forEach((pos, i) => {
    out[pos] = i % 2 === 0 ? SPIKE_VALUE : -SPIKE_VALUE;
  });
  return out;
}

// ---------------------------------------------------------------------------
// Hum fixture. 48.5 Hz lands exactly on the 17-point search grid the detector
// uses (the default 50 Hz +/- the default 2 Hz range, in 0.25 Hz steps), so the
// search picks it up without interpolation error. The 3 kHz tone sits far past
// the highest harmonic bin the detector reads (16 * 50 Hz).
// ---------------------------------------------------------------------------
const HUM_FRAMES = TRIM_SR;
const HUM_HZ = 48.5;

function hummedSignal(): Float32Array {
  const fundamental = sine(HUM_HZ, HUM_FRAMES, 0.3, 0, TRIM_SR);
  const second = sine(2 * HUM_HZ, HUM_FRAMES, 0.15, 0, TRIM_SR);
  const programme = sine(3000, HUM_FRAMES, 0.05, 0, TRIM_SR);
  const out = new Float32Array(HUM_FRAMES);
  for (let i = 0; i < HUM_FRAMES; i++) {
    out[i] = (fundamental[i] ?? 0) + (second[i] ?? 0) + (programme[i] ?? 0);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Noise-floor / reverb fixture: a tone under deterministic noise, half a second
// at 22050 Hz -- 43 hops at the default 256-sample hop, enough frames for the
// quantile estimator to have a quietest decile.
// ---------------------------------------------------------------------------
const SPECTRAL_FRAMES = TRIM_SR / 2;

function noisySignal(noiseAmp: number, seed: number): Float32Array {
  return withNoise(sine(440, SPECTRAL_FRAMES, 0.5, 0, TRIM_SR), noiseAmp, seed);
}

// ---------------------------------------------------------------------------
// Trim fixtures. Alternating sign at every sample, so |x| is exactly the block
// amplitude at every index and every boundary is an exact sample index.
// ---------------------------------------------------------------------------
const TRIM_FRAMES = TRIM_SR;
const LOUD_AMP = 0.5;

function burstChannel(begin: number, end: number): Float32Array {
  const out = new Float32Array(TRIM_FRAMES);
  for (let i = begin; i < end; i++) {
    out[i] = i % 2 === 0 ? LOUD_AMP : -LOUD_AMP;
  }
  return out;
}

describe('repair detection (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  describe('masteringRepairDetectClicks', () => {
    it('counts a planted click and reports it as a run', () => {
      const detected = masteringRepairDetectClicks({
        samples: clickedSignal(),
        sampleRate: SR,
        ...DECLICK_CONFIG,
      });

      expect(detected.count).toBe(1);
      expect(detected.longestRunSamples).toBeGreaterThan(0);
      expect(detected.longestRunSamples).toBeLessThanOrEqual(DECLICK_CONFIG.maxClickSamples);
      // Runs per second, so one click in half a second reads 2.
      expect(detected.perSecond).toBeCloseTo(detected.count / (CLICK_FRAMES / SR), 5);
      expect(Number.isFinite(detected.rejected)).toBe(true);
    });

    it('reports nothing on the same bed without the click', () => {
      // The control for the count above: the bed alone is what the fixture is
      // made of, so a detector answering 1 for everything would fail here.
      const detected = masteringRepairDetectClicks(clickBed(), SR, DECLICK_CONFIG);
      expect(detected.count).toBe(0);
      expect(detected.longestRunSamples).toBe(0);
      expect(detected.perSecond).toBe(0);
    });

    it('runs with the config omitted', () => {
      const detected = masteringRepairDetectClicks({ samples: clickedSignal(), sampleRate: SR });
      expect(Number.isFinite(detected.perSecond)).toBe(true);
      expect(detected.count).toBeGreaterThanOrEqual(0);
    });
  });

  describe('masteringRepairDetectNoiseFloor', () => {
    it('measures a floor that tracks the noise it was given', () => {
      const detected = masteringRepairDetectNoiseFloor({
        samples: noisySignal(0.2, 1),
        sampleRate: TRIM_SR,
      });
      expect(detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
      // Compact: one entry per band holding a bin, named by bandFloorBand.
      expect(detected.bandFloorBand).toBeInstanceOf(Int32Array);
      expect(detected.bandFloorBand.length).toBe(detected.bandFloorDbfs.length);
      expect(detected.bandFloorBand.length).toBeGreaterThan(0);
      expect(detected.bandFloorBand.length).toBeLessThanOrEqual(32);
      expect(
        Array.from(detected.bandFloorBand).every(
          (b, i) => b >= 0 && b < 32 && (i === 0 || b > detected.bandFloorBand[i - 1]),
        ),
      ).toBe(true);
      expect(Number.isFinite(detected.floorDbfs)).toBe(true);
      expect(detected.floorDbfs).toBeLessThan(0);

      // Noise alone, because a 0.5 tone pins the estimate: measured -22.6 dBFS
      // at every noise amplitude from 0.005 to 0.1, moving only at 0.4. Without
      // it the floor tracks the noise exactly -- measured -46.5 and -20.5 dBFS
      // for 0.02 and 0.4, the 26 dB the twenty-fold change is.
      const quiet = masteringRepairDetectNoiseFloor(
        withNoise(new Float32Array(SPECTRAL_FRAMES), 0.02, 1),
        TRIM_SR,
      );
      const loud = masteringRepairDetectNoiseFloor(
        withNoise(new Float32Array(SPECTRAL_FRAMES), 0.4, 1),
        TRIM_SR,
      );
      expect(loud.floorDbfs).toBeGreaterThan(present(quiet.floorDbfs) + 20);
    });

    it('pads a buffer shorter than nFft, as the reverb detector does', () => {
      // 512 samples under the default nFft of 1024 measure as the same buffer zero-padded.
      const short = sine(440, 512, 0.5, 0, TRIM_SR);
      const padded = new Float32Array(1024);
      padded.set(short);

      expect(masteringRepairDetectNoiseFloor(short, TRIM_SR).floorDbfs).toBe(
        masteringRepairDetectNoiseFloor(padded, TRIM_SR).floorDbfs,
      );
      expect(() => masteringRepairDetectReverb(short, TRIM_SR)).not.toThrow();
    });

    it('rejects a non-power-of-two nFft and a non-positive hopLength by name', () => {
      const samples = noisySignal(0.2, 3);
      expect(() => masteringRepairDetectNoiseFloor(samples, TRIM_SR, { nFft: 1000 })).toThrow(
        /masteringRepairDetectNoiseFloor: nFft/,
      );
      expect(() => masteringRepairDetectNoiseFloor(samples, TRIM_SR, { hopLength: 0 })).toThrow(
        /masteringRepairDetectNoiseFloor: hopLength/,
      );
    });

    it('rejects an unknown mode and an unknown noise estimator', () => {
      const samples = noisySignal(0.2, 4);
      expect(() =>
        masteringRepairDetectNoiseFloor(samples, TRIM_SR, {
          mode: 'wiener' as unknown as 'logMmse',
        }),
      ).toThrow(/unknown denoise mode/);
      expect(() =>
        masteringRepairDetectNoiseFloor(samples, TRIM_SR, {
          noiseEstimator: 'minima' as unknown as 'quantile',
        }),
      ).toThrow(/unknown denoise noise estimator/);
    });
  });

  describe('masteringRepairDetectClipping', () => {
    it('counts the plateau exactly and derives the fraction from the input length', () => {
      const detected = masteringRepairDetectClipping({
        samples: clippedSignal(),
        sampleRate: SR,
        clipThresholdDb: CLIP_THRESHOLD_DB,
      });

      expect(detected.clippedSamples).toBe(CLIP_LEN);
      expect(detected.runCount).toBe(1);
      expect(detected.longestRunSamples).toBe(CLIP_LEN);
      expect(detected.clippedRatio).toBeCloseTo(CLIP_LEN / CLIP_FRAMES, 8);
    });

    it('reads clipThresholdDb and nothing else from the config', () => {
      const samples = clippedSignal();
      const base = masteringRepairDetectClipping(samples, SR, {
        clipThresholdDb: CLIP_THRESHOLD_DB,
      });

      // The plateau sits below a threshold above CLIP_VALUE, so the knob is live.
      const above = masteringRepairDetectClipping(samples, SR, {
        clipThresholdDb: 20 * Math.log10(0.99),
      });
      expect(above.clippedSamples).toBe(0);
      expect(above.runCount).toBe(0);

      // The reconstruction knobs never reach the result. Exact equality, because
      // they are validated and then unread rather than read and rounded away --
      // and `base` is non-empty, so the equality is not two empty answers.
      const reshaped = masteringRepairDetectClipping(samples, SR, {
        clipThresholdDb: CLIP_THRESHOLD_DB,
        lpcOrder: 8,
        iterations: 7,
      });
      expect(base.clippedSamples).toBe(CLIP_LEN);
      expect(reshaped).toEqual(base);
    });

    it('runs with the config omitted', () => {
      // The default clipThreshold is 0.98, above the fixture's 0.95 plateau, so
      // the default path is exercised by its answer of zero rather than by the
      // fixture's count.
      const detected = masteringRepairDetectClipping({ samples: clippedSignal(), sampleRate: SR });
      expect(detected.clippedSamples).toBe(0);
      expect(detected.clippedRatio).toBe(0);
    });

    it('reads a flat top that survived a gain change hiding it from clippedSamples', () => {
      // Both directions on real signals, so neither reading is free: a tone
      // clipped hard, and turned down enough that nothing reaches
      // clipThreshold anymore, reports zero the old way and non-zero the
      // flat-top way -- the reverse of the full-scale sine below.
      const quiet = masteringRepairDetectClipping(attenuatedTone(), FLAT_RATE);

      expect(quiet.clippedSamples).toBe(0);
      expect(quiet.runCount).toBe(0);
      expect(quiet.flatRunCount).toBeGreaterThan(0);
      expect(quiet.flatSampleCount).toBeGreaterThan(0);
      expect(quiet.longestFlatRunSamples).toBeGreaterThan(0);
      expect(quiet.flatLevel).toBeCloseTo(0.25, 4);
    });

    it('does not read an unclipped sine peak as a flat top', () => {
      // A full-scale sine reaches the default threshold every cycle without
      // ever having been clipped -- clippedSamples alone reads it as clipped.
      // The apex of a sine never repeats a sample, so no flat run forms.
      const detected = masteringRepairDetectClipping(fullScaleSine(), FLAT_RATE);

      expect(detected.clippedSamples).toBeGreaterThan(0);
      expect(detected.flatRunCount).toBe(0);
      expect(detected.flatSampleCount).toBe(0);
      expect(detected.longestFlatRunSamples).toBe(0);
      expect(detected.flatLevel).toBe(0);
    });
  });

  describe('masteringRepairDetectCrackle', () => {
    it('counts the spikes the median criterion flags', () => {
      const detected = masteringRepairDetectCrackle({
        samples: crackledSignal(),
        sampleRate: TRIM_SR,
        threshold: LOW_THRESHOLD,
      });

      expect(detected.sampleCount).toBe(SPIKE_POSITIONS.length);
      expect(detected.sampleFraction).toBeCloseTo(SPIKE_POSITIONS.length / CRACKLE_FRAMES, 8);
      expect(detected.perSecond).toBeCloseTo(
        SPIKE_POSITIONS.length / (CRACKLE_FRAMES / TRIM_SR),
        4,
      );

      // The control: a threshold above every spike's deviation flags none, so
      // the count above is of the spikes rather than of the bed.
      const tight = masteringRepairDetectCrackle(crackledSignal(), TRIM_SR, {
        threshold: HIGH_THRESHOLD,
      });
      expect(tight.sampleCount).toBe(0);
    });

    it('measures by the median criterion in wavelet mode too', () => {
      const samples = crackledSignal();
      const median = masteringRepairDetectCrackle(samples, TRIM_SR, {
        mode: 'median' as const,
        threshold: LOW_THRESHOLD,
      });
      const wavelet = masteringRepairDetectCrackle(samples, TRIM_SR, {
        mode: 'waveletShrinkage' as const,
        threshold: LOW_THRESHOLD,
        levels: 3,
      });

      // Wavelet shrinkage removes crackle without ever deciding a sample is
      // crackle, so it has no detection of its own to report.
      expect(wavelet).toEqual(median);
      expect(median.sampleCount).toBe(SPIKE_POSITIONS.length);
    });

    it('runs with the config omitted and rejects an unknown mode', () => {
      const detected = masteringRepairDetectCrackle({
        samples: crackledSignal(),
        sampleRate: TRIM_SR,
      });
      // The default threshold is 0.4, below the 0.45 deviation each spike has.
      expect(detected.sampleCount).toBe(SPIKE_POSITIONS.length);

      expect(() =>
        masteringRepairDetectCrackle(crackledSignal(), TRIM_SR, {
          mode: 'wavelet-shrink' as unknown as 'median',
        }),
      ).toThrow(/unknown decrackle mode/);
    });
  });

  describe('masteringRepairDetectHum', () => {
    it('finds the planted fundamental and reports a level per harmonic', () => {
      const detected = masteringRepairDetectHum({ samples: hummedSignal(), sampleRate: TRIM_SR });

      expect(detected.fundamentalHz).toBeCloseTo(HUM_HZ, 2);
      expect(detected.harmonicDbfs).toBeInstanceOf(Float32Array);
      expect(detected.harmonicDbfs.length).toBe(16);
      expect(detected.harmonics).toBeGreaterThan(0);
      // Prominence is a ratio over the median candidate, so 1.0 means no peak
      // was found at all; a planted hum must stand above that.
      expect(detected.fundamentalProminence).toBeGreaterThan(1);
    });

    it('runs the estimation path whatever adaptive says', () => {
      const samples = hummedSignal();
      const off = masteringRepairDetectHum(samples, TRIM_SR, { adaptive: false });
      const on = masteringRepairDetectHum(samples, TRIM_SR, { adaptive: true });

      // The fixed repair path notches the configured frequency without ever
      // looking for hum, so a detector following the flag would hand back its
      // own input at `adaptive: false`. The control for this equality is the
      // assertion below that neither answer is the configured 50 Hz default.
      expect(on).toEqual(off);
      expect(off.fundamentalHz).toBeCloseTo(HUM_HZ, 2);
      expect(off.fundamentalHz).not.toBeCloseTo(50, 2);
    });

    it('searches inside searchRangeHz, so a range excluding the hum misses it', () => {
      const samples = hummedSignal();
      // The search window is fundamentalHz +/- searchRangeHz. 0.25 Hz around
      // the 50 Hz default cannot reach the 48.5 Hz hum.
      const narrow = masteringRepairDetectHum(samples, TRIM_SR, { searchRangeHz: 0.25 });
      expect(narrow.fundamentalHz).toBeGreaterThan(49.7);
      expect(narrow.fundamentalHz).toBeLessThan(50.3);
    });
  });

  describe('masteringRepairDetectReverb', () => {
    it('measures the module own late-lag decay and leaves WPE at zero by default', () => {
      const detected = masteringRepairDetectReverb({
        samples: noisySignal(0.1, 5),
        sampleRate: TRIM_SR,
      });

      expect(Number.isFinite(detected.lateDecayRatioDb)).toBe(true);
      // The WPE stage runs only under wpeEnabled, which is clear by default.
      expect(detected).not.toHaveProperty('latePredictability');
    });

    it('runs the WPE covariance and solve only under wpeEnabled', () => {
      const samples = noisySignal(0.1, 5);
      const off = masteringRepairDetectReverb(samples, TRIM_SR);
      const on = masteringRepairDetectReverb(samples, TRIM_SR, { wpeEnabled: true });

      expect(off).not.toHaveProperty('latePredictability');
      expect(on.latePredictability).toBeGreaterThan(0);
      // The decay statistic is the same measurement either way: WPE's
      // prediction is never subtracted here.
      expect(on.lateDecayRatioDb).toBeCloseTo(off.lateDecayRatioDb, 5);
    });

    it('rejects a non-power-of-two nFft and a hopLength past nFft by name', () => {
      const samples = noisySignal(0.1, 6);
      expect(() => masteringRepairDetectReverb(samples, TRIM_SR, { nFft: 1000 })).toThrow(
        /masteringRepairDetectReverb: nFft/,
      );
      expect(() =>
        masteringRepairDetectReverb(samples, TRIM_SR, { nFft: 512, hopLength: 1024 }),
      ).toThrow(/masteringRepairDetectReverb: hopLength/);
    });
  });

  describe('masteringRepairDetectTrimRange', () => {
    it('reports the range the repair would cut to', () => {
      const range = masteringRepairDetectTrimRange({
        samples: burstChannel(5000, 7000),
        sampleRate: TRIM_SR,
      });
      expect(range.first).toBe(5000);
      expect(range.lastExclusive).toBe(7000);
    });

    it('includes paddingSamples in the range rather than reporting the signal extent', () => {
      const samples = burstChannel(5000, 7000);
      const bare = masteringRepairDetectTrimRange(samples, TRIM_SR);
      const padded = masteringRepairDetectTrimRange(samples, TRIM_SR, { paddingSamples: 500 });

      expect(padded.first).toBe(bare.first - 500);
      expect(padded.lastExclusive).toBe(bare.lastExclusive + 500);
    });

    it('reports (length, length) when nothing clears the threshold', () => {
      const range = masteringRepairDetectTrimRange(new Float32Array(TRIM_FRAMES), TRIM_SR);
      expect(range.first).toBe(TRIM_FRAMES);
      expect(range.lastExclusive).toBe(TRIM_FRAMES);
    });

    it('refuses a negative paddingSamples rather than absorbing it', () => {
      const samples = burstChannel(5000, 7000);
      expect(() =>
        masteringRepairDetectTrimRange(samples, TRIM_SR, { paddingSamples: -1 }),
      ).toThrow(/masteringRepairDetectTrimRange: paddingSamples must be at least 0$/);
      // The control: 0 is accepted, so the refusal is of the value.
      expect(masteringRepairDetectTrimRange(samples, TRIM_SR, { paddingSamples: 0 }).first).toBe(
        5000,
      );
    });

    it('rejects an unknown mode name', () => {
      expect(() =>
        masteringRepairDetectTrimRange(burstChannel(5000, 7000), TRIM_SR, {
          mode: 'gate' as unknown as 'peak',
        }),
      ).toThrow(/unknown trim silence mode/);
    });
  });

  describe('masteringRepairDetectTrimRangeStereo', () => {
    it('unions two disjoint per-channel ranges', () => {
      const range = masteringRepairDetectTrimRangeStereo({
        left: burstChannel(1000, 3000),
        right: burstChannel(9000, 11000),
        sampleRate: TRIM_SR,
      });
      expect(range.first).toBe(1000);
      expect(range.lastExclusive).toBe(11000);
    });

    it('lets a SILENT channel contribute no edge at all', () => {
      // The one assertion a naive min/max implementation fails. A silent
      // channel scans to (length, length), so taking the max of the two
      // lastExclusive values would push the range out to the buffer's end and
      // keep 17050 samples of nothing.
      const range = masteringRepairDetectTrimRangeStereo({
        left: burstChannel(5000, 7000),
        right: new Float32Array(TRIM_FRAMES),
        sampleRate: TRIM_SR,
      });
      expect(range.first).toBe(5000);
      expect(range.lastExclusive).toBe(7000);
      expect(range.lastExclusive).not.toBe(TRIM_FRAMES);

      // Symmetric in the two channels.
      const mirrored = masteringRepairDetectTrimRangeStereo({
        left: new Float32Array(TRIM_FRAMES),
        right: burstChannel(5000, 7000),
        sampleRate: TRIM_SR,
      });
      expect(mirrored).toEqual(range);
    });

    it('reports (length, length) when neither channel carries signal', () => {
      const silence = new Float32Array(TRIM_FRAMES);
      const range = masteringRepairDetectTrimRangeStereo({
        left: silence,
        right: silence.slice(),
        sampleRate: TRIM_SR,
      });
      expect(range.first).toBe(TRIM_FRAMES);
      expect(range.lastExclusive).toBe(TRIM_FRAMES);
    });

    it('agrees with the mono entry when both channels are the same', () => {
      const samples = burstChannel(2000, 8000);
      const stereo = masteringRepairDetectTrimRangeStereo({
        left: samples,
        right: samples.slice(),
        sampleRate: TRIM_SR,
      });
      const mono = masteringRepairDetectTrimRange(samples, TRIM_SR);
      expect(stereo).toEqual(mono);
      expect(mono.first).toBe(2000);
    });

    it('is request-only and applies the padding', () => {
      const left = burstChannel(1000, 3000);
      const right = burstChannel(9000, 11000);

      const request = masteringRepairDetectTrimRangeStereo({
        left,
        right,
        sampleRate: TRIM_SR,
        paddingSamples: 200,
      });

      expect(request.first).toBe(800);
      expect(request.lastExclusive).toBe(11200);
      const positional = masteringRepairDetectTrimRangeStereo as unknown as (
        ...args: unknown[]
      ) => unknown;
      expect(() => positional(left, right, TRIM_SR)).toThrow(TypeError);
    });

    it('rejects mismatched channel lengths', () => {
      const left = burstChannel(1000, 3000);
      const right = burstChannel(1000, 3000);
      expect(() =>
        masteringRepairDetectTrimRangeStereo({
          left,
          right: right.slice(0, right.length - 1),
          sampleRate: TRIM_SR,
        }),
      ).toThrow();
    });
  });

  describe('input refusals shared by every detection entry', () => {
    const empty = new Float32Array(0);

    it('refuses an empty buffer at every entry', () => {
      // The core does NOT share one rule here -- detect_noise_floor and
      // detect_reverb throw for an empty buffer while the other six hand back a
      // zeroed detection or range. These wrappers load through
      // loadValidatedAudio, as every repair wrapper in this file's binding
      // source does, so the refusal is uniform across the surface and matches
      // the C ABI's own flattening.
      expect(() => masteringRepairDetectClicks(empty, SR)).toThrow();
      expect(() => masteringRepairDetectNoiseFloor(empty, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectClipping(empty, SR)).toThrow();
      expect(() => masteringRepairDetectCrackle(empty, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectHum(empty, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectReverb(empty, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectTrimRange(empty, TRIM_SR)).toThrow();
      expect(() =>
        masteringRepairDetectTrimRangeStereo({
          left: empty,
          right: empty.slice(),
          sampleRate: TRIM_SR,
        }),
      ).toThrow();
    });

    it('refuses a non-finite sample at every entry', () => {
      const bad = burstChannel(100, 900);
      bad[10] = Number.NaN;
      const badInf = burstChannel(100, 900);
      badInf[10] = Number.POSITIVE_INFINITY;

      expect(() => masteringRepairDetectClicks(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectNoiseFloor(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectClipping(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectCrackle(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectHum(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectReverb(bad, TRIM_SR)).toThrow();
      expect(() => masteringRepairDetectTrimRange(bad, TRIM_SR)).toThrow();
      expect(() =>
        masteringRepairDetectTrimRangeStereo({ left: bad, right: badInf, sampleRate: TRIM_SR }),
      ).toThrow();
    });

    it('refuses a NaN and a wrong-typed sample rate', () => {
      const samples = burstChannel(100, 900);
      expect(() => masteringRepairDetectClicks(samples, Number.NaN)).toThrow();
      expect(() =>
        masteringRepairDetectTrimRange(samples, 'not-a-number' as unknown as number),
      ).toThrow();
      expect(() => masteringRepairDetectHum(samples, 0)).toThrow();
    });

    it('refuses a non-finite option value', () => {
      const samples = burstChannel(100, 900);
      expect(() => masteringRepairDetectTrimRange(samples, TRIM_SR, { thresholdDb: 1 })).toThrow();
      expect(() =>
        masteringRepairDetectCrackle(samples, TRIM_SR, { threshold: Number.NaN }),
      ).toThrow();
      expect(() =>
        masteringRepairDetectHum(samples, TRIM_SR, { fundamentalHz: Number.POSITIVE_INFINITY }),
      ).toThrow();
    });
  });
});

describe('the profile and the detectors measure one take alike', () => {
  // The profile's noise and hum groups are the detectors' readings carried in a JSON
  // document; this pins that the document path loses nothing, including array types.
  const SR_EQ = 48000;
  const humTake = (): Float32Array => {
    let seed = 1;
    const out = new Float32Array(SR_EQ * 2);
    for (let i = 0; i < out.length; i++) {
      seed = (seed * 1103515245 + 12345) % 2147483648;
      out[i] =
        0.05 * Math.sin((2 * Math.PI * 60 * i) / SR_EQ) + 0.01 * ((seed / 2147483648) * 2 - 1);
    }
    return out;
  };

  beforeAll(async () => {
    await init();
  });

  it('carries the noise floor detection unchanged', () => {
    const take = humTake();
    const profile = masteringAudioProfile(take, SR_EQ);
    expect(profile.defects?.noise).toStrictEqual(masteringRepairDetectNoiseFloor(take, SR_EQ));
    expect(profile.defects?.noise.bandFloorBand).toBeInstanceOf(Int32Array);
  });

  it('carries the hum detection unchanged at the mains frequency it settled on', () => {
    const take = humTake();
    const profile = masteringAudioProfile(take, SR_EQ);
    // The profile searches 50 and 60 Hz and keeps the more prominent; this take hums at 60.
    expect(profile.defects?.hum.fundamentalHz).toBe(60);
    expect(profile.defects?.hum).toStrictEqual(
      masteringRepairDetectHum({ samples: take, sampleRate: SR_EQ, fundamentalHz: 60 }),
    );
  });
});
