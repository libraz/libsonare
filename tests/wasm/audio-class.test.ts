/**
 * Tests for the Audio convenience class.
 *
 * The Audio class delegates to standalone functions which are thoroughly tested
 * elsewhere. These tests verify the class API works end-to-end: construction,
 * property getters, and that each method returns the expected type/shape.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { Audio, detectBpm, init, melSpectrogram, stft } from '../../js/index';

const SR = 22050;
const DURATION = 2;
const DURATION_LONG = 4;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('Audio class', () => {
  beforeAll(async () => {
    await init();
  });

  const sine = () => generateSine(440, SR, DURATION);
  const sineLong = () => generateSine(440, SR, DURATION_LONG);

  // -- Construction and property getters --

  describe('construction and getters', () => {
    it('should create an Audio instance via fromBuffer', () => {
      const samples = sine();
      const audio = Audio.fromBuffer(samples, SR);
      expect(audio).toBeInstanceOf(Audio);
    });

    it('should return correct data', () => {
      const samples = sine();
      const audio = Audio.fromBuffer(samples, SR);
      expect(audio.data).toBe(samples);
    });

    it('should return correct length', () => {
      const samples = sine();
      const audio = Audio.fromBuffer(samples, SR);
      expect(audio.length).toBe(samples.length);
    });

    it('should return correct sampleRate', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      expect(audio.sampleRate).toBe(SR);
    });

    it('should return correct duration', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      expect(audio.duration).toBeCloseTo(DURATION, 2);
    });
  });

  // -- Analysis methods --

  describe('analysis', () => {
    it('should detect BPM', () => {
      const audio = Audio.fromBuffer(sineLong(), SR);
      const bpm = audio.detectBpm();
      expect(bpm).toBeGreaterThan(0);
    });

    it('should detect key', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const key = audio.detectKey();
      expect(key.root).toBeDefined();
      expect(key.mode).toBeDefined();
      expect(key.confidence).toBeGreaterThanOrEqual(0);
      expect(key.confidence).toBeLessThanOrEqual(1);
      expect(key.name).toBeDefined();
    });

    it('should detect onsets', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const onsets = audio.detectOnsets();
      expect(onsets).toBeInstanceOf(Float32Array);
    });

    it('should detect beats', () => {
      const audio = Audio.fromBuffer(sineLong(), SR);
      const beats = audio.detectBeats();
      expect(beats).toBeInstanceOf(Float32Array);
    });

    it('should return complete analysis result', { timeout: 30000 }, () => {
      const audio = Audio.fromBuffer(sineLong(), SR);
      const result = audio.analyze();
      expect(result.bpm).toBeGreaterThan(0);
      expect(result.key).toBeDefined();
      expect(result.timeSignature).toBeDefined();
      expect(result.beats).toBeDefined();
      expect(result.chords).toBeDefined();
      expect(result.sections).toBeDefined();
      expect(result.timbre).toBeDefined();
      expect(result.dynamics).toBeDefined();
    });

    it('should analyze with progress callback', { timeout: 30000 }, () => {
      const audio = Audio.fromBuffer(sineLong(), SR);
      const progressValues: number[] = [];
      const stages: string[] = [];
      const result = audio.analyzeWithProgress((progress, stage) => {
        progressValues.push(progress);
        stages.push(stage);
      });
      expect(result.bpm).toBeGreaterThan(0);
      expect(progressValues.length).toBeGreaterThan(0);
      expect(stages.length).toBeGreaterThan(0);
    });
  });

  // -- Effects methods --

  describe('effects', () => {
    it('should perform HPSS', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.hpss();
      expect(result.harmonic).toBeInstanceOf(Float32Array);
      expect(result.percussive).toBeInstanceOf(Float32Array);
      expect(result.harmonic.length).toBe(audio.length);
      expect(result.percussive.length).toBe(audio.length);
    });

    it('should extract harmonic component', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const h = audio.harmonic();
      expect(h).toBeInstanceOf(Float32Array);
      expect(h.length).toBe(audio.length);
    });

    it('should extract percussive component', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const p = audio.percussive();
      expect(p).toBeInstanceOf(Float32Array);
      expect(p.length).toBe(audio.length);
    });

    it('should time stretch', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const stretched = audio.timeStretch(1.5);
      expect(stretched).toBeInstanceOf(Float32Array);
      expect(stretched.length).toBeLessThan(audio.length);
    });

    it('should pitch shift', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const shifted = audio.pitchShift(2);
      expect(shifted).toBeInstanceOf(Float32Array);
      expect(shifted.length).toBeGreaterThan(0);
    });

    it('should normalize', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const normalized = audio.normalize(-3.0);
      expect(normalized).toBeInstanceOf(Float32Array);
      expect(normalized.length).toBe(audio.length);
    });

    it('should trim', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const trimmed = audio.trim(-60.0);
      expect(trimmed).toBeInstanceOf(Float32Array);
      expect(trimmed.length).toBeGreaterThan(0);
      expect(trimmed.length).toBeLessThanOrEqual(audio.length);
    });
  });

  // -- Feature methods --

  describe('features', () => {
    it('should compute STFT', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.stft();
      expect(result.nBins).toBeGreaterThan(0);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.magnitude).toBeInstanceOf(Float32Array);
      expect(result.power).toBeInstanceOf(Float32Array);
    });

    it('should compute STFT in dB', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.stftDb();
      expect(result.nBins).toBeGreaterThan(0);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.db).toBeInstanceOf(Float32Array);
    });

    it('should compute mel spectrogram', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.melSpectrogram();
      expect(result.nMels).toBe(128);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.power).toBeInstanceOf(Float32Array);
      expect(result.db).toBeInstanceOf(Float32Array);
    });

    it('should compute MFCC', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.mfcc();
      expect(result.nMfcc).toBe(13);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.coefficients).toBeInstanceOf(Float32Array);
    });

    it('should compute chroma', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.chroma();
      expect(result.nChroma).toBe(12);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.features).toBeInstanceOf(Float32Array);
    });

    it('should compute spectral centroid', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.spectralCentroid();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute spectral bandwidth', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.spectralBandwidth();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute spectral rolloff', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.spectralRolloff();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute spectral flatness', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.spectralFlatness();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute zero crossing rate', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.zeroCrossingRate();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute RMS energy', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.rmsEnergy();
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('should compute pitch via YIN', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.pitchYin();
      expect(result.f0).toBeInstanceOf(Float32Array);
      expect(result.voicedProb).toBeInstanceOf(Float32Array);
      expect(result.f0.length).toBeGreaterThan(0);
    });

    it('should compute pitch via pYIN', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.pitchPyin();
      expect(result.f0).toBeInstanceOf(Float32Array);
      expect(result.voicedProb).toBeInstanceOf(Float32Array);
      expect(result.f0.length).toBeGreaterThan(0);
    });
  });

  // -- Core methods --

  describe('core', () => {
    it('should resample', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const resampled = audio.resample(16000);
      expect(resampled).toBeInstanceOf(Float32Array);
      const expectedLength = Math.round(audio.length * (16000 / SR));
      // Allow some tolerance for resampling length
      expect(Math.abs(resampled.length - expectedLength)).toBeLessThan(100);
    });
  });

  // -- Consistency with standalone functions --

  describe('consistency with standalone functions', () => {
    it('should produce same STFT as standalone stft()', () => {
      const samples = sine();
      const audio = Audio.fromBuffer(samples, SR);

      const classResult = audio.stft();
      const standaloneResult = stft(samples, SR);

      expect(classResult.nBins).toBe(standaloneResult.nBins);
      expect(classResult.nFrames).toBe(standaloneResult.nFrames);
      expect(classResult.magnitude).toEqual(standaloneResult.magnitude);
      expect(classResult.power).toEqual(standaloneResult.power);
    });

    it('should produce same mel spectrogram as standalone melSpectrogram()', () => {
      const samples = sine();
      const audio = Audio.fromBuffer(samples, SR);

      const classResult = audio.melSpectrogram();
      const standaloneResult = melSpectrogram(samples, SR);

      expect(classResult.nMels).toBe(standaloneResult.nMels);
      expect(classResult.nFrames).toBe(standaloneResult.nFrames);
      expect(classResult.power).toEqual(standaloneResult.power);
      expect(classResult.db).toEqual(standaloneResult.db);
    });

    it('should produce same BPM as standalone detectBpm()', () => {
      const samples = sineLong();
      const audio = Audio.fromBuffer(samples, SR);

      const classBpm = audio.detectBpm();
      const standaloneBpm = detectBpm(samples, SR);

      expect(classBpm).toBe(standaloneBpm);
    });
  });
});
