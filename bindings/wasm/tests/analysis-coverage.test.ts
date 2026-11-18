/**
 * Regression tests for analyzers newly exposed to the WASM binding.
 *
 * These mirror the Node / Python `analyzeBpm` / `analyzeRhythm` /
 * `analyzeDynamics` / `analyzeTimbre` / `detectKeyCandidates` /
 * `capabilities().features.ffmpeg` / `masterAudio` progress surfaces. The intent is
 * to keep feature parity across all three bindings — if any of these is
 * missing from a new WASM build, this file fails.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  capabilities,
  chordFunctionalAnalysis,
  ErrorCode,
  init,
  masterAudio,
  masterAudioStereo,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringNormalizeLoudness,
  PitchClass,
} from '../src/index';
import { sine as genSine } from './_helpers';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  return genSine(freqHz, durationSec, { amp: 0.25, sampleRate: SR });
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
      expect(result.loudnessTimesSec).toBeInstanceOf(Float32Array);
      expect(result.loudnessRmsDb).toBeInstanceOf(Float32Array);
      expect(result.loudnessTimesSec.length).toBe(result.loudnessRmsDb.length);
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

  describe('chordFunctionalAnalysis', () => {
    it('returns a Roman-numeral label array relative to the given key', () => {
      // A simple tonal signal; the exact labels depend on detection, but the
      // surface must return the timed entries (one per detected chord, with roman and function).
      const samples = makeSine(4, 261.63); // C4
      const labels = chordFunctionalAnalysis({
        samples,
        sampleRate: SR,
        keyRoot: PitchClass.C,
        keyMode: 'major',
      });
      expect(Array.isArray(labels)).toBe(true);
      for (const entry of labels) {
        expect(entry.romanNumeral.length).toBeGreaterThan(0);
        expect(entry.function.length).toBeGreaterThan(0);
      }
    });
  });

  describe('capabilities().features.ffmpeg', () => {
    it('is a boolean (the WASM binding is FFmpeg-less by default)', () => {
      expect(typeof capabilities().features.ffmpeg).toBe('boolean');
    });
  });

  describe('masterAudio', () => {
    it('accepts the canonical loudness-mastering request object', () => {
      const samples = makeSine(2, 440);
      const positional = masteringNormalizeLoudness(samples, SR, { targetLufs: -18 });
      const request = masteringNormalizeLoudness({ samples, sampleRate: SR, targetLufs: -18 });

      expect(request).toEqual(positional);
    });

    it('accepts canonical named-processor request objects', () => {
      const samples = makeSine(2, 440);
      const mono = masteringApplyProcessor({
        processor: 'dynamics.compressor',
        samples,
        sampleRate: SR,
        params: { ratio: 2 },
      });
      expect(
        masteringApplyProcessor(samples, SR, {
          processor: 'dynamics.compressor',
          params: { ratio: 2 },
        }),
      ).toEqual(mono);

      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      expect(
        masteringApplyProcessorStereo({
          processor: 'stereo.imager',
          left,
          right,
          sampleRate: SR,
          params: { width: 1 },
        }).left,
      ).toHaveLength(left.length);
    });

    it('refuses a master call giving neither a preset nor overrides, in every call shape', () => {
      const samples = makeSine(2, 440);
      const message = /^masterAudio: give a preset or overrides$/;
      expect(() => masterAudio({ samples, sampleRate: SR })).toThrow(TypeError);
      expect(() => masterAudio({ samples, sampleRate: SR })).toThrow(message);
      expect(() => masterAudio(samples, SR)).toThrow(message);
      expect(() =>
        masterAudio({ samples, sampleRate: SR, preset: undefined, overrides: undefined }),
      ).toThrow(message);
      expect(() => masterAudio(samples, SR, { onProgress: () => {} })).toThrow(message);
      using audio = Audio.fromBuffer(samples, SR);
      expect(() => audio.masterAudio({})).toThrow(message);
      expect(() => masterAudioStereo({ left: samples, right: samples, sampleRate: SR })).toThrow(
        /^masterAudioStereo: give a preset or overrides$/,
      );
    });

    it('runs an explicit empty overrides as an empty chain that returns the input', () => {
      const samples = makeSine(2, 440);
      expect(masterAudio({ samples, sampleRate: SR, overrides: {} }).samples).toEqual(samples);
      const stereo = masterAudioStereo({
        left: samples,
        right: samples,
        sampleRate: SR,
        overrides: {},
      });
      expect(stereo.left).toEqual(samples);
      expect(stereo.right).toEqual(samples);
    });

    it('runs a chain without a preset from the request and the positional form', () => {
      const samples = makeSine(2, 440);
      expect(masterAudio(samples, SR, { overrides: {} })).toEqual(
        masterAudio({ samples, sampleRate: SR, overrides: {} }),
      );

      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      expect(masterAudioStereo({ left, right, sampleRate: SR, overrides: {} }).left).toHaveLength(
        left.length,
      );
    });

    it('accepts the canonical mono request object', () => {
      const samples = makeSine(2, 440);
      const positional = masterAudio(samples, SR, { preset: 'pop' });
      const request = masterAudio({ samples, sampleRate: SR, preset: 'pop' });

      expect(request).toEqual(positional);
    });

    it('accepts the canonical stereo request object', () => {
      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      const request = masterAudioStereo({ left, right, sampleRate: SR, preset: 'pop' });

      expect(request.left).toHaveLength(left.length);
      expect(request.right).toHaveLength(right.length);
    });

    it('invokes the progress callback at least once and returns mastered samples', () => {
      const samples = makeSine(2, 440);
      let calls = 0;
      let lastProgress = -1;
      const result = masterAudio({
        samples,
        sampleRate: SR,
        preset: 'pop',
        overrides: { eq: { tilt: { tiltDb: 1 } } },
        onProgress: (p, stage) => {
          calls++;
          expect(typeof stage).toBe('string');
          expect(p).toBeGreaterThanOrEqual(lastProgress); // monotonic
          lastProgress = p;
        },
      });
      expect(calls).toBeGreaterThan(0);
      expect(result.samples.length).toBeGreaterThan(0);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
    });

    // Mono and stereo, with and without a preset, forward `cancel` through their own
    // native call, so each variant is driven: a forwarding regression on any one of
    // them would otherwise run to completion instead of aborting.
    it.each([
      ['mono without a preset', { overrides: {} }],
      ['mono with a preset', { preset: 'pop' }],
      ['mono with progress', { preset: 'pop', onProgress: () => {} }],
      ['mono chain with progress', { overrides: {}, onProgress: () => {} }],
    ] as const)('masterAudio cancels through the request callback: %s', (_name, extra) => {
      const samples = makeSine(2, 440);
      expect(() => masterAudio({ samples, sampleRate: SR, ...extra, cancel: () => true })).toThrow(
        expect.objectContaining({ code: ErrorCode.Cancelled }),
      );
    });

    it.each([
      ['without a preset', { overrides: {} }],
      ['with a preset', { preset: 'pop' }],
      ['with progress', { preset: 'pop', onProgress: () => {} }],
      ['chain with progress', { overrides: {}, onProgress: () => {} }],
    ] as const)('masterAudioStereo cancels through the request callback: %s', (_name, extra) => {
      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      expect(() =>
        masterAudioStereo({ left, right, sampleRate: SR, ...extra, cancel: () => true }),
      ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled }));
    });

    it('stereo variant accepts two channels and reports stages', () => {
      const left = makeSine(2, 440);
      const right = makeSine(2, 660);
      let calls = 0;
      const result = masterAudioStereo({
        left,
        right,
        sampleRate: SR,
        preset: 'pop',
        onProgress: () => {
          calls++;
        },
      });
      expect(calls).toBeGreaterThan(0);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(Array.isArray(result.stages)).toBe(true);
    });
  });
});
