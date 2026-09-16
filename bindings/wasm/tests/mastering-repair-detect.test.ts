/**
 * Tests for the eight repair detection WASM wrappers — `masteringRepairDetectClicks`,
 * `…DetectNoiseFloor`, `…DetectClipping`, `…DetectCrackle`, `…DetectHum`, `…DetectReverb`,
 * `…DetectTrimRange` and `…DetectTrimRangeStereo`.
 *
 * These measure without repairing and allocate nothing, so there is no ownership question
 * here and no output buffer to compare. What there IS to get wrong is which config field
 * reaches the answer and which does not, and the family does not answer a short or an empty
 * buffer the same way at every entry: the noise-floor detector refuses a buffer shorter than
 * `nFft` while the reverb detector pads one. Both sides of that pair are covered against the
 * SAME buffer, so neither result is a property of the fixture.
 *
 * Every "the knob is inert" assertion is paired with a control showing the detection was
 * non-empty, so equality reads as the field being unread rather than as nothing having been
 * measured.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
} from '../src/index';

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
const CLIP_THRESHOLD = 0.9;

function clippedSignal(): Float32Array {
  const out = sine(440, CLIP_FRAMES, CLIP_BED_AMP);
  for (let i = 0; i < CLIP_LEN; i++) {
    out[CLIP_START + i] = CLIP_VALUE;
  }
  return out;
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
      expect(detected.bandFloorDbfs.length).toBe(32);
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
      expect(loud.floorDbfs).toBeGreaterThan(quiet.floorDbfs + 20);
    });

    it('refuses a buffer shorter than nFft, which the reverb detector accepts', () => {
      // The sharpest divergence in the family, so both sides are measured
      // against the SAME buffer: 512 samples under the default nFft of 1024.
      const short = sine(440, 512, 0.5, 0, TRIM_SR);

      expect(() => masteringRepairDetectNoiseFloor(short, TRIM_SR)).toThrow(/n_fft|nFft/);
      expect(() => masteringRepairDetectReverb(short, TRIM_SR)).not.toThrow();

      // The control for the refusal: the same buffer passes once nFft is small
      // enough to fit inside it, so the throw is about the length rather than
      // about this buffer.
      const fitted = masteringRepairDetectNoiseFloor(short, TRIM_SR, { nFft: 256 });
      expect(Number.isFinite(fitted.floorDbfs)).toBe(true);
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
        clipThreshold: CLIP_THRESHOLD,
      });

      expect(detected.sampleCount).toBe(CLIP_LEN);
      expect(detected.runCount).toBe(1);
      expect(detected.longestRunSamples).toBe(CLIP_LEN);
      expect(detected.sampleFraction).toBeCloseTo(CLIP_LEN / CLIP_FRAMES, 8);
    });

    it('reads clipThreshold and nothing else from the config', () => {
      const samples = clippedSignal();
      const base = masteringRepairDetectClipping(samples, SR, { clipThreshold: CLIP_THRESHOLD });

      // The plateau sits below a threshold above CLIP_VALUE, so the knob is live.
      const above = masteringRepairDetectClipping(samples, SR, { clipThreshold: 0.99 });
      expect(above.sampleCount).toBe(0);
      expect(above.runCount).toBe(0);

      // The reconstruction knobs never reach the result. Exact equality, because
      // they are validated and then unread rather than read and rounded away --
      // and `base` is non-empty, so the equality is not two empty answers.
      const reshaped = masteringRepairDetectClipping(samples, SR, {
        clipThreshold: CLIP_THRESHOLD,
        lpcOrder: 8,
        iterations: 7,
        lpcBlend: 0.1,
      });
      expect(base.sampleCount).toBe(CLIP_LEN);
      expect(reshaped).toEqual(base);
    });

    it('runs with the config omitted', () => {
      // The default clipThreshold is 0.98, above the fixture's 0.95 plateau, so
      // the default path is exercised by its answer of zero rather than by the
      // fixture's count.
      const detected = masteringRepairDetectClipping({ samples: clippedSignal(), sampleRate: SR });
      expect(detected.sampleCount).toBe(0);
      expect(detected.sampleFraction).toBe(0);
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
      // The WPE stage runs only under wpeEnabled, which is clear by default, so
      // 0 here is the measurement rather than an unset field.
      expect(detected.latePredictability).toBe(0);
    });

    it('runs the WPE covariance and solve only under wpeEnabled', () => {
      const samples = noisySignal(0.1, 5);
      const off = masteringRepairDetectReverb(samples, TRIM_SR);
      const on = masteringRepairDetectReverb(samples, TRIM_SR, { wpeEnabled: true });

      expect(off.latePredictability).toBe(0);
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
      ).toThrow(/masteringRepairDetectTrimRange: paddingSamples must be non-negative/);
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
      const range = masteringRepairDetectTrimRangeStereo(silence, silence.slice(), TRIM_SR);
      expect(range.first).toBe(TRIM_FRAMES);
      expect(range.lastExclusive).toBe(TRIM_FRAMES);
    });

    it('agrees with the mono entry when both channels are the same', () => {
      const samples = burstChannel(2000, 8000);
      const stereo = masteringRepairDetectTrimRangeStereo(samples, samples.slice(), TRIM_SR);
      const mono = masteringRepairDetectTrimRange(samples, TRIM_SR);
      expect(stereo).toEqual(mono);
      expect(mono.first).toBe(2000);
    });

    it('accepts the positional call form identically to the request form', () => {
      const left = burstChannel(1000, 3000);
      const right = burstChannel(9000, 11000);

      const positional = masteringRepairDetectTrimRangeStereo(left, right, TRIM_SR, {
        paddingSamples: 200,
      });
      const request = masteringRepairDetectTrimRangeStereo({
        left,
        right,
        sampleRate: TRIM_SR,
        paddingSamples: 200,
      });

      expect(positional).toEqual(request);
      // The equality is worthless if the padding was dropped on both paths.
      expect(request.first).toBe(800);
      expect(request.lastExclusive).toBe(11200);
    });

    it('rejects mismatched channel lengths', () => {
      const left = burstChannel(1000, 3000);
      const right = burstChannel(1000, 3000);
      expect(() =>
        masteringRepairDetectTrimRangeStereo(left, right.slice(0, right.length - 1), TRIM_SR),
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
      expect(() => masteringRepairDetectTrimRangeStereo(empty, empty.slice(), TRIM_SR)).toThrow();
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
      expect(() => masteringRepairDetectTrimRangeStereo(bad, badInf, TRIM_SR)).toThrow();
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
      expect(() => masteringRepairDetectTrimRange(samples, TRIM_SR, { threshold: -1 })).toThrow();
      expect(() =>
        masteringRepairDetectCrackle(samples, TRIM_SR, { threshold: Number.NaN }),
      ).toThrow();
      expect(() =>
        masteringRepairDetectHum(samples, TRIM_SR, { fundamentalHz: Number.POSITIVE_INFINITY }),
      ).toThrow();
    });
  });
});
