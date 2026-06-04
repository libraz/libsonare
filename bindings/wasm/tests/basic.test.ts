/**
 * Basic WASM module tests
 */

import { beforeAll, describe, expect, it, vi } from 'vitest';
import {
  amplitudeToDb,
  analyze,
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
  mastering,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
  pcen,
  peakPick,
  plp,
  powerToDb,
  preemphasis,
  RealtimeEngine,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  samplesToFrames,
  splitSilence,
  tempogram,
  tonnetz,
  trimSilence,
  vectorNormalize,
  version,
} from '../dist/index.js';

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

    it('should return engine ABI version', () => {
      expect(engineAbiVersion()).toBeGreaterThan(0);
    });

    it('reports realtime engine capabilities and ABI compatibility', () => {
      const capabilities = engineCapabilities();
      expect(capabilities.engineAbiVersion).toBe(EXPECTED_ENGINE_ABI_VERSION);
      expect(capabilities.abiCompatible).toBe(true);
      expect(capabilities.mode === 'sab' || capabilities.mode === 'postMessage').toBe(true);
    });

    it('loads the dedicated sonare-rt target and processes an audio block through the C ABI', async () => {
      const { default: createSonareRt } = (await import('../dist/sonare-rt.js')) as {
        default: (options?: { locateFile?: (path: string) => string }) => Promise<{
          _malloc: (size: number) => number;
          _free: (ptr: number) => void;
          _sonare_rt_engine_abi_version: () => number;
          _sonare_rt_engine_create: () => number;
          _sonare_rt_engine_prepare: (
            engine: number,
            sampleRate: number,
            maxBlockSize: number,
            commandCapacity: number,
            telemetryCapacity: number,
          ) => number;
          _sonare_rt_engine_play: (engine: number, renderFrame: bigint) => number;
          _sonare_rt_engine_process: (
            engine: number,
            channelsPtr: number,
            numChannels: number,
            numFrames: number,
          ) => void;
          _sonare_rt_engine_seek_sample: (
            engine: number,
            timelineSample: bigint,
            renderFrame: bigint,
          ) => number;
          _sonare_rt_engine_drain_telemetry: (
            engine: number,
            typesErrorsValuesPtr: number,
            frameValuesPtr: number,
            maxRecords: number,
          ) => number;
          _sonare_rt_engine_destroy: (engine: number) => void;
        }>;
      };
      const memory = new WebAssembly.Memory({ initial: 1024, maximum: 1024, shared: true });
      const rt = await createSonareRt({
        wasmMemory: memory,
        locateFile: (path) => new URL(`../dist/${path}`, import.meta.url).pathname,
      } as Parameters<typeof createSonareRt>[0]);
      expect(rt._sonare_rt_engine_abi_version()).toBe(EXPECTED_ENGINE_ABI_VERSION);
      const engine = rt._sonare_rt_engine_create();
      expect(engine).toBeGreaterThan(0);
      expect(rt._sonare_rt_engine_prepare(engine, 48000, 128, 64, 64)).toBe(1);
      expect(rt._sonare_rt_engine_play(engine, -1n)).toBe(1);

      const frames = 128;
      const leftPtr = rt._malloc(frames * Float32Array.BYTES_PER_ELEMENT);
      const rightPtr = rt._malloc(frames * Float32Array.BYTES_PER_ELEMENT);
      const channelPtr = rt._malloc(2 * Uint32Array.BYTES_PER_ELEMENT);
      const telemetryIntsPtr = rt._malloc(4 * Int32Array.BYTES_PER_ELEMENT);
      const telemetryFramesPtr = rt._malloc(3 * Float64Array.BYTES_PER_ELEMENT);
      const samples = new Float32Array(memory.buffer);
      const pointers = new Uint32Array(memory.buffer);
      const telemetryInts = new Int32Array(memory.buffer);
      const telemetryFrames = new Float64Array(memory.buffer);
      for (let i = 0; i < frames; i++) {
        samples[(leftPtr >> 2) + i] = i / frames;
        samples[(rightPtr >> 2) + i] = -i / frames;
      }
      pointers[channelPtr >> 2] = leftPtr;
      pointers[(channelPtr >> 2) + 1] = rightPtr;

      rt._sonare_rt_engine_process(engine, channelPtr, 2, frames);

      expect(samples[leftPtr >> 2]).toBeCloseTo(0, 6);
      expect(samples[(leftPtr >> 2) + 127]).toBeCloseTo(127 / 128, 6);
      expect(samples[rightPtr >> 2]).toBeCloseTo(0, 6);
      expect(samples[(rightPtr >> 2) + 127]).toBeCloseTo(-127 / 128, 6);
      expect(
        rt._sonare_rt_engine_drain_telemetry(engine, telemetryIntsPtr, telemetryFramesPtr, 1),
      ).toBe(1);
      expect(telemetryInts[telemetryIntsPtr >> 2]).toBe(0);
      expect(telemetryFrames[telemetryFramesPtr >> 3]).toBe(0);
      expect(telemetryFrames[(telemetryFramesPtr >> 3) + 1]).toBe(128);

      expect(rt._sonare_rt_engine_seek_sample(engine, 48000n, -1n)).toBe(1);
      rt._sonare_rt_engine_process(engine, channelPtr, 2, frames);
      expect(
        rt._sonare_rt_engine_drain_telemetry(engine, telemetryIntsPtr, telemetryFramesPtr, 1),
      ).toBe(1);
      expect(telemetryFrames[(telemetryFramesPtr >> 3) + 1]).toBe(48000 + 128);

      rt._free(telemetryFramesPtr);
      rt._free(telemetryIntsPtr);
      rt._free(channelPtr);
      rt._free(rightPtr);
      rt._free(leftPtr);
      rt._sonare_rt_engine_destroy(engine);
    });

    it('processes realtime engine clips, capture, and telemetry', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(60);
      engine.setTimeSignature(3, 4);
      engine.setMarkers([
        { id: 11, ppq: 1, name: 'intro' },
        { id: 12, ppq: 2, name: 'out' },
      ]);
      expect(engine.markerCount()).toBe(2);
      expect(engine.markerByIndex(0).name).toBe('intro');
      expect(engine.marker(12).ppq).toBe(2);
      engine.setLoopFromMarkers(11, 12);
      engine.setMetronome({ enabled: true, beatGain: 0.25, accentGain: 0.75, clickSamples: 16 });
      expect(engine.metronome().enabled).toBe(true);
      expect(engine.countInEndSample(0, 2)).toBe(288000);
      engine.setMetronome({ enabled: false });
      engine.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0, // canonical AutomationCurve::Linear
      });
      expect(engine.parameterCount()).toBe(1);
      expect(engine.parameterInfo(7).name).toBe('gain');
      expect(engine.parameterInfoByIndex(0).unit).toBe('dB');
      engine.setAutomationLane(7, [
        { ppq: 0, value: 0 },
        { ppq: 1, value: 6.0205999, curveToNext: 0 }, // Linear
      ]);
      expect(engine.automationLaneCount()).toBe(1);
      engine.setGraph({
        nodes: [
          { id: 'in', numPorts: 2 },
          { id: 'gain', type: 1, gainDb: 0, numPorts: 2 },
          { id: 'out', numPorts: 2 },
        ],
        connections: [
          { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
          { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
          { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
          { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
        ],
        inputNode: 'in',
        outputNode: 'out',
        numChannels: 2,
        parameterBindings: [{ paramId: 7, nodeId: 'gain' }],
      });
      expect(engine.graphNodeCount()).toBe(3);
      expect(engine.graphConnectionCount()).toBe(4);
      engine.setClips([
        {
          id: 101,
          channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.125)],
          startPpq: 1,
          lengthSamples: 128,
        },
      ]);
      expect(engine.clipCount()).toBe(1);
      engine.setCaptureBuffer(2, 128);
      engine.setCapturePunch(48000, 48128);
      engine.seekMarker(11);
      engine.play();

      const processed = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(processed[0][0]).toBeCloseTo(0.75, 4);
      expect(processed[1][0]).toBeCloseTo(-0.75, 4);

      engine.armCapture();
      engine.seekMarker(11);
      const capturedBlock = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(capturedBlock[0][0]).toBeCloseTo(0.75, 4);
      const captureStatus = engine.captureStatus();
      expect(captureStatus.capturedFrames).toBe(128);
      expect(captureStatus.overflowCount).toBe(0);
      expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.75, 4);
      engine.resetCapture();
      expect(engine.captureStatus().capturedFrames).toBe(0);

      const telemetry = engine.drainTelemetry();
      expect(telemetry.length).toBeGreaterThan(0);
      expect(telemetry.at(-1)?.timelineSample).toBe(48000 + 128);
      const meters = engine.drainMeterTelemetry();
      expect(meters.length).toBeGreaterThan(0);
      expect(meters.at(-1)).toMatchObject({ targetId: 0 });
      expect(meters.at(-1)?.peakDbL).toBeGreaterThan(-20);
      const bounced = engine.bounceOffline({
        totalFrames: 256,
        blockSize: 128,
        numChannels: 2,
        sourceSampleRate: 48000,
        targetSampleRate: 24000,
      });
      expect(bounced.frames).toBe(128);
      expect(bounced.numChannels).toBe(2);
      expect(bounced.sampleRate).toBe(24000);
      expect(bounced.interleaved.length).toBe(256);
      expect(Number.isFinite(bounced.integratedLufs) || !Number.isNaN(bounced.integratedLufs)).toBe(
        true,
      );
      engine.setClips([
        {
          id: 202,
          channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.25)],
          startPpq: 0,
          lengthSamples: 128,
        },
      ]);
      engine.seekSample(0);
      const frozen = engine.freezeOffline({
        totalFrames: 128,
        blockSize: 128,
        numChannels: 2,
        clipId: 77,
      });
      expect(frozen.clipId).toBe(77);
      expect(frozen.frames).toBe(128);
      expect(frozen.numChannels).toBe(2);
      engine.seekSample(0);
      const frozenRendered = engine.renderOffline([new Float32Array(128), new Float32Array(128)]);
      expect(frozenRendered[0][0]).toBeCloseTo(0.125, 4);
      expect(frozenRendered[1][0]).toBeCloseTo(-0.25, 4);
      engine.destroy();
    });

    it('processWithMonitor returns output and monitor buses', () => {
      const engine = new RealtimeEngine(48000, 16);
      const result = engine.processWithMonitor([
        new Float32Array(16).fill(0.25),
        new Float32Array(16).fill(-0.25),
      ]);
      expect(result.output).toHaveLength(2);
      expect(result.monitor).toHaveLength(2);
      expect(result.output[0][0]).toBeCloseTo(0.25);
      expect(result.output[1][0]).toBeCloseTo(-0.25);
      expect(result.monitor[0][0]).toBeCloseTo(0);
      expect(result.monitor[1][0]).toBeCloseTo(0);
      engine.destroy();
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
      expect(Array.from(peakPick(new Float32Array([0, 1, 0, 2, 0]), 1, 1, 1, 1, 0, 0))).toEqual([
        1, 3,
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

  describe('mastering', () => {
    it('should return processed samples and loudness metadata', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }

      const result = mastering(samples, sampleRate, -18.0, -1.0, 4);
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(Number.isFinite(result.appliedGainDb)).toBe(true);
      expect(result.outputLufs).toBeCloseTo(-18.0, 1);
    });

    it('should run a configurable mastering chain in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        const tone = Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        const overtone = 0.4 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
        samples[i] = 0.18 * (tone + overtone);
      }

      const result = masteringChain(samples, sampleRate, {
        eq: { tiltDb: 1.5, pivotHz: 1200 },
        dynamics: {
          compressor: {
            thresholdDb: -22,
            ratio: 1.6,
            attackMs: 15,
            releaseMs: 120,
            kneeDb: 3,
          },
        },
        saturation: {
          tape: { driveDb: 1.5, saturation: 0.25, hysteresis: 0.1 },
          exciter: { amount: 0.05, driveDb: 2 },
        },
        spectral: { airBand: { amount: 0.08 } },
        maximizer: {
          truePeakLimiter: {
            ceilingDb: -1,
            oversampleFactor: 4,
            applyGainAtInputRate: true,
          },
        },
        loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
      });

      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(result.stages).toContain('eq.tilt');
      expect(result.stages).toContain('dynamics.compressor');
      expect(result.stages).toContain('saturation.tape');
      expect(result.stages).toContain('maximizer.truePeakLimiter');
      expect(result.stages).toContain('loudness.optimize');
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
    });

    describe('color saturation stages engage only when meaningful', () => {
      const sampleRate = 22050;
      const tone = () => {
        const samples = new Float32Array(sampleRate);
        for (let i = 0; i < samples.length; i++) {
          samples[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        }
        return samples;
      };
      const stagesFor = (saturation: Record<string, unknown>): string[] =>
        masteringChain(tone(), sampleRate, { saturation }).stages;

      it('does not engage the exciter when amount is zero', () => {
        expect(stagesFor({ exciter: { amount: 0 } })).not.toContain('saturation.exciter');
      });

      it('does not engage tape when drive and saturation are zero', () => {
        expect(stagesFor({ tape: { driveDb: 0, saturation: 0 } })).not.toContain('saturation.tape');
      });

      it('engages the exciter when amount is positive', () => {
        expect(stagesFor({ exciter: { amount: 0.2 } })).toContain('saturation.exciter');
      });

      it('engages tape when drive is positive', () => {
        expect(stagesFor({ tape: { driveDb: 2 } })).toContain('saturation.tape');
      });

      it('honors an explicit enabled:true even with zero amount', () => {
        expect(stagesFor({ exciter: { amount: 0, enabled: true } })).toContain(
          'saturation.exciter',
        );
      });

      it('honors an explicit enabled:false even with meaningful params', () => {
        expect(stagesFor({ tape: { driveDb: 3, saturation: 0.5, enabled: false } })).not.toContain(
          'saturation.tape',
        );
      });
    });

    it('should run a stereo mastering chain in WASM', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate);
      const right = new Float32Array(sampleRate);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.16 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const result = masteringChainStereo(left, right, sampleRate, {
        eq: { tiltDb: 1.0 },
        dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
        saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
        stereo: {
          imager: { width: 1.15, decorrelationAmount: 0.05 },
          monoMaker: { amount: 0.2 },
        },
        loudness: {
          targetLufs: -18,
          ceilingDb: -1,
          truePeakOversample: 4,
          applyGainAtInputRate: true,
        },
      });

      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(result.stages).toContain('eq.tilt');
      expect(result.stages).toContain('stereo.imager');
      expect(result.stages).toContain('stereo.monoMaker');
      expect(result.stages).toContain('loudness.optimize');
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
    });

    it('should invoke progress callback for masteringChainWithProgress', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
      }

      const stages: string[] = [];
      const progresses: number[] = [];
      const result = masteringChainWithProgress(
        samples,
        sampleRate,
        {
          eq: { tiltDb: 1.0 },
          dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
        },
        (progress, stage) => {
          progresses.push(progress);
          stages.push(stage);
        },
      );

      expect(stages).toEqual(['eq.tilt', 'dynamics.compressor']);
      expect(progresses.length).toBe(2);
      expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
      expect(result.stages).toEqual(['eq.tilt', 'dynamics.compressor']);
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
    });

    it('should invoke progress callback for masteringChainStereoWithProgress', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate);
      const right = new Float32Array(sampleRate);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.16 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const stages: string[] = [];
      const progresses: number[] = [];
      const result = masteringChainStereoWithProgress(
        left,
        right,
        sampleRate,
        {
          eq: { tiltDb: 1.0 },
          stereo: { imager: { width: 1.1 } },
        },
        (progress, stage) => {
          progresses.push(progress);
          stages.push(stage);
        },
      );

      expect(stages).toEqual(['eq.tilt', 'stereo.imager']);
      expect(progresses.length).toBe(2);
      expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
      expect(result.stages).toEqual(['eq.tilt', 'stereo.imager']);
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
    });

    it('should expose named mastering processors in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate / 2);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }

      const names = masteringProcessorNames();
      expect(names).toContain('dynamics.compressor');
      expect(names).toContain('eq.equalizer');
      expect(names).toContain('saturation.tape');
      expect(names).toContain('stereo.imager');

      const mono = masteringProcess('dynamics.compressor', samples, sampleRate, {
        thresholdDb: -24,
        ratio: 1.5,
      });
      expect(mono.samples).toBeInstanceOf(Float32Array);
      expect(mono.samples.length).toBe(samples.length);
      expect(Number.isFinite(mono.outputLufs)).toBe(true);

      const eq = masteringProcess('eq.equalizer', samples, sampleRate, {
        'band0.enabled': 1,
        'band0.frequencyHz': 440,
        'band0.gainDb': 6,
        'band0.q': 1,
        autoGain: 1,
      });
      expect(eq.samples).toBeInstanceOf(Float32Array);
      expect(eq.samples.length).toBe(samples.length);
      expect(Number.isFinite(eq.outputLufs)).toBe(true);
    });

    it('should expose named stereo mastering processors in WASM', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate / 2);
      const right = new Float32Array(sampleRate / 2);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.2 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const result = masteringProcessStereo('stereo.imager', left, right, sampleRate, {
        width: 1.1,
      });
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(Number.isFinite(result.outputLufs)).toBe(true);

      const leftEq = masteringProcessStereo('eq.equalizer', left, left, sampleRate, {
        'band0.enabled': 1,
        'band0.frequencyHz': 220,
        'band0.gainDb': 12,
        'band0.q': 1,
        'band0.placement': 1,
      });
      const leftPeak = Math.max(...Array.from(leftEq.left, Math.abs));
      const rightPeak = Math.max(...Array.from(leftEq.right, Math.abs));
      expect(leftPeak).toBeGreaterThan(rightPeak * 1.5);

      const linearEq = masteringProcessStereo('eq.equalizer', left, left, sampleRate, {
        phaseMode: 3,
        'band0.enabled': 1,
        'band0.frequencyHz': 220,
        'band0.gainDb': 3,
        'band0.q': 1,
      });
      expect(linearEq.latencySamples).toBeGreaterThan(0);
    });

    it('should expose pair and stereo mastering APIs in WASM', () => {
      const sampleRate = 44100;
      const source = new Float32Array(sampleRate / 4);
      const reference = new Float32Array(sampleRate / 4);
      for (let i = 0; i < source.length; i++) {
        source[i] = 0.18 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
        reference[i] = 0.12 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
      }

      expect(masteringPairProcessorNames()).toContain('match.abCrossfade');
      expect(masteringPairAnalysisNames()).toContain('match.referenceLoudness');
      expect(masteringStereoAnalysisNames()).toContain('stereo.monoCompatCheck');

      const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
        mix: 0.25,
      });
      expect(paired.samples).toBeInstanceOf(Float32Array);
      expect(paired.samples.length).toBe(source.length);

      const pairJson = masteringPairAnalyze(
        'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      );
      expect(pairJson).toContain('"sourceLufs"');
      expect(pairJson).toContain('"referenceLufs"');

      const stereoJson = masteringStereoAnalyze(
        'stereo.monoCompatCheck',
        source,
        reference,
        sampleRate,
      );
      expect(stereoJson).toContain('"correlation"');
    });

    it('should accept independent source/reference lengths for pair mastering', () => {
      const sampleRate = 44100;
      const source = new Float32Array(Math.floor(sampleRate * 0.25));
      const reference = new Float32Array(Math.floor(sampleRate * 0.6)); // different duration
      for (let i = 0; i < source.length; i++) {
        source[i] = 0.18 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      for (let i = 0; i < reference.length; i++) {
        reference[i] = 0.12 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
      }

      const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
        mix: 0.25,
      });
      expect(paired.samples).toBeInstanceOf(Float32Array);
      expect(paired.samples.length).toBe(source.length);

      const pairJson = masteringPairAnalyze(
        'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      );
      expect(pairJson).toContain('"sourceLufs"');
      expect(pairJson).toContain('"referenceLufs"');
    });

    it('should expose mastering assistant suggestions in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate * 3);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
      }
      const json = masteringAssistantSuggest(samples, sampleRate, {
        targetLufs: -13,
        ceilingDb: -0.8,
        enableRepair: true,
      });
      const result = JSON.parse(json);

      expect(result).toHaveProperty('chainConfig');
      expect(result).toHaveProperty('profile');
      expect(Array.isArray(result.explanation)).toBe(true);
      expect(Array.isArray(result.genreCandidates)).toBe(true);
      expect(result.chainConfig.params['loudness.targetLufs']).toBe(-13);
      expect(result.chainConfig.params['loudness.ceilingDb']).toBeCloseTo(-0.8, 6);
      // After the chain_json.cpp migration to util::json, booleans serialize as
      // JSON `true`/`false` (per RFC 8259) instead of `1`/`0`. Both representations
      // mean the same thing to callers; the test now reflects the spec-compliant form.
      expect(result.chainConfig.params['repair.declick.enabled']).toBe(true);
    });

    it('should expose mastering audio profiles in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate * 2);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }
      const json = masteringAudioProfile(samples, sampleRate, {
        nFft: 1024,
        hopLength: 256,
      });
      const result = JSON.parse(json);

      expect(typeof result.durationSec).toBe('number');
      expect(result.durationSec).toBeGreaterThan(1.9);
      expect(result).toHaveProperty('loudness.integratedLufs');
      expect(result).toHaveProperty('spectral.centroidHz');
      expect(result).toHaveProperty('dynamics.attackDensity');
      expect(Array.isArray(result.genreCandidates)).toBe(true);
    });

    it('should expose streaming platform loudness previews in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      const json = masteringStreamingPreview(samples, sampleRate, [
        { name: 'Unit Test', targetLufs: -12, ceilingDb: -1 },
      ]);
      const result = JSON.parse(json);

      expect(result.platforms).toHaveLength(1);
      expect(result.platforms[0].name).toBe('Unit Test');
      expect(typeof result.platforms[0].integratedLufs).toBe('number');
      expect(typeof result.platforms[0].truePeakDb).toBe('number');
      expect(typeof result.platforms[0].normalizationGainDb).toBe('number');
      expect(typeof result.platforms[0].ceilingRisk).toBe('boolean');
    });

    it('should expose mixing presets and stereo mix in WASM', () => {
      expect(mixingScenePresetNames()).toContain('vocalReverbSend');
      expect(mixingScenePresetJson('vocalReverbSend')).toContain('"vocal"');

      const left = new Float32Array([1, 1]);
      const right = new Float32Array([0, 0]);
      const result = mixStereo([left], [right], 48000, { inputTrimDb: 6.0206, faderDb: -6.0206 });
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      // +6.02 dB trim and -6.02 dB fader cancel to unity. With the Balance pan
      // law no longer attenuating a centered signal by 3 dB, the output passes
      // through at unity instead of sqrt(0.5).
      expect(result.left[0]).toBeCloseTo(1.0, 2);
      expect(result.left[1]).toBeCloseTo(1.0, 2);
      expect(Array.from(result.right)).toEqual([0, 0]);
      expect(result.meters).toHaveLength(1);
      expect(Number.isFinite(result.meters[0].peakDbL)).toBe(true);
      expect(typeof result.meters[0].likelyMonoCompatible).toBe('boolean');
    });

    it('should stream a mono block through StreamingMasteringChain', () => {
      const chain = new StreamingMasteringChain({
        eq: { tiltDb: 1.0 },
      });
      try {
        chain.prepare(44100, 512, 1);
        const block = new Float32Array(512);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = 0.1;
        }
        const out = chain.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        // tilt EQ should modify the constant signal at least somewhere
        const stages = chain.stageNames();
        expect(stages).toContain('eq.tilt');
      } finally {
        chain.delete();
      }
    });

    it('should construct a loudness-enabled StreamingMasteringChain with a static gain', () => {
      // A loudness-enabled config normally throws at construction (whole-signal
      // LUFS is unavailable while streaming). Supplying a precomputed
      // loudnessStaticGainDb must let it construct, prepare, and process.
      const chain = new StreamingMasteringChain({
        eq: { tiltDb: 1.0 },
        loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
        loudnessStaticGainDb: 3.0,
        loudnessStaticGainPeakDb: -6.0,
      });
      try {
        chain.prepare(44100, 512, 1);
        const block = new Float32Array(512);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = 0.1;
        }
        const out = chain.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        // The loudness stage is wired in once a static gain is provided.
        expect(chain.stageNames()).toContain('loudness.optimize');
      } finally {
        chain.delete();
      }
    });

    it('should still throw for a loudness-enabled chain without a static gain', () => {
      expect(
        () =>
          new StreamingMasteringChain({
            loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
          }),
      ).toThrow();
    });

    it('should stream stereo blocks through StreamingEqualizer', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, {
          type: 'HighShelf',
          frequencyHz: 8000,
          gainDb: 6,
          enabled: true,
        });
        eq.setGainScale(0.5);
        eq.setOutputGainDb(3);
        eq.setOutputPan(0);

        const length = 512;
        const left = new Float32Array(length);
        const right = new Float32Array(length);
        for (let i = 0; i < length; i += 1) {
          const value = Math.sin((2 * Math.PI * 1000 * i) / 48000) * 0.5;
          left[i] = value;
          right[i] = value;
        }

        const firstSeq = eq.spectrum().seq;
        const out = eq.processStereo(left, right);
        expect(out.left).toBeInstanceOf(Float32Array);
        expect(out.right).toBeInstanceOf(Float32Array);
        expect(out.left.length).toBe(length);
        expect(out.right.length).toBe(length);

        const snapshot = eq.spectrum();
        expect(snapshot.seq).toBeGreaterThan(firstSeq);
        expect(snapshot.bandGainDb.length).toBe(24);
        expect(snapshot.bandGainDb[0]).toBeGreaterThan(2.5);
        expect(snapshot.bandGainDb[0]).toBeLessThan(3.5);
        expect(snapshot.profileDb.length).toBe(16);
        expect(snapshot.preLeft.length).toBe(snapshot.postLeft.length);
        expect(eq.latencySamples()).toBeGreaterThanOrEqual(0);
      } finally {
        eq.delete();
      }
    });

    it('should stream mono blocks through StreamingRetune', () => {
      const retune = new StreamingRetune({ semitones: 12, mix: 1, grainSize: 512 });
      try {
        retune.prepare(48000, 128);
        expect(retune.config().semitones).toBe(12);
        expect(retune.grainSize()).toBe(512);

        const block = new Float32Array(128);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.5;
        }

        const out = retune.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        expect(Array.from(out).every(Number.isFinite)).toBe(true);

        retune.setConfig({ semitones: -5, mix: 0.5, grainSize: 512 });
        expect(retune.config().mix).toBeCloseTo(0.5, 6);
      } finally {
        retune.delete();
      }
    });
  });
});
