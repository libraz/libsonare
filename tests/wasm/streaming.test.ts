/**
 * StreamAnalyzer WASM module tests
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, StreamAnalyzer } from '../../js/index';

describe('StreamAnalyzer', () => {
  beforeAll(async () => {
    await init();
  });

  describe('chord progression', () => {
    it('should detect chord changes over time', () => {
      const sampleRate = 22050;
      const duration = 8;
      const samples = new Float32Array(sampleRate * duration);

      // Generate chord progression: C -> G -> Am -> F
      const chords = [
        { freqs: [261.63, 329.63, 392.0], start: 0, end: 2 }, // C major
        { freqs: [392.0, 493.88, 587.33], start: 2, end: 4 }, // G major
        { freqs: [220.0, 261.63, 329.63], start: 4, end: 6 }, // A minor
        { freqs: [349.23, 440.0, 523.25], start: 6, end: 8 }, // F major
      ];

      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        let val = 0;
        for (const chord of chords) {
          if (t >= chord.start && t < chord.end) {
            for (const freq of chord.freqs) {
              val += Math.sin(2 * Math.PI * freq * t) / chord.freqs.length;
            }
            break;
          }
        }
        samples[i] = val * 0.3;
      }

      const analyzer = new StreamAnalyzer({ sampleRate });

      // Process in chunks
      const chunkSize = 4096;
      for (let i = 0; i < samples.length; i += chunkSize) {
        const chunk = samples.slice(i, Math.min(i + chunkSize, samples.length));
        analyzer.process(chunk);
      }

      const stats = analyzer.stats();

      // Verify chord progression was detected
      expect(stats.estimate.chordProgression.length).toBeGreaterThan(0);

      // Each chord change should have valid data
      for (const chord of stats.estimate.chordProgression) {
        expect(chord.root).toBeGreaterThanOrEqual(0);
        expect(chord.root).toBeLessThanOrEqual(11);
        expect(chord.quality).toBeGreaterThanOrEqual(0);
        expect(chord.startTime).toBeGreaterThanOrEqual(0);
        expect(chord.confidence).toBeGreaterThanOrEqual(0);
      }

      analyzer.dispose();
    });

    it('should detect current chord', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Generate C major chord
      const samples = new Float32Array(sampleRate * 2);
      const freqs = [261.63, 329.63, 392.0]; // C-E-G
      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        for (const freq of freqs) {
          samples[i] += Math.sin(2 * Math.PI * freq * t) / freqs.length;
        }
        samples[i] *= 0.3;
      }

      analyzer.process(samples);
      const stats = analyzer.stats();

      // Current chord should be valid
      expect(stats.estimate.chordRoot).toBeGreaterThanOrEqual(0);
      expect(stats.estimate.chordRoot).toBeLessThanOrEqual(11);
      expect(stats.estimate.chordConfidence).toBeGreaterThan(0);

      analyzer.dispose();
    });
  });

  describe('progressive estimation', () => {
    it('should estimate BPM and key progressively', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Generate a simple signal
      const samples = new Float32Array(sampleRate * 4);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5;
      }

      analyzer.process(samples);
      const stats = analyzer.stats();

      expect(stats.durationSeconds).toBeGreaterThan(3);
      expect(stats.totalFrames).toBeGreaterThan(0);
      expect(stats.estimate.accumulatedSeconds).toBeGreaterThan(0);

      analyzer.dispose();
    });

    it('should track key with confidence', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Generate C major scale
      const notes = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88];
      const samples = new Float32Array(sampleRate * 7);

      for (let n = 0; n < notes.length; n++) {
        const start = n * sampleRate;
        for (let i = 0; i < sampleRate; i++) {
          samples[start + i] = Math.sin((2 * Math.PI * notes[n] * i) / sampleRate) * 0.5;
        }
      }

      analyzer.process(samples);
      const stats = analyzer.stats();

      expect(stats.estimate.key).toBeGreaterThanOrEqual(0);
      expect(stats.estimate.key).toBeLessThanOrEqual(11);
      expect(stats.estimate.keyConfidence).toBeGreaterThanOrEqual(0);

      analyzer.dispose();
    });
  });

  describe('state management', () => {
    it('should reset state correctly', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Process some audio
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5;
      }
      analyzer.process(samples);

      expect(analyzer.frameCount()).toBeGreaterThan(0);
      expect(analyzer.currentTime()).toBeGreaterThan(0);

      // Reset
      analyzer.reset();

      expect(analyzer.frameCount()).toBe(0);

      const stats = analyzer.stats();
      expect(stats.estimate.chordProgression.length).toBe(0);

      analyzer.dispose();
    });

    it('should handle chunked processing', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      const totalSamples = sampleRate * 2;
      const chunkSize = 512;

      for (let i = 0; i < totalSamples; i += chunkSize) {
        const chunk = new Float32Array(Math.min(chunkSize, totalSamples - i));
        for (let j = 0; j < chunk.length; j++) {
          chunk[j] = Math.sin((2 * Math.PI * 440 * (i + j)) / sampleRate) * 0.5;
        }
        analyzer.process(chunk);
      }

      const stats = analyzer.stats();
      expect(stats.durationSeconds).toBeGreaterThan(1.8);
      expect(stats.durationSeconds).toBeLessThan(2.2);

      analyzer.dispose();
    });
  });

  describe('frame reading', () => {
    it('should read frames as SOA buffer', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5;
      }
      analyzer.process(samples);

      const availableFrames = analyzer.availableFrames();
      expect(availableFrames).toBeGreaterThan(0);

      const buffer = analyzer.readFrames(availableFrames);
      expect(buffer.nFrames).toBe(availableFrames);
      expect(buffer.timestamps.length).toBe(availableFrames);
      expect(buffer.chroma.length).toBe(availableFrames * 12);

      analyzer.dispose();
    });
  });
});
