/**
 * Tests for the Audio convenience class.
 *
 * The Audio class delegates to standalone functions which are thoroughly tested
 * elsewhere. These tests verify the class API works end-to-end: construction,
 * property getters, and that each method returns the expected type/shape.
 */

import { beforeAll, describe, expect, it, vi } from 'vitest';
import { Audio, detectBpm, init, melSpectrogram, stft } from '../dist/index.js';

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

function pcm16Wav(samples: Int16Array, sampleRate: number): Uint8Array {
  const bytesPerSample = 2;
  const dataBytes = samples.length * bytesPerSample;
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);
  let offset = 0;
  const text = (value: string) => {
    for (let i = 0; i < value.length; i++) {
      view.setUint8(offset++, value.charCodeAt(i));
    }
  };
  text('RIFF');
  view.setUint32(offset, 36 + dataBytes, true);
  offset += 4;
  text('WAVE');
  text('fmt ');
  view.setUint32(offset, 16, true);
  offset += 4;
  view.setUint16(offset, 1, true);
  offset += 2;
  view.setUint16(offset, 1, true);
  offset += 2;
  view.setUint32(offset, sampleRate, true);
  offset += 4;
  view.setUint32(offset, sampleRate * bytesPerSample, true);
  offset += 4;
  view.setUint16(offset, bytesPerSample, true);
  offset += 2;
  view.setUint16(offset, 16, true);
  offset += 2;
  text('data');
  view.setUint32(offset, dataBytes, true);
  offset += 4;
  for (const sample of samples) {
    view.setInt16(offset, sample, true);
    offset += 2;
  }
  return new Uint8Array(buffer);
}

function fakeAudioBuffer(channels: Float32Array[], sampleRate: number): AudioBuffer {
  return {
    length: channels[0]?.length ?? 0,
    numberOfChannels: channels.length,
    sampleRate,
    getChannelData: (channel: number) => channels[channel],
  } as AudioBuffer;
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

    it('should create an Audio instance by decoding bytes from memory', () => {
      const bytes = pcm16Wav(new Int16Array([0, 8192, -8192, 16384]), 8000);
      const audio = Audio.fromMemory(bytes);
      expect(audio).toBeInstanceOf(Audio);
      expect(audio.sampleRate).toBe(8000);
      expect(audio.length).toBe(4);
      expect(audio.data[0]).toBeCloseTo(0, 6);
      expect(audio.data[1]).toBeCloseTo(8192 / 32768, 4);
      expect(audio.data[2]).toBeCloseTo(-8192 / 32768, 4);
    });

    it('should keep the native decoder path for supported memory formats', async () => {
      const bytes = pcm16Wav(new Int16Array([0, 8192]), 8000);
      const decodeAudioData = vi.fn(async () => {
        throw new Error('fallback should not run');
      });

      const audio = await Audio.fromMemoryWithBrowserFallback(bytes, {
        audioContext: {
          decodeAudioData,
          sampleRate: 44100,
        },
      });

      expect(audio.sampleRate).toBe(8000);
      expect(audio.length).toBe(2);
      expect(decodeAudioData).not.toHaveBeenCalled();
    });

    it('should fall back to browser decodeAudioData and mix down channels', async () => {
      const bytes = new Uint8Array([1, 2, 3, 4, 5]);
      const decodeAudioData = vi.fn(async (data: ArrayBuffer) => {
        expect(data.byteLength).toBe(bytes.byteLength);
        return fakeAudioBuffer(
          [new Float32Array([1, 0, -1]), new Float32Array([0, 0.5, 1])],
          44100,
        );
      });

      const audio = await Audio.fromMemoryWithBrowserFallback(bytes, {
        audioContext: {
          decodeAudioData,
          sampleRate: 44100,
        },
      });

      expect(decodeAudioData).toHaveBeenCalledTimes(1);
      expect(audio.sampleRate).toBe(44100);
      expect(Array.from(audio.data)).toEqual([0.5, 0.25, 0]);
    });

    it('should pass targetSampleRate to created browser fallback contexts', async () => {
      const close = vi.fn(async () => undefined);
      const createAudioContext = vi.fn(() => ({
        decodeAudioData: async () => fakeAudioBuffer([new Float32Array([0.25, -0.25])], 32000),
        sampleRate: 32000,
        close,
      }));

      const audio = await Audio.fromMemoryWithBrowserFallback(new Uint8Array([9, 8, 7]), {
        createAudioContext,
        targetSampleRate: 32000,
      });

      expect(createAudioContext).toHaveBeenCalledWith({ sampleRate: 32000 });
      expect(close).toHaveBeenCalledTimes(1);
      expect(audio.sampleRate).toBe(32000);
      expect(Array.from(audio.data)).toEqual([0.25, -0.25]);
    });

    it('should report both decoder failures when browser fallback also fails', async () => {
      await expect(
        Audio.fromMemoryWithBrowserFallback(new Uint8Array([1]), {
          audioContext: {
            decodeAudioData: async () => {
              throw new Error('browser decode failed');
            },
            sampleRate: 48000,
          },
        }),
      ).rejects.toThrow(/browser decodeAudioData fallback failed: browser decode failed/);
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

    it('should master to target loudness', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.mastering({ targetLufs: -18.0, ceilingDb: -1.0, truePeakOversample: 4 });
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(audio.length);
      expect(result.sampleRate).toBe(SR);
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(Number.isFinite(result.appliedGainDb)).toBe(true);
    });

    it('should run a mastering chain', () => {
      const audio = Audio.fromBuffer(sine(), SR);
      const result = audio.masteringChain({
        eq: { tiltDb: 1.0 },
        dynamics: { compressor: { thresholdDb: -24.0, ratio: 1.5 } },
        saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
        loudness: { targetLufs: -18.0, ceilingDb: -1.0, truePeakOversample: 4 },
      });
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(audio.length);
      expect(result.stages).toContain('eq.tilt');
      expect(result.stages).toContain('dynamics.compressor');
      expect(result.stages).toContain('saturation.tape');
      expect(result.stages).toContain('loudness.optimize');
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
      expect(result.nMfcc).toBe(20);
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
