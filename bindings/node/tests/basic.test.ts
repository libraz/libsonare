import { describe, expect, it } from 'vitest';
import {
  Audio,
  analyze,
  chroma,
  detectBeats,
  detectBpm,
  detectKey,
  detectOnsets,
  framesToTime,
  harmonic,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  melSpectrogram,
  melToHz,
  mfcc,
  midiToHz,
  normalize,
  noteToHz,
  percussive,
  pitchPyin,
  pitchShift,
  pitchYin,
  resample,
  rmsEnergy,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  timeStretch,
  timeToFrames,
  trim,
  version,
  zeroCrossingRate,
} from '../src/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('sonare native binding', () => {
  describe('standalone functions', () => {
    it('version returns a string', () => {
      const v = version();
      expect(typeof v).toBe('string');
      expect(v.length).toBeGreaterThan(0);
    });

    it('detectBpm returns a number', () => {
      const bpm = detectBpm(new Float32Array(SR), SR);
      expect(typeof bpm).toBe('number');
      expect(bpm).toBeGreaterThanOrEqual(0);
    });

    it('detectKey returns key object', () => {
      const key = detectKey(new Float32Array(SR), SR);
      expect(typeof key.root).toBe('string');
      expect(typeof key.mode).toBe('string');
      expect(typeof key.confidence).toBe('number');
    });

    it('detectBeats returns Float32Array', () => {
      expect(detectBeats(new Float32Array(SR), SR)).toBeInstanceOf(Float32Array);
    });

    it('detectOnsets returns Float32Array', () => {
      expect(detectOnsets(new Float32Array(SR), SR)).toBeInstanceOf(Float32Array);
    });

    it('analyze returns full result', () => {
      const result = analyze(new Float32Array(SR), SR);
      expect(result).toHaveProperty('bpm');
      expect(result).toHaveProperty('key');
      expect(result).toHaveProperty('timeSignature');
      expect(result).toHaveProperty('beatTimes');
      expect(result.beatTimes).toBeInstanceOf(Float32Array);
    });
  });

  describe('effects', () => {
    const tone = generateSine(440, SR, 1.0);

    it('hpss returns harmonic and percussive', () => {
      const result = hpss(tone, SR);
      expect(result.harmonic).toBeInstanceOf(Float32Array);
      expect(result.percussive).toBeInstanceOf(Float32Array);
      expect(result.harmonic.length).toBe(tone.length);
    });

    it('harmonic returns Float32Array', () => {
      const result = harmonic(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBe(tone.length);
    });

    it('percussive returns Float32Array', () => {
      const result = percussive(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBe(tone.length);
    });

    it('timeStretch changes length', () => {
      const fast = timeStretch(tone, SR, 2.0);
      expect(fast.length).toBeLessThan(tone.length);
      const slow = timeStretch(tone, SR, 0.5);
      expect(slow.length).toBeGreaterThan(tone.length);
    });

    it('pitchShift returns Float32Array', () => {
      const result = pitchShift(tone, SR, 2);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('normalize sets peak close to target', () => {
      const quiet = new Float32Array(SR);
      for (let i = 0; i < SR; i++) {
        quiet[i] = 0.1 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }
      const result = normalize(quiet, SR, 0.0);
      let peak = 0;
      for (let i = 0; i < result.length; i++) {
        const abs = Math.abs(result[i]);
        if (abs > peak) {
          peak = abs;
        }
      }
      expect(peak).toBeGreaterThan(0.8);
      expect(peak).toBeLessThanOrEqual(1.01);
    });

    it('trim removes silence', () => {
      const samples = new Float32Array(SR);
      const start = Math.floor(SR * 0.25);
      const end = Math.floor(SR * 0.75);
      for (let i = start; i < end; i++) {
        samples[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }
      const result = trim(samples, SR, -40.0);
      expect(result.length).toBeLessThan(samples.length);
      expect(result.length).toBeGreaterThan(0);
    });
  });

  describe('features', () => {
    const tone = generateSine(440, SR, 1.0);

    it('stft returns magnitude and power', () => {
      const result = stft(tone, SR, 2048, 512);
      expect(result.nBins).toBe(1025);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.magnitude).toBeInstanceOf(Float32Array);
      expect(result.power).toBeInstanceOf(Float32Array);
      expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
    });

    it('stftDb returns dB values', () => {
      const result = stftDb(tone, SR, 2048, 512);
      expect(result.nBins).toBe(1025);
      expect(result.db).toBeInstanceOf(Float32Array);
      for (let i = 0; i < result.db.length; i++) {
        expect(Number.isFinite(result.db[i])).toBe(true);
      }
    });

    it('melSpectrogram returns power and dB', () => {
      const result = melSpectrogram(tone, SR, 2048, 512, 128);
      expect(result.nMels).toBe(128);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.power).toBeInstanceOf(Float32Array);
      expect(result.db).toBeInstanceOf(Float32Array);
    });

    it('mfcc returns coefficients', () => {
      const result = mfcc(tone, SR, 2048, 512, 64, 13);
      expect(result.nMfcc).toBe(13);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.coefficients).toBeInstanceOf(Float32Array);
      expect(result.coefficients.length).toBe(result.nMfcc * result.nFrames);
    });

    it('chroma returns 12 pitch classes', () => {
      const result = chroma(tone, SR, 2048, 512);
      expect(result.nChroma).toBe(12);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.features).toBeInstanceOf(Float32Array);
      expect(result.meanEnergy.length).toBe(12);
    });

    it('spectralCentroid returns Float32Array', () => {
      const result = spectralCentroid(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('spectralBandwidth returns Float32Array', () => {
      const result = spectralBandwidth(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('spectralRolloff returns Float32Array', () => {
      const result = spectralRolloff(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('spectralFlatness returns Float32Array', () => {
      const result = spectralFlatness(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(0);
    });

    it('zeroCrossingRate returns values in [0, 1]', () => {
      const result = zeroCrossingRate(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      for (let i = 0; i < result.length; i++) {
        expect(result[i]).toBeGreaterThanOrEqual(0);
        expect(result[i]).toBeLessThanOrEqual(1);
      }
    });

    it('rmsEnergy returns non-negative values', () => {
      const result = rmsEnergy(tone, SR);
      expect(result).toBeInstanceOf(Float32Array);
      for (let i = 0; i < result.length; i++) {
        expect(result[i]).toBeGreaterThanOrEqual(0);
      }
    });

    it('pitchYin detects 440 Hz', () => {
      const result = pitchYin(tone, SR);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);
      expect(result.f0).toBeInstanceOf(Float32Array);
      expect(result.voicedProb).toBeInstanceOf(Float32Array);
    });

    it('pitchPyin detects 440 Hz', () => {
      const result = pitchPyin(tone, SR);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);
    });
  });

  describe('unit conversions', () => {
    it('hzToMel and melToHz round-trip', () => {
      const mel = hzToMel(440);
      expect(mel).toBeGreaterThan(0);
      const hz = melToHz(mel);
      expect(hz).toBeCloseTo(440, 0);
    });

    it('hzToMidi(440) returns 69', () => {
      expect(hzToMidi(440)).toBeCloseTo(69, 2);
    });

    it('midiToHz(69) returns 440', () => {
      expect(midiToHz(69)).toBeCloseTo(440, 2);
    });

    it('hzToNote(440) returns A4', () => {
      expect(hzToNote(440)).toBe('A4');
    });

    it('noteToHz("A4") returns ~440', () => {
      expect(noteToHz('A4')).toBeCloseTo(440, 1);
    });

    it('framesToTime computes correct time', () => {
      expect(framesToTime(1, SR, 512)).toBeCloseTo(512 / SR, 5);
    });

    it('timeToFrames computes correct frame', () => {
      expect(timeToFrames(1.0, SR, 512)).toBe(Math.floor(SR / 512));
    });
  });

  describe('resample', () => {
    it('doubles length when upsampling 22050->44100', () => {
      const tone = generateSine(440, SR, 1.0);
      const result = resample(tone, SR, 44100);
      expect(result).toBeInstanceOf(Float32Array);
      expect(result.length).toBeGreaterThan(tone.length * 1.8);
      expect(result.length).toBeLessThan(tone.length * 2.2);
    });
  });

  describe('Audio class methods', () => {
    it('detectBpm matches standalone', () => {
      const samples = generateSine(440, SR, 1.0);
      const audio = Audio.fromBuffer(samples, SR);
      expect(audio.detectBpm()).toBe(detectBpm(samples, SR));
      audio.destroy();
    });

    it('detectKey matches standalone', () => {
      const samples = generateSine(440, SR, 1.0);
      const audio = Audio.fromBuffer(samples, SR);
      const classKey = audio.detectKey();
      const funcKey = detectKey(samples, SR);
      expect(classKey.root).toBe(funcKey.root);
      expect(classKey.mode).toBe(funcKey.mode);
      audio.destroy();
    });

    it('stft works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.stft();
      expect(result.nBins).toBe(1025);
      expect(result.magnitude).toBeInstanceOf(Float32Array);
      audio.destroy();
    });

    it('melSpectrogram works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.melSpectrogram();
      expect(result.nMels).toBe(128);
      audio.destroy();
    });

    it('chroma works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.chroma();
      expect(result.nChroma).toBe(12);
      audio.destroy();
    });

    it('hpss works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.hpss();
      expect(result.harmonic).toBeInstanceOf(Float32Array);
      expect(result.percussive).toBeInstanceOf(Float32Array);
      audio.destroy();
    });

    it('pitchYin works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.pitchYin();
      expect(result.medianF0).toBeGreaterThan(400);
      expect(result.medianF0).toBeLessThan(480);
      audio.destroy();
    });

    it('resample works via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.resample(44100);
      expect(result.length).toBeGreaterThan(SR * 1.8);
      audio.destroy();
    });
  });
});
