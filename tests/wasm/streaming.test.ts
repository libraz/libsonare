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

  describe('bar chord progression', () => {
    it('should detect bar chords after BPM stabilizes', () => {
      const sampleRate = 22050;
      const duration = 15; // Need enough time for BPM to stabilize
      const samples = new Float32Array(sampleRate * duration);

      // Generate rhythmic signal at 120 BPM (2 beats per second, 0.5s per beat)
      // With chord progression: C -> G repeated
      const bpm = 120;
      const beatDuration = 60 / bpm;
      const barDuration = beatDuration * 4; // 4/4 time signature

      const chords = [
        { freqs: [261.63, 329.63, 392.0] }, // C major
        { freqs: [392.0, 493.88, 587.33] }, // G major
      ];

      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        const barIndex = Math.floor(t / barDuration);
        const chord = chords[barIndex % 2];

        // Add chord tones
        for (const freq of chord.freqs) {
          samples[i] += Math.sin(2 * Math.PI * freq * t) / chord.freqs.length;
        }

        // Add rhythmic impulse at beat positions
        const beatPhase = (t % beatDuration) / beatDuration;
        if (beatPhase < 0.05) {
          samples[i] += 0.5 * (1 - beatPhase / 0.05);
        }

        samples[i] *= 0.3;
      }

      const analyzer = new StreamAnalyzer({ sampleRate });

      // Process in chunks
      const chunkSize = 4096;
      for (let i = 0; i < samples.length; i += chunkSize) {
        const chunk = samples.slice(i, Math.min(i + chunkSize, samples.length));
        analyzer.process(chunk);
      }

      const stats = analyzer.stats();

      // After 15 seconds, BPM should be somewhat stable
      // Bar chord progression should start detecting
      if (stats.estimate.bpmConfidence >= 0.3) {
        expect(stats.estimate.barDuration).toBeGreaterThan(0);
        expect(stats.estimate.currentBar).toBeGreaterThanOrEqual(0);

        // If bar chords were detected, verify their structure
        if (stats.estimate.barChordProgression.length > 0) {
          for (const chord of stats.estimate.barChordProgression) {
            expect(chord.barIndex).toBeGreaterThanOrEqual(0);
            expect(chord.root).toBeGreaterThanOrEqual(0);
            expect(chord.root).toBeLessThanOrEqual(11);
            expect(chord.quality).toBeGreaterThanOrEqual(0);
            expect(chord.startTime).toBeGreaterThanOrEqual(0);
            expect(chord.confidence).toBeGreaterThanOrEqual(0);
          }
        }
      }

      analyzer.dispose();
    });

    it('should have correct bar duration based on BPM', () => {
      const sampleRate = 22050;
      const duration = 20;
      const samples = new Float32Array(sampleRate * duration);

      // Generate strong rhythmic signal at 120 BPM
      const bpm = 120;
      const beatDuration = 60 / bpm; // 0.5 seconds

      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;

        // Sine wave with beat envelope
        samples[i] = Math.sin(2 * Math.PI * 440 * t);

        // Strong beat impulse
        const beatPhase = (t % beatDuration) / beatDuration;
        if (beatPhase < 0.03) {
          samples[i] += 2.0 * (1 - beatPhase / 0.03);
        }

        samples[i] *= 0.3;
      }

      const analyzer = new StreamAnalyzer({ sampleRate });
      analyzer.process(samples);

      const stats = analyzer.stats();

      // If BPM is stable and close to 120, bar duration should be ~2 seconds
      if (
        stats.estimate.bpmConfidence >= 0.3 &&
        stats.estimate.bpm > 100 &&
        stats.estimate.bpm < 140
      ) {
        // bar_duration = 4 * 60 / bpm
        // For 120 BPM: 4 * 60 / 120 = 2.0 seconds
        const expectedBarDuration = (4 * 60) / stats.estimate.bpm;
        expect(stats.estimate.barDuration).toBeCloseTo(expectedBarDuration, 1);
      }

      analyzer.dispose();
    });

    it('should not have bar chords before BPM stabilizes', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Very short audio - not enough for BPM detection
      const samples = new Float32Array(sampleRate * 0.5);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5;
      }

      analyzer.process(samples);
      const stats = analyzer.stats();

      // BPM should not be stable yet
      expect(stats.estimate.barChordProgression.length).toBe(0);
      expect(stats.estimate.currentBar).toBe(-1);
      expect(stats.estimate.barDuration).toBe(0);

      analyzer.dispose();
    });

    it('should reset bar chord progression on reset', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Process enough audio to potentially get bar chords
      const samples = new Float32Array(sampleRate * 10);
      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        // Beat at 120 BPM
        const beatPhase = (t % 0.5) / 0.5;
        samples[i] = Math.sin(2 * Math.PI * 440 * t) * 0.3;
        if (beatPhase < 0.05) {
          samples[i] += 0.5;
        }
      }

      analyzer.process(samples);

      // Reset
      analyzer.reset();

      const stats = analyzer.stats();

      // All bar tracking should be reset
      expect(stats.estimate.barChordProgression.length).toBe(0);
      expect(stats.estimate.currentBar).toBe(-1);
      expect(stats.estimate.barDuration).toBe(0);

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

    it('should include per-frame chord data', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Generate C major chord
      const samples = new Float32Array(sampleRate);
      const freqs = [261.63, 329.63, 392.0]; // C-E-G
      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        for (const freq of freqs) {
          samples[i] += Math.sin(2 * Math.PI * freq * t) / freqs.length;
        }
        samples[i] *= 0.3;
      }

      analyzer.process(samples);

      const buffer = analyzer.readFrames(analyzer.availableFrames());

      // Verify chord arrays exist and have correct length
      expect(buffer.chordRoot).toBeDefined();
      expect(buffer.chordQuality).toBeDefined();
      expect(buffer.chordConfidence).toBeDefined();
      expect(buffer.chordRoot.length).toBe(buffer.nFrames);
      expect(buffer.chordQuality.length).toBe(buffer.nFrames);
      expect(buffer.chordConfidence.length).toBe(buffer.nFrames);

      // Verify chord data is valid
      for (let i = 0; i < buffer.nFrames; i++) {
        expect(buffer.chordRoot[i]).toBeGreaterThanOrEqual(0);
        expect(buffer.chordRoot[i]).toBeLessThanOrEqual(11);
        expect(buffer.chordConfidence[i]).toBeGreaterThanOrEqual(0);
      }

      analyzer.dispose();
    });

    it('should detect chord changes in real-time', () => {
      const sampleRate = 22050;
      const analyzer = new StreamAnalyzer({ sampleRate });

      // Generate C major for 1 second, then G major for 1 second
      const samples = new Float32Array(sampleRate * 2);
      const chords = [
        { freqs: [261.63, 329.63, 392.0], start: 0, end: 1 }, // C major
        { freqs: [392.0, 493.88, 587.33], start: 1, end: 2 }, // G major
      ];

      for (let i = 0; i < samples.length; i++) {
        const t = i / sampleRate;
        for (const chord of chords) {
          if (t >= chord.start && t < chord.end) {
            for (const freq of chord.freqs) {
              samples[i] += Math.sin(2 * Math.PI * freq * t) / chord.freqs.length;
            }
            break;
          }
        }
        samples[i] *= 0.3;
      }

      analyzer.process(samples);

      const buffer = analyzer.readFrames(analyzer.availableFrames());

      // Find unique chords in the buffer
      const uniqueChords = new Set<string>();
      for (let i = 0; i < buffer.nFrames; i++) {
        uniqueChords.add(`${buffer.chordRoot[i]}-${buffer.chordQuality[i]}`);
      }

      // Should have detected at least 2 different chords (C and G)
      expect(uniqueChords.size).toBeGreaterThanOrEqual(2);

      analyzer.dispose();
    });
  });
});
