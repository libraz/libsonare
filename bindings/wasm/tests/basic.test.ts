/**
 * Basic WASM module tests: what the module reports about itself once loaded,
 * and the top-level analysis facades.
 */

import { beforeAll, describe, expect, it, vi } from 'vitest';
import type { EngineTrackMonitorMode } from '../dist/index.js';
import {
  abiVersion,
  amplitudeToDb,
  analyze,
  capabilities,
  capabilityCatalog,
  dbToAmplitude,
  dbToPower,
  deemphasis,
  detectBeats,
  detectBpm,
  detectKey,
  EXPECTED_ENGINE_ABI_VERSION,
  engineAbiVersion,
  engineCapabilities,
  fixFrames,
  fixLength,
  frameSignal,
  framesToSamples,
  init,
  isInitialized,
  pcen,
  peakPick,
  plp,
  powerToDb,
  preemphasis,
  RealtimeEngine,
  samplesToFrames,
  splitSilence,
  tempogram,
  tonnetz,
  trimSilence,
  vectorNormalize,
  version,
} from '../dist/index.js';
import { SonareEngineTelemetryError } from '../dist/worklet.js';

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
      expect(v).toMatch(/^\d+\.\d+\.\d+$/);
    });

    it('reports the loaded build capabilities', () => {
      expect(capabilities()).toMatchObject({
        version: version(),
        abi: {
          project: expect.any(Number),
          engine: engineAbiVersion(),
        },
        platform: 'wasm32',
        features: {
          mastering: expect.any(Boolean),
          mixing: expect.any(Boolean),
          fx: expect.any(Boolean),
          ffmpeg: false,
        },
        decode: {
          builtin: ['wav', 'mp3'],
          ffmpeg: [],
        },
        simd: expect.any(String),
        hardwareConcurrency: expect.any(Number),
      });
      expect(capabilities().abi.project).toBeGreaterThan(0);
      expect(capabilities().hardwareConcurrency).toBeGreaterThanOrEqual(1);
    });

    it('aggregates processors and built-in presets in the capability catalog', () => {
      const catalog = capabilityCatalog();
      expect(catalog.version).toBe(version());
      expect(catalog.abi.project).toBeGreaterThan(0);
      expect(catalog.processors.length).toBeGreaterThan(0);
      expect(catalog.presets.mastering).toContain('pop');
      const compressor = catalog.processors.find(({ id }) => id === 'dynamics.compressor');
      expect(compressor).toMatchObject({ category: 'dynamics', realtimeInsertable: true });
    });

    it('should return engine ABI version', () => {
      expect(engineAbiVersion()).toBeGreaterThan(0);
    });

    it('should return the aggregate ABI version', () => {
      expect(abiVersion()).toBeGreaterThan(0);
    });

    it('reports realtime engine capabilities and ABI compatibility', () => {
      const capabilities = engineCapabilities();
      expect(capabilities.engineAbiVersion).toBe(EXPECTED_ENGINE_ABI_VERSION);
      expect(capabilities.abiCompatible).toBe(true);
      expect(capabilities.mode === 'sab' || capabilities.mode === 'postMessage').toBe(true);
    });

    it('exports the track monitor mode type with the C-ABI ordinals', () => {
      const modes: EngineTrackMonitorMode[] = ['off', 'pfl', 'afl', 0, 1, 2];
      expect(modes).toEqual(['off', 'pfl', 'afl', 0, 1, 2]);
    });

    it('keeps worklet telemetry error ordinals stable when appending channel limits', () => {
      const numericOrdinals = Object.entries(SonareEngineTelemetryError)
        .filter(([key]) => Number.isNaN(Number(key)))
        .map(([, value]) => value);
      expect(numericOrdinals).toEqual(Array.from({ length: 21 }, (_, ordinal) => ordinal));
      expect(SonareEngineTelemetryError.InvalidCommand).toBe(19);
      expect(SonareEngineTelemetryError.MaxChannelsExceeded).toBe(20);
    });

    it('reports the warp-stretch overflow counter beside the clip-page one', () => {
      // clipPageRequestOverflowCount already existed here; warpStretchOverflowCount
      // is the mirror of the same C-ABI shape and was the surface's only gap.
      // A fresh engine has dropped nothing, so this asserts the method EXISTS
      // and reads zero -- a missing embind registration is a TypeError.
      const engine = new RealtimeEngine(48000, 128);
      try {
        expect(engine.warpStretchOverflowCount()).toBe(0);
        expect(typeof engine.warpStretchOverflowCount()).toBe('number');
        // Positive control: the sibling that has always been registered reads
        // the same way, so zero is the counter answering rather than a stub.
        expect(engine.clipPageRequestOverflowCount()).toBe(0);
      } finally {
        engine.delete();
      }
    });

    it('should allow retry after failed init', async () => {
      vi.resetModules();
      const fresh = await import('../dist/index.js');

      await expect(
        fresh.init({
          locateFile: () => '/definitely-missing/sonare.wasm',
        }),
      ).rejects.toBeDefined();

      await expect(fresh.init()).resolves.toBeUndefined();
      expect(fresh.isInitialized()).toBe(true);
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

  describe('compatibility utilities', () => {
    it('exposes numeric and signal utility functions', () => {
      expect(framesToSamples(4, 512, 0)).toBe(2048);
      expect(samplesToFrames(2048, 512, 0)).toBe(4);

      const powerDb = powerToDb(new Float32Array([1, 0.01]), 1, 1e-10, 80);
      expect(powerDb[0]).toBeCloseTo(0, 5);
      expect(powerDb[1]).toBeCloseTo(-20, 4);
      expect(dbToPower(powerDb, 1)[1]).toBeCloseTo(0.01, 5);

      const ampDb = amplitudeToDb(new Float32Array([1, 0.5]), 1, 1e-5, 80);
      expect(ampDb[0]).toBeCloseTo(0, 5);
      expect(dbToAmplitude(ampDb, 1)[1]).toBeCloseTo(0.5, 5);

      const emphasized = preemphasis(new Float32Array([1, 1, 1]), 0.5, 0);
      expect(Array.from(emphasized)).toEqual([1, 0.5, 0.5]);
      expect(deemphasis(emphasized, 0.5, 0)[2]).toBeCloseTo(1, 5);

      const framed = frameSignal(new Float32Array([1, 2, 3, 4]), 2, 1);
      expect(framed.nFrames).toBe(3);
      expect(Array.from(framed.frames)).toEqual([1, 2, 2, 3, 3, 4]);
      expect(Array.from(fixLength(new Float32Array([1, 2]), 4, -1))).toEqual([1, 2, -1, -1]);
      expect(Array.from(fixFrames(new Int32Array([2, 4]), 0, 5, true))).toEqual([0, 2, 4, 5]);
      // Matches librosa.util.peak_pick exactly (index 0 is a peak under its
      // first-frame rule: x[0] >= max/mean of the leading window).
      expect(Array.from(peakPick(new Float32Array([0, 1, 0, 2, 0]), 1, 1, 1, 1, 0, 0))).toEqual([
        0, 1, 3,
      ]);

      const normalized = vectorNormalize(new Float32Array([3, 4]), 2, 1e-12);
      expect(normalized[0]).toBeCloseTo(0.6, 5);
      expect(normalized[1]).toBeCloseTo(0.8, 5);
    });

    it('exposes silence and rhythm utility functions', () => {
      const samples = new Float32Array([0, 0, 1, 1, 0, 0]);
      const trimmed = trimSilence(samples, 20, 2, 1);
      expect(trimmed.audio.length).toBeGreaterThan(0);
      expect(trimmed.endSample).toBeGreaterThan(trimmed.startSample);
      expect(splitSilence(samples, 20, 2, 1)).toBeInstanceOf(Int32Array);

      const pcenValues = pcen(new Float32Array([1, 2, 3, 4]), 2, 2);
      expect(pcenValues).toBeInstanceOf(Float32Array);
      expect(pcenValues.length).toBe(4);

      const chromaValues = new Float32Array(12 * 2);
      chromaValues[0] = 1;
      chromaValues[12] = 1;
      const tonnetzValues = tonnetz(chromaValues, 12, 2);
      expect(tonnetzValues).toBeInstanceOf(Float32Array);
      expect(tonnetzValues.length).toBe(12);

      const onset = new Float32Array([0, 1, 0, 1, 0, 1, 0, 1]);
      const temp = tempogram(onset, 22050, 512, 4);
      expect(temp.data).toBeInstanceOf(Float32Array);
      expect(temp.winLength).toBe(4);
      const cosine = tempogram(onset, 22050, 512, 4, 'cosine');
      expect(cosine.data).toBeInstanceOf(Float32Array);
      expect(cosine.data.length).toBe(4 * onset.length);
      expect(() => tempogram(onset, 22050, 512, 4, 'invalid' as never)).toThrow();
      expect(plp(onset, 22050, 512, 30, 300, 4)).toBeInstanceOf(Float32Array);
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
    it('should return complete analysis result', { timeout: 30000 }, () => {
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
      expect(result.beatTimes).toBeInstanceOf(Float32Array);
      expect(result.beats).toBeDefined();
      expect(result.chords).toBeDefined();
      expect(result.sections).toBeDefined();
      expect(result.timbre).toBeDefined();
      expect(result.dynamics).toBeDefined();
    });
  });
});
