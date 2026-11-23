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
  engineAbiVersion,
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
import { EXPECTED_ENGINE_ABI_VERSION } from '../src/realtime_engine';
import { expectRefusalOf } from './_helpers';
import { clearModuleRealm } from './_module_realm.js';

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
          liveDevices: false,
        },
        decode: {
          builtin: ['wav', 'mp3'],
          ffmpeg: [],
        },
        simd: expect.any(String),
        hardwareConcurrency: expect.any(Number),
      });
      expect(capabilities().abi.project).toBeGreaterThan(0);
      // The full bundle links every gated subsystem, so each flag is true.
      expect(capabilities().features).toMatchObject({
        arrangement: true,
        acousticSim: true,
        pitchEditor: true,
        vocalEdit: true,
        voiceChanger: true,
        playback: true,
      });
      expect(Object.keys(capabilities().features).sort()).toEqual([
        'acousticSim',
        'arrangement',
        'assist',
        'ffmpeg',
        'fx',
        'graph',
        'instrumentParamAutomation',
        'liveDevices',
        'mastering',
        'mixing',
        'mixingAssistant',
        'pitchEditor',
        'playback',
        'project',
        'vocalEdit',
        'voiceChanger',
      ]);
      expect(capabilities().hardwareConcurrency).toBeGreaterThanOrEqual(1);
    });

    it('aggregates processors and built-in presets in the capability catalog', () => {
      const catalog = capabilityCatalog();
      expect(catalog.version).toBe(version());
      expect(catalog.abi.project).toBeGreaterThan(0);
      expect(catalog.processors.length).toBeGreaterThan(0);
      expect(catalog.presets.mastering).toContain('pop');
      // Every group is present, an empty list rather than a missing key when its subsystem is out.
      expect(Object.keys(catalog.presets).sort()).toEqual([
        'mastering',
        'mixingScene',
        'playbackRoom',
        'synth',
        'voiceChanger',
      ]);
      expect(catalog.presets.playbackRoom).toContain('none');
      const compressor = catalog.processors.find(({ id }) => id === 'dynamics.compressor');
      expect(compressor).toMatchObject({ category: 'dynamics', realtimeInsertable: true });
    });

    it('should return engine ABI version', () => {
      expect(engineAbiVersion()).toBeGreaterThan(0);
    });

    it('should return the aggregate ABI version', () => {
      expect(abiVersion()).toBeGreaterThan(0);
    });

    it('agrees with the internal engine ABI mirror', () => {
      expect(engineAbiVersion()).toBe(EXPECTED_ENGINE_ABI_VERSION);
    });

    it('exports the track monitor mode type as named tokens only', () => {
      const modes: EngineTrackMonitorMode[] = ['off', 'pfl', 'afl'];
      // @ts-expect-error a C-ABI ordinal is not a monitor mode
      const ordinal: EngineTrackMonitorMode = 0;
      expect([...modes, ordinal]).toEqual(['off', 'pfl', 'afl', 0]);
    });

    it('keeps worklet telemetry error ordinals stable when appending channel limits', () => {
      const numericOrdinals = Object.entries(SonareEngineTelemetryError)
        .filter(([key]) => Number.isNaN(Number(key)))
        .map(([, value]) => value);
      expect(numericOrdinals).toEqual(Array.from({ length: 22 }, (_, ordinal) => ordinal));
      expect(SonareEngineTelemetryError.InvalidCommand).toBe(19);
      expect(SonareEngineTelemetryError.MaxChannelsExceeded).toBe(20);
      expect(SonareEngineTelemetryError.ParameterBaseOverflow).toBe(21);
    });

    it('reports the warp-stretch overflow counter beside the clip-page one', () => {
      // clipPageRequestOverflowCount already existed here; warpStretchOverflowCount
      // is the mirror of the same C-ABI shape and was the surface's only gap.
      // A fresh engine has dropped nothing, so this asserts the method EXISTS
      // and reads zero -- a missing embind registration is a TypeError.
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        expect(engine.warpStretchOverflowCount).toBe(0);
        expect(typeof engine.warpStretchOverflowCount).toBe('number');
        // Positive control: the sibling that has always been registered reads
        // the same way, so zero is the counter answering rather than a stub.
        expect(engine.clipPageRequestOverflowCount).toBe(0);
      } finally {
        engine.destroy();
      }
    });

    it('should allow retry after failed init', async () => {
      vi.resetModules();
      clearModuleRealm();
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
      expect(framesToSamples(4, 512)).toBe(2048);
      expect(samplesToFrames(2048, 512)).toBe(4);

      const powerDb = powerToDb({
        values: new Float32Array([1, 0.01]),
        ref: 1,
        amin: 1e-10,
        topDb: 80,
      });
      expect(powerDb[0]).toBeCloseTo(0, 5);
      expect(powerDb[1]).toBeCloseTo(-20, 4);
      expect(dbToPower({ values: powerDb, ref: 1 })[1]).toBeCloseTo(0.01, 5);

      const ampDb = amplitudeToDb({
        values: new Float32Array([1, 0.5]),
        ref: 1,
        amin: 1e-5,
        topDb: 80,
      });
      expect(ampDb[0]).toBeCloseTo(0, 5);
      expect(dbToAmplitude({ values: ampDb, ref: 1 })[1]).toBeCloseTo(0.5, 5);

      const emphasized = preemphasis(new Float32Array([1, 1, 1]), 22050, { coef: 0.5, zi: 0 });
      expect(Array.from(emphasized)).toEqual([1, 0.5, 0.5]);
      expect(deemphasis(emphasized, 22050, { coef: 0.5, zi: 0 })[2]).toBeCloseTo(1, 5);

      const framed = frameSignal(new Float32Array([1, 2, 3, 4]), 22050, {
        frameLength: 2,
        hopLength: 1,
      });
      expect(framed.cols).toBe(3);
      expect(framed.rows).toBe(2);
      expect(Array.from(framed.data)).toEqual([1, 2, 3, 2, 3, 4]);
      expect(
        Array.from(fixLength({ values: new Float32Array([1, 2]), targetSize: 4, padValue: -1 })),
      ).toEqual([1, 2, -1, -1]);
      expect(
        Array.from(fixFrames({ frames: new Int32Array([2, 4]), xMin: 0, xMax: 5, pad: true })),
      ).toEqual([0, 2, 4, 5]);
      // Matches librosa.util.peak_pick exactly (index 0 is a peak under its
      // first-frame rule: x[0] >= max/mean of the leading window).
      expect(
        Array.from(
          peakPick({
            values: new Float32Array([0, 1, 0, 2, 0]),
            preMax: 1,
            postMax: 1,
            preAvg: 1,
            postAvg: 1,
            delta: 0,
            wait: 0,
          }),
        ),
      ).toEqual([0, 1, 3]);

      const normalized = vectorNormalize({
        values: new Float32Array([3, 4]),
        norm: 'l2',
        threshold: 1e-12,
      });
      expect(normalized[0]).toBeCloseTo(0.6, 5);
      expect(normalized[1]).toBeCloseTo(0.8, 5);
    });

    it('exposes silence and rhythm utility functions', () => {
      const samples = new Float32Array([0, 0, 1, 1, 0, 0]);
      const trimmed = trimSilence(samples, 22050, { topDb: 20, frameLength: 2, hopLength: 1 });
      expect(trimmed.audio.length).toBeGreaterThan(0);
      expect(trimmed.endSample).toBeGreaterThan(trimmed.startSample);
      expect(
        splitSilence(samples, 22050, { topDb: 20, frameLength: 2, hopLength: 1 }),
      ).toBeInstanceOf(Int32Array);

      const pcenValues = pcen({
        values: { data: new Float32Array([1, 2, 3, 4]), rows: 2, cols: 2 },
      });
      expect(pcenValues.data).toBeInstanceOf(Float32Array);
      expect(pcenValues.data.length).toBe(4);

      const chromaValues = new Float32Array(12 * 2);
      chromaValues[0] = 1;
      chromaValues[12] = 1;
      const tonnetzValues = tonnetz({ chromagram: { data: chromaValues, rows: 12, cols: 2 } });
      expect(tonnetzValues.data).toBeInstanceOf(Float32Array);
      expect(tonnetzValues.rows).toBe(6);
      expect(tonnetzValues.cols).toBe(2);
      expect(tonnetzValues.data.length).toBe(12);

      const onset = new Float32Array([0, 1, 0, 1, 0, 1, 0, 1]);
      const temp = tempogram({
        onsetEnvelope: onset,
        sampleRate: 22050,
        hopLength: 512,
        winLength: 4,
      });
      expect(temp.data).toBeInstanceOf(Float32Array);
      expect(temp.rows).toBe(4);
      const cosine = tempogram({
        onsetEnvelope: onset,
        sampleRate: 22050,
        hopLength: 512,
        winLength: 4,
        mode: 'cosine',
      });
      expect(cosine.data).toBeInstanceOf(Float32Array);
      expect(cosine.data.length).toBe(4 * onset.length);
      expect(
        expectRefusalOf(
          () =>
            tempogram({
              onsetEnvelope: onset,
              sampleRate: 22050,
              hopLength: 512,
              winLength: 4,
              mode: 'invalid' as never,
            }),
          RangeError,
        ).message,
      ).toBe('tempogram: mode must be one of autocorrelation, cosine');
      expect(
        plp({
          onsetEnvelope: onset,
          sampleRate: 22050,
          hopLength: 512,
          bpmMin: 30,
          bpmMax: 300,
          winLength: 4,
        }),
      ).toBeInstanceOf(Float32Array);
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
      expect(Array.isArray(result.beats)).toBe(true);
      expect(result.chords).toBeDefined();
      expect(result.sections).toBeDefined();
      expect(result.timbre).toBeDefined();
      expect(result.dynamics).toBeDefined();
    });
  });
});
