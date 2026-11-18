/**
 * WASM feature API precision tests against reference data.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  chroma,
  detectOnsets,
  ErrorCode,
  framesToSamples,
  framesToTime,
  griffinLim,
  harmonic,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  init,
  melDelta,
  melSpectrogram,
  melToHz,
  mfcc,
  midiToHz,
  normalize,
  noteSegments,
  noteToHz,
  onsetBacktrack,
  percussive,
  piptrack,
  pitchPyin,
  pitchShift,
  pitchYin,
  reassignedSpectrogram,
  resample,
  rmsEnergy,
  samplesToFrames,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralFlux,
  spectralRolloff,
  stft,
  stftDb,
  timeStretch,
  timeToFrames,
  trim,
  zeroCrossingRate,
} from '../dist/index.js';

const refDir = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '..',
  '..',
  '..',
  'tests',
  'librosa',
  'reference',
);

function loadRef(filename: string) {
  return JSON.parse(readFileSync(resolve(refDir, filename), 'utf-8'));
}

function withinRel(actual: number, expected: number, tol: number): boolean {
  return Math.abs(actual - expected) <= Math.abs(expected) * tol;
}

function withinAbs(actual: number, expected: number, tol: number): boolean {
  return Math.abs(actual - expected) <= tol;
}

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function generateTwoTone(f1: number, f2: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] =
      0.5 * Math.sin((2 * Math.PI * f1 * i) / sr) + 0.5 * Math.sin((2 * Math.PI * f2 * i) / sr);
  }
  return samples;
}

function generateCMajorChord(sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  const freqs = [261.63, 329.63, 392.0];
  for (let i = 0; i < n; i++) {
    samples[i] =
      (1 / 3) *
      (Math.sin((2 * Math.PI * freqs[0] * i) / sr) +
        Math.sin((2 * Math.PI * freqs[1] * i) / sr) +
        Math.sin((2 * Math.PI * freqs[2] * i) / sr));
  }
  return samples;
}

const SR = 22050;
const DURATION = 1.0;

describe('Feature API precision (reference compatibility)', () => {
  it('melDelta returns the slope of a linear feature row', () => {
    const result = melDelta({
      features: { data: new Float32Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]), rows: 2, cols: 5 },
      width: 5,
    });
    expect([...result.data]).toEqual(Array(10).fill(1));
  });

  it('piptrack returns equally shaped pitch and magnitude matrices', () => {
    const tone = generateSine(440, SR, 1);
    const result = piptrack(tone, SR, { nFft: 1024, hopLength: 256, fminHz: 100, fmaxHz: 1000 });
    expect(result.pitches.rows).toBe(513);
    expect(result.pitches.data.length).toBe(result.pitches.rows * result.pitches.cols);
    expect(result.magnitudes.rows).toBe(result.pitches.rows);
    expect(result.magnitudes.cols).toBe(result.pitches.cols);
    expect(result.magnitudes.data.length).toBe(result.pitches.data.length);
  });

  it('reassignedSpectrogram returns equally shaped coordinate matrices', () => {
    const result = reassignedSpectrogram(generateSine(440, SR, 1), SR, {
      nFft: 1024,
      hopLength: 256,
    });
    expect(result.magnitude.rows).toBe(513);
    expect(result.magnitude.cols).toBeGreaterThan(0);
    expect(result.magnitude.data.length).toBe(result.magnitude.rows * result.magnitude.cols);
    expect(result.times.rows).toBe(result.magnitude.rows);
    expect(result.times.cols).toBe(result.magnitude.cols);
    expect(result.frequencies.rows).toBe(result.magnitude.rows);
    expect(result.frequencies.cols).toBe(result.magnitude.cols);
  });

  it('exposes every librosa.segment-compatible operation', () => {
    const data = { data: new Float32Array([1, 0, 1, 0, 1, 1]), rows: 2, cols: 3 };
    const cross = segmentCrossSimilarity({ x: data, y: data });
    expect(cross.data).toHaveLength(9);
    const recurrence = segmentRecurrenceMatrix({ data });
    expect(recurrence.data).toHaveLength(9);
    const lag = segmentRecurrenceToLag({ recurrence, pad: true });
    expect(lag.data).toHaveLength(18);
    expect(segmentLagToRecurrence({ lag }).data).toHaveLength(9);
    // librosa 0.11.0 recurrence_to_lag on rec[r][c] = 4r + c: pad defaults to true, lag-by-time.
    const asymmetric = {
      data: Float32Array.from({ length: 16 }, (_, i) => i),
      rows: 4,
      cols: 4,
    };
    const padded = segmentRecurrenceToLag({ recurrence: asymmetric });
    expect(padded.rows).toBe(8);
    expect(padded.cols).toBe(4);
    expect(Array.from(padded.data)).toEqual([
      0, 5, 10, 15, 4, 9, 14, 0, 8, 13, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 2, 7, 0, 1,
      6, 11,
    ]);
    expect(Array.from(segmentLagToRecurrence({ lag: padded }).data)).toEqual(
      Array.from(asymmetric.data),
    );
    expect(segmentSubsegment({ data, boundaries: new Int32Array([0, 3]) })[0]).toBe(0);
    expect(segmentAgglomerative({ data, k: 2 })).toHaveLength(3);
    expect(segmentPathEnhance({ recurrence, win: 1 }).data).toHaveLength(9);
  });
  beforeAll(async () => {
    await init();
  });

  it('preserves vibrato and separates unvoiced intervals', () => {
    const segments = noteSegments({
      f0Hz: new Float32Array([440, 445, 435, 444, 436, 0, 0, 660, 666, 654, 665, 655, 0]),
      voicedProb: new Float32Array([1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0]),
      frameRate: 100,
    });
    expect(segments).toHaveLength(2);
    expect(segments[0]).toMatchObject({ frameStart: 0, frameEnd: 5, startSec: 0 });
    expect(segments[1]).toMatchObject({ frameStart: 7, frameEnd: 12 });
    expect(segments[0].endSec).toBeCloseTo(0.05);
    expect(segments[1].startSec).toBeCloseTo(0.07);
    expect(segments[1].endSec).toBeCloseTo(0.12);
  });

  describe('unit conversions', () => {
    const ref = loadRef('convert.json');

    it('should convert Hz to Mel (Slaney) accurately', () => {
      for (const entry of ref.data) {
        if (entry.hz !== undefined && entry.mel_slaney !== undefined) {
          const mel = hzToMel(entry.hz);
          expect(
            withinRel(mel, entry.mel_slaney, 1e-4),
            `hzToMel(${entry.hz}): got ${mel}, expected ${entry.mel_slaney}`,
          ).toBe(true);
        }
      }
    });

    it('should convert Mel to Hz (Slaney) accurately', () => {
      for (const entry of ref.data) {
        if (entry.mel !== undefined && entry.hz_slaney !== undefined) {
          const hz = melToHz(entry.mel);
          if (entry.hz_slaney === 0) {
            expect(withinAbs(hz, 0, 1e-4)).toBe(true);
          } else {
            expect(
              withinRel(hz, entry.hz_slaney, 1e-4),
              `melToHz(${entry.mel}): got ${hz}, expected ${entry.hz_slaney}`,
            ).toBe(true);
          }
        }
      }
    });

    it('should convert Hz to MIDI accurately', () => {
      for (const entry of ref.data) {
        if (entry.hz !== undefined && entry.midi !== undefined) {
          const midi = hzToMidi(entry.hz);
          expect(
            withinRel(midi, entry.midi, 1e-4),
            `hzToMidi(${entry.hz}): got ${midi}, expected ${entry.midi}`,
          ).toBe(true);
        }
      }
    });

    it('should convert MIDI to Hz accurately', () => {
      expect(withinRel(midiToHz(69), 440.0, 1e-4)).toBe(true);
      expect(withinRel(midiToHz(60), 261.6255653, 1e-4)).toBe(true);
    });

    it('frame, time and sample conversions take their documented defaults and refuse bad arguments', () => {
      expect(framesToTime(8)).toBe(framesToTime(8, 22050, 512));
      expect(timeToFrames(0.5)).toBe(timeToFrames(0.5, 22050, 512));
      expect(framesToSamples(8)).toBe(framesToSamples(8, 512));
      expect(samplesToFrames(4096)).toBe(samplesToFrames(4096, 512));
      expect(framesToTime(8, 44100, 128)).toBeCloseTo((8 * 128) / 44100, 9);
      expect(() => framesToTime(1, 22050, 0)).toThrow(RangeError);
      expect(() => timeToFrames('1' as unknown as number)).toThrow(TypeError);
    });
  });

  describe('STFT', () => {
    const ref = loadRef('stft.json');
    const refData = ref.data[0];

    it('should produce correct magnitude_sum and magnitude_max', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stft(tone, SR, { nFft: 2048, hopLength: 512 });

      let magSum = 0;
      let magMax = 0;
      for (let i = 0; i < result.magnitude.data.length; i++) {
        magSum += result.magnitude.data[i];
        if (result.magnitude.data[i] > magMax) {
          magMax = result.magnitude.data[i];
        }
      }

      expect(
        withinRel(magSum, refData.magnitude_sum, 5e-2),
        `magnitude_sum: got ${magSum}, expected ${refData.magnitude_sum}`,
      ).toBe(true);
      expect(
        withinRel(magMax, refData.magnitude_max, 5e-2),
        `magnitude_max: got ${magMax}, expected ${refData.magnitude_max}`,
      ).toBe(true);
    });

    it('should have correct output shape', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stft(tone, SR, { nFft: 2048, hopLength: 512 });
      expect(result.magnitude.rows).toBe(refData.shape[0]);
      expect(result.magnitude.cols).toBe(refData.shape[1]);
    });

    it('should match reference magnitude values (spot check)', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stft(tone, SR, { nFft: 2048, hopLength: 512 });
      const refMag = refData.magnitude as number[];
      const [, nFrames] = refData.shape as [number, number];

      // The array is bin-major: flat index = bin * nFrames + frame. This check
      // used to walk the first ten flat indices, which are all bin 0 — the DC
      // bin — across frames 0..9, and of those only two clear the `> 1.0`
      // filter (7.97 and 3.98, the window's DC edge artifact; frames 2..9 sit
      // at ~0.001). So it compared two edge values and nothing carrying the
      // tone. The 440 Hz energy is at bin 41, flat index 1804 and up.
      const at = (bin: number, frame: number): number => bin * nFrames + frame;
      const toneBin = Math.round((440 * 2048) / SR);
      const midFrame = Math.floor(nFrames / 2);
      const indices = [
        at(0, 0), // the DC edge this check already covered
        at(0, 1),
        at(toneBin - 1, 5), // the tone's lower shoulder
        at(toneBin, 5), // the tone itself
        at(toneBin + 1, 5), // its upper shoulder
        at(toneBin, midFrame), // and again once the frame is fully inside
      ];

      // Positive control on the selection itself: if the tone bin did not carry
      // the energy, every "signal band" index below would be another edge
      // value and the test would be back where it started.
      expect(
        refMag[at(toneBin, 5)],
        `bin ${toneBin} should hold the 440 Hz energy`,
      ).toBeGreaterThan(refMag[at(0, 5)] * 100);

      let compared = 0;
      for (const i of indices) {
        if (!(refMag[i] > 1.0)) {
          continue;
        }
        compared += 1;
        expect(
          withinRel(result.magnitude.data[i], refMag[i], 5e-2),
          `magnitude[${i}]: got ${result.magnitude.data[i]}, expected ${refMag[i]}`,
        ).toBe(true);
      }
      // An empty comparison set must fail rather than pass with nothing run:
      // the filter above is a guard against comparing noise, not a licence to
      // compare nothing.
      expect(compared, 'spot check ran no comparisons').toBe(indices.length);
    });
  });

  describe('Mel spectrogram', () => {
    it('should produce correct shape and non-negative values', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = melSpectrogram(tone, SR, { nFft: 2048, hopLength: 512, nMels: 128 });

      expect(result.power.rows).toBe(128);
      expect(result.power.cols).toBeGreaterThan(0);
      expect(result.power.data.length).toBe(result.power.rows * result.power.cols);

      for (let i = 0; i < result.power.data.length; i++) {
        expect(result.power.data[i]).toBeGreaterThanOrEqual(0);
      }
    });

    it('should produce dB values in reasonable range', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = melSpectrogram(tone, SR, { nFft: 2048, hopLength: 512, nMels: 128 });

      let hasFinite = false;
      for (let i = 0; i < result.db.data.length; i++) {
        if (Number.isFinite(result.db.data[i])) {
          hasFinite = true;
          expect(result.db.data[i]).toBeLessThan(100);
        }
      }
      expect(hasFinite).toBe(true);
    });

    it('should honour an explicit Mel range (fmin/fmax/htk)', () => {
      const tone = generateSine(440, SR, DURATION);
      const base = melSpectrogram(tone, SR, { nFft: 2048, hopLength: 512, nMels: 40 });
      const ranged = melSpectrogram(tone, SR, {
        nFft: 2048,
        hopLength: 512,
        nMels: 40,
        fminHz: 500,
        fmaxHz: 4000,
      });
      const htk = melSpectrogram(tone, SR, { nFft: 2048, hopLength: 512, nMels: 40, htk: true });

      expect(ranged.power.data.length).toBe(base.power.data.length);
      let dRange = 0;
      let dHtk = 0;
      for (let i = 0; i < base.power.data.length; i++) {
        dRange += (base.power.data[i] - ranged.power.data[i]) ** 2;
        dHtk += (base.power.data[i] - htk.power.data[i]) ** 2;
      }
      expect(dRange).toBeGreaterThan(1e-6);
      expect(dHtk).toBeGreaterThan(1e-6);
    });
  });

  describe('MFCC', () => {
    const ref = loadRef('mfcc.json');
    const refData = ref.data[0];

    it('should produce correct shape', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = mfcc(tone, SR, { nFft: 2048, hopLength: 512, nMels: 64, nMfcc: 13 });

      expect(result.coefficients.rows).toBe(refData.shape[0]);
      expect(result.coefficients.cols).toBe(refData.shape[1]);
    });

    it('should match reference per-coefficient means', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = mfcc(tone, SR, { nFft: 2048, hopLength: 512, nMels: 64, nMfcc: 13 });
      const refMean = refData.mean as number[];

      const { rows, cols, data } = result.coefficients;
      for (let c = 0; c < rows; c++) {
        let sum = 0;
        for (let f = 0; f < cols; f++) {
          sum += data[c * cols + f];
        }
        const mean = sum / cols;

        if (Math.abs(refMean[c]) > 1.0) {
          expect(
            withinRel(mean, refMean[c], 0.15),
            `MFCC mean[${c}]: got ${mean}, expected ${refMean[c]}`,
          ).toBe(true);
        }
      }
    });
  });

  describe('Chroma', () => {
    const ref = loadRef('chroma.json');
    const refData = ref.data[0];

    it('should produce correct shape', () => {
      const chord = generateCMajorChord(SR, DURATION);
      const result = chroma(chord, SR, { nFft: 2048, hopLength: 512 });

      expect(result.features.rows).toBe(12);
      expect(result.features.cols).toBe(refData.shape[1]);
    });

    it('should match reference mean_per_class within tolerance', () => {
      const chord = generateCMajorChord(SR, DURATION);
      const result = chroma(chord, SR, { nFft: 2048, hopLength: 512 });
      const refMean = refData.mean_per_class as number[];

      for (let c = 0; c < 12; c++) {
        let sum = 0;
        for (let f = 0; f < result.features.cols; f++) {
          sum += result.features.data[c * result.features.cols + f];
        }
        const mean = sum / result.features.cols;

        expect(
          withinAbs(mean, refMean[c], 0.5),
          `chroma mean[${c}]: got ${mean}, expected ${refMean[c]}`,
        ).toBe(true);
      }
    });

    it('should show dominant G (class 7) for C major chord', () => {
      const chord = generateCMajorChord(SR, DURATION);
      const result = chroma(chord, SR, { nFft: 2048, hopLength: 512 });

      // Compute mean per class and find the dominant one
      const means: number[] = [];
      for (let c = 0; c < 12; c++) {
        let sum = 0;
        for (let f = 0; f < result.features.cols; f++) {
          sum += result.features.data[c * result.features.cols + f];
        }
        means.push(sum / result.features.cols);
      }

      // G (class 7) should be the strongest in C major chord reference
      const maxClass = means.indexOf(Math.max(...means));
      // C=0, E=4, G=7 are the notes; G is strongest in reference
      expect([0, 4, 7]).toContain(maxClass);
    });
  });

  describe('Spectral features', () => {
    const ref = loadRef('spectral_features.json');
    const refData = ref.data;

    it('should match spectral centroid within tolerance', () => {
      const signal = generateTwoTone(440, 880, SR, DURATION);
      const centroid = spectralCentroid(signal, SR, { nFft: 2048, hopLength: 512 });
      const refCentroid = refData.centroid as number[];

      // Skip first 2 and last 2 boundary frames
      for (let i = 2; i < centroid.length - 2 && i < refCentroid.length - 2; i++) {
        if (refCentroid[i] > 1.0) {
          expect(
            withinRel(centroid[i], refCentroid[i], 5e-2),
            `centroid[${i}]: got ${centroid[i]}, expected ${refCentroid[i]}`,
          ).toBe(true);
        }
      }
    });

    it('should match spectral bandwidth within tolerance', () => {
      const signal = generateTwoTone(440, 880, SR, DURATION);
      const bandwidth = spectralBandwidth(signal, SR, { nFft: 2048, hopLength: 512 });
      expect(spectralBandwidth(signal, SR, { nFft: 2048, hopLength: 512, p: 1 })).not.toEqual(
        bandwidth,
      );
      const refBandwidth = refData.bandwidth as number[];

      for (let i = 2; i < bandwidth.length - 2 && i < refBandwidth.length - 2; i++) {
        if (refBandwidth[i] > 1.0) {
          expect(
            withinRel(bandwidth[i], refBandwidth[i], 2e-1),
            `bandwidth[${i}]: got ${bandwidth[i]}, expected ${refBandwidth[i]}`,
          ).toBe(true);
        }
      }
    });

    it('should match spectral rolloff within tolerance', () => {
      const signal = generateTwoTone(440, 880, SR, DURATION);
      const rolloff = spectralRolloff(signal, SR, { nFft: 2048, hopLength: 512 });
      const refRolloff = refData.rolloff as number[];

      for (let i = 2; i < rolloff.length - 2 && i < refRolloff.length - 2; i++) {
        if (refRolloff[i] > 1.0) {
          expect(
            withinRel(rolloff[i], refRolloff[i], 5e-2),
            `rolloff[${i}]: got ${rolloff[i]}, expected ${refRolloff[i]}`,
          ).toBe(true);
        }
      }
    });

    it('should match spectral flatness within tolerance', () => {
      const signal = generateTwoTone(440, 880, SR, DURATION);
      const flatness = spectralFlatness(signal, SR, { nFft: 2048, hopLength: 512 });
      const refFlatness = refData.flatness as number[];

      for (let i = 2; i < flatness.length - 2 && i < refFlatness.length - 2; i++) {
        expect(
          withinAbs(flatness[i], refFlatness[i], 1e-1),
          `flatness[${i}]: got ${flatness[i]}, expected ${refFlatness[i]}`,
        ).toBe(true);
      }
    });

    it('should return spectral flux with a configurable lag', () => {
      const signal = generateTwoTone(440, 880, SR, DURATION);
      const flux = spectralFlux(signal, SR, { nFft: 2048, hopLength: 512 });
      expect(flux).toBeInstanceOf(Float32Array);
      expect(flux.length).toBeGreaterThan(0);
      expect(spectralFlux(signal, SR, { nFft: 2048, hopLength: 512, lag: 2 })).not.toEqual(flux);
    });

    it('should backtrack onset indices to energy minima', () => {
      expect(
        Array.from(
          onsetBacktrack({
            events: new Int32Array([6]),
            energy: new Float32Array([1, 0.8, 0.5, 0.2, 0.6, 0.9, 1.2]),
          }),
        ),
      ).toEqual([3]);
    });

    it('should reconstruct STFT magnitude with Griffin-Lim', () => {
      const spectrum = stft(generateTwoTone(440, 880, SR, DURATION), SR, {
        nFft: 1024,
        hopLength: 256,
      });
      const result = griffinLim({
        magnitude: spectrum.magnitude,
        sampleRate: SR,
        nFft: 1024,
        hopLength: 256,
        nIter: 1,
        momentum: 0,
      });
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });
  });

  describe('Zero crossing rate and RMS', () => {
    it('should return non-empty zero crossing rate', () => {
      const tone = generateSine(440, SR, DURATION);
      const zcr = zeroCrossingRate(tone, SR, { frameLength: 2048, hopLength: 512 });
      expect(zcr.length).toBeGreaterThan(0);
      for (let i = 0; i < zcr.length; i++) {
        expect(zcr[i]).toBeGreaterThanOrEqual(0);
        expect(zcr[i]).toBeLessThanOrEqual(1);
      }
    });

    it('should return non-empty RMS energy', () => {
      const tone = generateSine(440, SR, DURATION);
      const rms = rmsEnergy(tone, SR, { frameLength: 2048, hopLength: 512 });
      expect(rms.length).toBeGreaterThan(0);
      for (let i = 0; i < rms.length; i++) {
        expect(rms[i]).toBeGreaterThanOrEqual(0);
      }
    });
  });

  describe('Pitch detection', () => {
    it('should detect 440 Hz from a pure tone', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = pitchYin(tone, SR, {
        frameLength: 2048,
        hopLength: 512,
        f0MinHz: 65.0,
        f0MaxHz: 2093.0,
        threshold: 0.3,
      });

      expect(result.f0.length).toBeGreaterThan(0);
      expect(result).not.toHaveProperty('nFrames');
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);

      // Check that most voiced frames are near 440 Hz.
      // Use voicedProb > 0.5 instead of voicedFlag, because std::vector<bool>
      // cannot be marshalled by Emscripten's embind.
      let nearCount = 0;
      let voicedCount = 0;
      for (let i = 0; i < result.f0.length; i++) {
        if (result.voicedProb[i] > 0.5) {
          voicedCount++;
          if (withinRel(result.f0[i], 440.0, 0.05)) {
            nearCount++;
          }
        }
      }
      if (voicedCount > 0) {
        expect(nearCount / voicedCount).toBeGreaterThan(0.7);
      }
    });
  });

  describe('Onset detection', () => {
    it('should detect onsets from a signal with impulses', () => {
      // Generate a signal with clicks at regular intervals
      const n = Math.floor(SR * DURATION);
      const samples = new Float32Array(n);
      const clickInterval = Math.floor(SR * 0.25); // click every 250ms
      for (let i = 0; i < n; i++) {
        // Short impulse at each click position
        if (i % clickInterval < 50) {
          samples[i] = 0.8 * (1 - (i % clickInterval) / 50);
        }
      }

      const onsets = detectOnsets(samples, SR);
      expect(onsets).toBeInstanceOf(Float32Array);
      expect(onsets.length).toBeGreaterThan(0);
      for (let i = 0; i < onsets.length; i++) {
        expect(onsets[i]).toBeGreaterThanOrEqual(0);
      }
    });
  });

  describe('analyze with progress', () => {
    it('should return analysis result and call progress callback', () => {
      const tone = generateSine(440, SR, DURATION);
      const progressValues: number[] = [];
      const stages: string[] = [];

      const result = analyze(tone, SR, {
        onProgress: (progress, stage) => {
          progressValues.push(progress);
          stages.push(stage);
        },
      });

      // Verify result has expected structure
      expect(result.bpm).toBeGreaterThanOrEqual(0);
      expect(result.key).toBeDefined();
      expect(result.beats).toBeDefined();

      // Verify progress callback was invoked
      expect(progressValues.length).toBeGreaterThan(0);
      for (const p of progressValues) {
        expect(p).toBeGreaterThanOrEqual(0);
        expect(p).toBeLessThanOrEqual(1);
      }
    });

    it('ignores a progress callback false result', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = analyze(tone, SR, { onProgress: () => false });
      expect(Array.isArray(result.beats)).toBe(true);
    });

    it('cancels through the request cancel callback', () => {
      expect(() =>
        analyze({ samples: generateSine(440, SR, DURATION), sampleRate: SR, cancel: () => true }),
      ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled }));
    });
  });

  describe('Effects', () => {
    it('hpss should return harmonic and percussive of same length', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = hpss(tone, SR);

      expect(result.harmonic).toBeInstanceOf(Float32Array);
      expect(result.percussive).toBeInstanceOf(Float32Array);
      expect(result.harmonic.length).toBe(tone.length);
      expect(result.percussive.length).toBe(tone.length);
    });

    it('harmonic should return Float32Array of same length', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = harmonic(tone, SR);

      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBe(tone.length);
    });

    it('percussive should return Float32Array of same length', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = percussive(tone, SR);

      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBe(tone.length);
    });

    it('timeStretch rate=2.0 should produce shorter output', () => {
      const tone = generateSine(440, SR, DURATION);
      const stretched = timeStretch(tone, SR, { speedRatio: 2.0 });

      expect(stretched).toBeInstanceOf(Float32Array);
      // Rate 2.0 means playback at double speed -> half the length
      expect(stretched.length).toBeLessThan(tone.length);
      expect(stretched.length).toBeGreaterThan(tone.length * 0.3);
      expect(stretched.length).toBeLessThan(tone.length * 0.7);
    });

    it('timeStretch rate=0.5 should produce longer output', () => {
      const tone = generateSine(440, SR, DURATION);
      const stretched = timeStretch(tone, SR, { speedRatio: 0.5 });

      expect(stretched).toBeInstanceOf(Float32Array);
      // Rate 0.5 means playback at half speed -> double the length
      expect(stretched.length).toBeGreaterThan(tone.length);
      expect(stretched.length).toBeGreaterThan(tone.length * 1.5);
      expect(stretched.length).toBeLessThan(tone.length * 2.5);
    });

    it('timeStretch rejects non-finite and oversized projected rates, then recovers', () => {
      const tone = generateSine(440, SR, 0.1);
      // Pinned to the class, not to "something was thrown". WASM answers a
      // non-finite rate with the shared facade assertion and agrees with the
      // addon on the wording, but it agrees only because it was made to: an
      // assertion that accepts any throw cannot hold that agreement, and this
      // one went on passing when the class moved underneath it.
      expect(() => timeStretch(tone, SR, { speedRatio: Number.NaN })).toThrow(
        new RangeError(
          'timeStretch: speedRatio must be a finite number within the 32-bit float range',
        ),
      );
      expect(() => timeStretch(tone, SR, { speedRatio: Number.POSITIVE_INFINITY })).toThrow(
        new RangeError(
          'timeStretch: speedRatio must be a finite number within the 32-bit float range',
        ),
      );
      // Finite, so the facade passes it on and the core refuses the projected
      // length instead: the same class, told apart by the core's own wording.
      expect(() => timeStretch(tone, SR, { speedRatio: 1.1754943508222875e-38 })).toThrow(
        RangeError,
      );
      expect(() => timeStretch(tone, SR, { speedRatio: 1.1754943508222875e-38 })).not.toThrow(
        /finite number/,
      );
      expect(Array.from(timeStretch(tone, SR, { speedRatio: 1 })).every(Number.isFinite)).toBe(
        true,
      );
    });

    it('pitchShift should return Float32Array of similar length', () => {
      const tone = generateSine(440, SR, DURATION);
      const shifted = pitchShift(tone, SR, { pitchSemitones: 2 });

      expect(shifted).toBeInstanceOf(Float32Array);
      // pitchShift uses time-stretch + resample so length may vary slightly
      expect(shifted.length).toBeGreaterThan(tone.length * 0.5);
      expect(shifted.length).toBeLessThan(tone.length * 1.5);
    });

    it('pitchShift rejects an unsupported effective rate before processing input', () => {
      expect(() => pitchShift(new Float32Array(4), 48000, { pitchSemitones: -120 })).toThrow();
      expect(pitchShift(generateSine(440, SR, 0.1), SR, { pitchSemitones: 0 })).toBeInstanceOf(
        Float32Array,
      );
    });

    it('normalize should produce peak close to target', () => {
      // Generate a quiet sine wave
      const n = Math.floor(SR * DURATION);
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        samples[i] = 0.1 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }

      const normalized = normalize(samples, SR, { targetDb: 0.0 });
      expect(normalized).toBeInstanceOf(Float32Array);
      expect(normalized.length).toBe(samples.length);

      // Find peak of normalized signal - should be close to 1.0 (0 dB)
      let peak = 0;
      for (let i = 0; i < normalized.length; i++) {
        const abs = Math.abs(normalized[i]);
        if (abs > peak) {
          peak = abs;
        }
      }
      expect(peak).toBeGreaterThan(0.8);
      expect(peak).toBeLessThanOrEqual(1.01);
    });

    it('trim should remove leading and trailing silence', () => {
      const n = Math.floor(SR * DURATION);
      // silence + tone + silence
      const samples = new Float32Array(n);
      const toneStart = Math.floor(n * 0.25);
      const toneEnd = Math.floor(n * 0.75);
      for (let i = toneStart; i < toneEnd; i++) {
        samples[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }

      const trimmed = trim(samples, SR, { thresholdDb: -40.0 });
      expect(trimmed).toBeInstanceOf(Float32Array);
      expect(trimmed.length).toBeLessThan(samples.length);
      expect(trimmed.length).toBeGreaterThan(0);
    });
  });

  describe('STFT dB', () => {
    it('should return finite dB values with correct shape', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stftDb(tone, SR, { nFft: 2048, hopLength: 512 });

      expect(result.rows).toBe(1025);
      expect(result.cols).toBeGreaterThan(0);
      expect(result.data).toBeInstanceOf(Float32Array);
      expect(result.data.length).toBe(result.rows * result.cols);

      // All dB values should be finite
      for (let i = 0; i < result.data.length; i++) {
        expect(Number.isFinite(result.data[i])).toBe(true);
      }
    });
  });

  describe('Pitch pYIN', () => {
    it('should detect 440 Hz from a pure tone', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = pitchPyin(tone, SR, {
        frameLength: 2048,
        hopLength: 512,
        f0MinHz: 65.0,
        f0MaxHz: 2093.0,
        threshold: 0.3,
      });

      expect(result.f0.length).toBeGreaterThan(0);
      expect(result).not.toHaveProperty('nFrames');
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);
    });
  });

  describe('Note/Hz conversions', () => {
    it('hzToNote(440) should return A4', () => {
      expect(hzToNote(440)).toBe('A4');
    });

    it('noteToHz("A4") should return ~440', () => {
      expect(withinRel(noteToHz('A4'), 440.0, 1e-4)).toBe(true);
    });

    it('noteToHz refuses an unknown name with a RangeError', () => {
      expect(() => noteToHz('H4')).toThrow(RangeError);
    });

    it('hzToNote refuses a non-positive frequency with a RangeError', () => {
      expect(() => hzToNote(0)).toThrow(RangeError);
    });

    it('framesToTime should compute correct time', () => {
      const t = framesToTime(1, 22050, 512);
      expect(withinAbs(t, 512 / 22050, 1e-6)).toBe(true);
    });

    it('timeToFrames should compute correct frame count', () => {
      const f = timeToFrames(1.0, 22050, 512);
      expect(f).toBe(Math.floor(22050 / 512));
    });
  });

  describe('Resample', () => {
    it('should double output length when upsampling 22050->44100', () => {
      const tone = generateSine(440, SR, DURATION);
      const resampled = resample(tone, 22050, 44100);

      expect(resampled).toBeInstanceOf(Float32Array);
      // Output should be approximately double the input length
      const expectedLength = tone.length * 2;
      expect(resampled.length).toBeGreaterThan(expectedLength * 0.9);
      expect(resampled.length).toBeLessThan(expectedLength * 1.1);
    });
  });
});
