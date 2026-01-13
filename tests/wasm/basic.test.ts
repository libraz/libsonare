/**
 * Basic WASM module tests
 */

import { describe, it, expect, beforeAll } from 'vitest';
import { init, isInitialized, version, detectBpm, detectKey, detectBeats, analyze } from '../../js/index';

describe('Sonare WASM Module', () => {
  beforeAll(async () => {
    await init();
  });

  describe('initialization', () => {
    it('should be initialized after init()', () => {
      expect(isInitialized()).toBe(true);
    });

    it('should return version string', () => {
      const v = version();
      expect(v).toBe('1.0.0');
    });
  });

  describe('detectBpm', () => {
    it('should detect BPM from sine wave', () => {
      // Generate 120 BPM click track (4 seconds)
      const sampleRate = 22050;
      const duration = 4;
      const bpm = 120;
      const samples = new Float32Array(sampleRate * duration);

      // Create clicks at beat positions
      const samplesPerBeat = (sampleRate * 60) / bpm;
      for (let beat = 0; beat < (duration * bpm) / 60; beat++) {
        const startSample = Math.floor(beat * samplesPerBeat);
        // Short click
        for (let i = 0; i < 100 && startSample + i < samples.length; i++) {
          samples[startSample + i] = Math.sin((i * Math.PI) / 100);
        }
      }

      const detectedBpm = detectBpm(samples, sampleRate);
      // Allow ±10% tolerance
      expect(detectedBpm).toBeGreaterThan(bpm * 0.9);
      expect(detectedBpm).toBeLessThan(bpm * 1.1);
    });
  });

  describe('detectKey', () => {
    it('should detect key from chromatic content', () => {
      const sampleRate = 22050;
      const duration = 2;
      const samples = new Float32Array(sampleRate * duration);

      // Generate A4 (440 Hz) - should detect A major or A minor
      const freq = 440;
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * freq * i) / sampleRate);
      }

      const key = detectKey(samples, sampleRate);
      expect(key.root).toBeDefined();
      expect(key.mode).toBeDefined();
      expect(key.confidence).toBeGreaterThanOrEqual(0);
      expect(key.confidence).toBeLessThanOrEqual(1);
      expect(key.name).toBeDefined();
    });
  });

  describe('detectBeats', () => {
    it('should return beat times array', () => {
      const sampleRate = 22050;
      const duration = 4;
      const samples = new Float32Array(sampleRate * duration);

      // Simple impulse pattern
      for (let i = 0; i < samples.length; i += sampleRate / 2) {
        samples[i] = 1.0;
      }

      const beats = detectBeats(samples, sampleRate);
      expect(beats).toBeInstanceOf(Float32Array);
    });
  });

  describe('analyze', () => {
    it('should return complete analysis result', () => {
      const sampleRate = 22050;
      const duration = 4;
      const samples = new Float32Array(sampleRate * duration);

      // Generate test signal
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.5;
      }

      const result = analyze(samples, sampleRate);

      expect(result.bpm).toBeGreaterThan(0);
      expect(result.key).toBeDefined();
      expect(result.timeSignature).toBeDefined();
      expect(result.beats).toBeDefined();
      expect(result.chords).toBeDefined();
      expect(result.sections).toBeDefined();
      expect(result.timbre).toBeDefined();
      expect(result.dynamics).toBeDefined();
    });
  });
});
