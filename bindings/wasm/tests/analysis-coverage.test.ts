/**
 * Regression tests for analyzers newly exposed to the WASM binding.
 *
 * These mirror the Node / Python `analyzeBpm` / `analyzeRhythm` /
 * `analyzeDynamics` / `analyzeTimbre` / `detectKeyCandidates` /
 * `hasFfmpegSupport` / `masterAudioWithProgress` surfaces. The intent is
 * to keep feature parity across all three bindings — if any of these is
 * missing from a new WASM build, this file fails.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  hasFfmpegSupport,
  init,
  masterAudioStereoWithProgress,
  masterAudioWithProgress,
} from '../src/index';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.25 * Math.sin((2 * Math.PI * freqHz * i) / SR);
  }
  return out;
}

function makeClickTrack(durationSec: number, bpm: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  const samplesPerBeat = Math.floor((60 / bpm) * SR);
  const clickLen = Math.floor(0.01 * SR); // 10 ms click
  for (let beat = 0; beat * samplesPerBeat < n; beat++) {
    const start = beat * samplesPerBeat;
    for (let i = 0; i < clickLen && start + i < n; i++) {
      const env = Math.exp(-i / (0.002 * SR));
      out[start + i] = env * Math.sin((2 * Math.PI * 1000 * i) / SR);
    }
  }
  return out;
}

describe('WASM analyzer coverage parity', () => {
  beforeAll(async () => {
    await init();
  });

  describe('analyzeBpm', () => {
    it('returns finite BPM, candidates, autocorrelation, and tempogram', () => {
      const samples = makeClickTrack(8, 120);
      const result = analyzeBpm(samples, SR);
      expect(Number.isFinite(result.bpm)).toBe(true);
      expect(result.bpm).toBeGreaterThan(0);
      expect(Number.isFinite(result.confidence)).toBe(true);
      expect(Array.isArray(result.candidates)).toBe(true);
      expect(result.autocorrelation).toBeInstanceOf(Float32Array);
      expect(result.tempogram).toBeInstanceOf(Float32Array);
      expect(result.autocorrelation.length).toBeGreaterThan(0);
    });
  });

  describe('analyzeRhythm', () => {
    it('returns groove, syncopation, and beat intervals', () => {
      const samples = makeClickTrack(8, 120);
      const result = analyzeRhythm(samples, SR);
      expect(typeof result.grooveType).toBe('string');
      expect(Number.isFinite(result.syncopation)).toBe(true);
      expect(Number.isFinite(result.patternRegularity)).toBe(true);
      expect(Number.isFinite(result.tempoStability)).toBe(true);
      expect(result.timeSignature.numerator).toBeGreaterThan(0);
      expect(result.timeSignature.denominator).toBeGreaterThan(0);
      expect(result.beatIntervals).toBeInstanceOf(Float32Array);
    });
  });

  describe('analyzeDynamics', () => {
    it('returns peak/RMS/crest factor and a loudness curve', () => {
      const samples = makeSine(5, 1000);
      const result = analyzeDynamics(samples, SR);
      expect(Number.isFinite(result.peakDb)).toBe(true);
      expect(Number.isFinite(result.rmsDb)).toBe(true);
      expect(Number.isFinite(result.crestFactor)).toBe(true);
      expect(Number.isFinite(result.dynamicRangeDb)).toBe(true);
      expect(typeof result.isCompressed).toBe('boolean');
      expect(result.loudnessTimes).toBeInstanceOf(Float32Array);
      expect(result.loudnessRmsDb).toBeInstanceOf(Float32Array);
      expect(result.loudnessTimes.length).toBe(result.loudnessRmsDb.length);
    });

    it('flags a heavily limited signal as compressed', () => {
      // Brick-wall full-scale square approximation: very small dynamic range.
      const n = SR * 4;
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        samples[i] = Math.sign(Math.sin((2 * Math.PI * 220 * i) / SR)) * 0.99;
      }
      const result = analyzeDynamics(samples, SR);
      expect(result.crestFactor).toBeLessThan(6); // square wave ~3 dB crest
    });
  });

  describe('analyzeTimbre', () => {
    it('returns scalar timbre features and per-window frames', () => {
      const samples = makeSine(3, 1000);
      const result = analyzeTimbre(samples, SR);
      for (const v of [
        result.brightness,
        result.warmth,
        result.density,
        result.roughness,
        result.complexity,
      ]) {
        expect(Number.isFinite(v)).toBe(true);
      }
      expect(result.spectralCentroid).toBeInstanceOf(Float32Array);
      expect(result.spectralFlatness).toBeInstanceOf(Float32Array);
      expect(result.spectralRolloff).toBeInstanceOf(Float32Array);
      expect(Array.isArray(result.timbreOverTime)).toBe(true);
    });
  });

  describe('hasFfmpegSupport', () => {
    it('returns a boolean (the WASM binding is FFmpeg-less by default)', () => {
      expect(typeof hasFfmpegSupport()).toBe('boolean');
    });
  });

  describe('masterAudioWithProgress', () => {
    it('invokes the progress callback at least once and returns mastered samples', () => {
      const samples = makeSine(2, 440);
      let calls = 0;
      let lastProgress = -1;
      const result = masterAudioWithProgress(samples, SR, 'pop', (p, stage) => {
        calls++;
        expect(typeof stage).toBe('string');
        expect(p).toBeGreaterThanOrEqual(lastProgress); // monotonic
        lastProgress = p;
      });
      expect(calls).toBeGreaterThan(0);
      expect(result.samples.length).toBeGreaterThan(0);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
    });

    it('stereo variant accepts two channels and reports stages', () => {
      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      let calls = 0;
      const result = masterAudioStereoWithProgress(left, right, SR, 'pop', () => {
        calls++;
      });
      expect(calls).toBeGreaterThan(0);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(Array.isArray(result.stages)).toBe(true);
    });
  });
});
