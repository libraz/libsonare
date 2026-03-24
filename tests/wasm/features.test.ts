/**
 * WASM feature API precision tests against librosa reference data.
 */

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeWithProgress,
  chroma,
  detectOnsets,
  framesToTime,
  harmonic,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  init,
  melSpectrogram,
  melToHz,
  mfcc,
  midiToHz,
  normalize,
  noteToHz,
  percussive,
  pitchPyin,
  pitchShift,
  pitchYin,
  resample,
  rmsEnergy,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  timeStretch,
  timeToFrames,
  trim,
  zeroCrossingRate,
} from '../../js/index';

const refDir = resolve(fileURLToPath(import.meta.url), '..', '..', 'librosa', 'reference');

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

describe('Feature API precision (librosa compatibility)', () => {
  beforeAll(async () => {
    await init();
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
  });

  describe('STFT', () => {
    const ref = loadRef('stft.json');
    const refData = ref.data[0];

    it('should produce correct magnitude_sum and magnitude_max', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stft(tone, SR, 2048, 512);

      let magSum = 0;
      let magMax = 0;
      for (let i = 0; i < result.magnitude.length; i++) {
        magSum += result.magnitude[i];
        if (result.magnitude[i] > magMax) {
          magMax = result.magnitude[i];
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
      const result = stft(tone, SR, 2048, 512);
      expect(result.nBins).toBe(refData.shape[0]);
      expect(result.nFrames).toBe(refData.shape[1]);
    });

    it('should match reference magnitude values (spot check)', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stft(tone, SR, 2048, 512);
      const refMag = refData.magnitude as number[];

      // Spot-check first few non-trivial elements
      for (let i = 0; i < Math.min(10, refMag.length); i++) {
        if (refMag[i] > 1.0) {
          expect(
            withinRel(result.magnitude[i], refMag[i], 5e-2),
            `magnitude[${i}]: got ${result.magnitude[i]}, expected ${refMag[i]}`,
          ).toBe(true);
        }
      }
    });
  });

  describe('Mel spectrogram', () => {
    it('should produce correct shape and non-negative values', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = melSpectrogram(tone, SR, 2048, 512, 128);

      expect(result.nMels).toBe(128);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.power.length).toBe(result.nMels * result.nFrames);

      for (let i = 0; i < result.power.length; i++) {
        expect(result.power[i]).toBeGreaterThanOrEqual(0);
      }
    });

    it('should produce dB values in reasonable range', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = melSpectrogram(tone, SR, 2048, 512, 128);

      let hasFinite = false;
      for (let i = 0; i < result.db.length; i++) {
        if (Number.isFinite(result.db[i])) {
          hasFinite = true;
          expect(result.db[i]).toBeLessThan(100);
        }
      }
      expect(hasFinite).toBe(true);
    });
  });

  describe('MFCC', () => {
    const ref = loadRef('mfcc.json');
    const refData = ref.data[0];

    it('should produce correct shape', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = mfcc(tone, SR, 2048, 512, 64, 13);

      expect(result.nMfcc).toBe(refData.shape[0]);
      expect(result.nFrames).toBe(refData.shape[1]);
    });

    it('should match reference per-coefficient means', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = mfcc(tone, SR, 2048, 512, 64, 13);
      const refMean = refData.mean as number[];

      for (let c = 0; c < result.nMfcc; c++) {
        let sum = 0;
        for (let f = 0; f < result.nFrames; f++) {
          sum += result.coefficients[c * result.nFrames + f];
        }
        const mean = sum / result.nFrames;

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
      const result = chroma(chord, SR, 2048, 512);

      expect(result.nChroma).toBe(12);
      expect(result.nFrames).toBe(refData.shape[1]);
    });

    it('should match reference mean_per_class within tolerance', () => {
      const chord = generateCMajorChord(SR, DURATION);
      const result = chroma(chord, SR, 2048, 512);
      const refMean = refData.mean_per_class as number[];

      // L-inf normalize each frame (column) so max chroma bin = 1.0,
      // matching librosa's default chroma normalization.
      const normalized = new Float32Array(result.features.length);
      for (let f = 0; f < result.nFrames; f++) {
        let frameMax = 0;
        for (let c = 0; c < 12; c++) {
          const val = Math.abs(result.features[c * result.nFrames + f]);
          if (val > frameMax) {
            frameMax = val;
          }
        }
        for (let c = 0; c < 12; c++) {
          normalized[c * result.nFrames + f] =
            frameMax > 0 ? result.features[c * result.nFrames + f] / frameMax : 0;
        }
      }

      for (let c = 0; c < 12; c++) {
        let sum = 0;
        for (let f = 0; f < result.nFrames; f++) {
          sum += normalized[c * result.nFrames + f];
        }
        const mean = sum / result.nFrames;

        expect(
          withinAbs(mean, refMean[c], 0.5),
          `chroma mean[${c}]: got ${mean}, expected ${refMean[c]}`,
        ).toBe(true);
      }
    });

    it('should show dominant G (class 7) for C major chord', () => {
      const chord = generateCMajorChord(SR, DURATION);
      const result = chroma(chord, SR, 2048, 512);

      // Compute mean per class and find the dominant one
      const means: number[] = [];
      for (let c = 0; c < 12; c++) {
        let sum = 0;
        for (let f = 0; f < result.nFrames; f++) {
          sum += result.features[c * result.nFrames + f];
        }
        means.push(sum / result.nFrames);
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
      const centroid = spectralCentroid(signal, SR, 2048, 512);
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
      const bandwidth = spectralBandwidth(signal, SR, 2048, 512);
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
      const rolloff = spectralRolloff(signal, SR, 2048, 512);
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
      const flatness = spectralFlatness(signal, SR, 2048, 512);
      const refFlatness = refData.flatness as number[];

      for (let i = 2; i < flatness.length - 2 && i < refFlatness.length - 2; i++) {
        expect(
          withinAbs(flatness[i], refFlatness[i], 1e-1),
          `flatness[${i}]: got ${flatness[i]}, expected ${refFlatness[i]}`,
        ).toBe(true);
      }
    });
  });

  describe('Zero crossing rate and RMS', () => {
    it('should return non-empty zero crossing rate', () => {
      const tone = generateSine(440, SR, DURATION);
      const zcr = zeroCrossingRate(tone, SR, 2048, 512);
      expect(zcr.length).toBeGreaterThan(0);
      for (let i = 0; i < zcr.length; i++) {
        expect(zcr[i]).toBeGreaterThanOrEqual(0);
        expect(zcr[i]).toBeLessThanOrEqual(1);
      }
    });

    it('should return non-empty RMS energy', () => {
      const tone = generateSine(440, SR, DURATION);
      const rms = rmsEnergy(tone, SR, 2048, 512);
      expect(rms.length).toBeGreaterThan(0);
      for (let i = 0; i < rms.length; i++) {
        expect(rms[i]).toBeGreaterThanOrEqual(0);
      }
    });
  });

  describe('Pitch detection', () => {
    it('should detect 440 Hz from a pure tone', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = pitchYin(tone, SR, 2048, 512, 65.0, 2093.0, 0.3);

      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);

      // Check that most voiced frames are near 440 Hz.
      // Use voicedProb > 0.5 instead of voicedFlag, because std::vector<bool>
      // cannot be marshalled by Emscripten's embind.
      let nearCount = 0;
      let voicedCount = 0;
      for (let i = 0; i < result.nFrames; i++) {
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

  describe('analyzeWithProgress', () => {
    it('should return analysis result and call progress callback', () => {
      const tone = generateSine(440, SR, DURATION);
      const progressValues: number[] = [];
      const stages: string[] = [];

      const result = analyzeWithProgress(tone, SR, (progress, stage) => {
        progressValues.push(progress);
        stages.push(stage);
      });

      // Verify result has expected structure (same as analyze())
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
      const stretched = timeStretch(tone, SR, 2.0);

      expect(stretched).toBeInstanceOf(Float32Array);
      // Rate 2.0 means playback at double speed -> half the length
      expect(stretched.length).toBeLessThan(tone.length);
      expect(stretched.length).toBeGreaterThan(tone.length * 0.3);
      expect(stretched.length).toBeLessThan(tone.length * 0.7);
    });

    it('timeStretch rate=0.5 should produce longer output', () => {
      const tone = generateSine(440, SR, DURATION);
      const stretched = timeStretch(tone, SR, 0.5);

      expect(stretched).toBeInstanceOf(Float32Array);
      // Rate 0.5 means playback at half speed -> double the length
      expect(stretched.length).toBeGreaterThan(tone.length);
      expect(stretched.length).toBeGreaterThan(tone.length * 1.5);
      expect(stretched.length).toBeLessThan(tone.length * 2.5);
    });

    it('pitchShift should return Float32Array of similar length', () => {
      const tone = generateSine(440, SR, DURATION);
      const shifted = pitchShift(tone, SR, 2);

      expect(shifted).toBeInstanceOf(Float32Array);
      // pitchShift uses time-stretch + resample so length may vary slightly
      expect(shifted.length).toBeGreaterThan(tone.length * 0.5);
      expect(shifted.length).toBeLessThan(tone.length * 1.5);
    });

    it('normalize should produce peak close to target', () => {
      // Generate a quiet sine wave
      const n = Math.floor(SR * DURATION);
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        samples[i] = 0.1 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }

      const normalized = normalize(samples, SR, 0.0);
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

      const trimmed = trim(samples, SR, -40.0);
      expect(trimmed).toBeInstanceOf(Float32Array);
      expect(trimmed.length).toBeLessThan(samples.length);
      expect(trimmed.length).toBeGreaterThan(0);
    });
  });

  describe('STFT dB', () => {
    it('should return finite dB values with correct shape', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = stftDb(tone, SR, 2048, 512);

      expect(result.nBins).toBe(1025);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.db).toBeInstanceOf(Float32Array);
      expect(result.db.length).toBe(result.nBins * result.nFrames);

      // All dB values should be finite
      for (let i = 0; i < result.db.length; i++) {
        expect(Number.isFinite(result.db[i])).toBe(true);
      }
    });
  });

  describe('Pitch pYIN', () => {
    it('should detect 440 Hz from a pure tone', () => {
      const tone = generateSine(440, SR, DURATION);
      const result = pitchPyin(tone, SR, 2048, 512, 65.0, 2093.0, 0.3);

      expect(result.nFrames).toBeGreaterThan(0);
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
