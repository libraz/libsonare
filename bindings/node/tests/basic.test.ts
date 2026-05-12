import { execFileSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
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
  hasFfmpegSupport,
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

function findFfmpegCli(): string | null {
  try {
    const result = execFileSync('which', ['ffmpeg'], { encoding: 'utf-8' }).trim();
    return result || null;
  } catch {
    return null;
  }
}

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
      expect(typeof key.name).toBe('string');
      expect(typeof key.shortName).toBe('string');
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
      expect(result).toHaveProperty('beats');
      expect(result.beatTimes).toBeInstanceOf(Float32Array);
      expect(Array.isArray(result.beats)).toBe(true);
    });

    it('rejects non-Float32Array input', () => {
      expect(() => detectBpm(new Float64Array(SR) as unknown as Float32Array, SR)).toThrow(
        /Float32Array/,
      );
    });

    it('converts invalid native arguments into JS exceptions', () => {
      expect(() => timeStretch(new Float32Array(SR), -1, 2.0)).toThrow(/Invalid parameter/);
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
      expect(classKey.name).toBe(funcKey.name);
      audio.destroy();
    });

    it('analyze via class returns beat aliases and rich key', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
      const result = audio.analyze();
      expect(result.key.name).toContain(result.key.root);
      expect(result.beats.length).toBe(result.beatTimes.length);
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

  describe('error message propagation', () => {
    it('hasFfmpegSupport returns a boolean', () => {
      expect(typeof hasFfmpegSupport()).toBe('boolean');
    });

    it('Audio.fromFile on unsupported format throws actionable error', () => {
      // Write a tiny non-audio file with an .m4a extension into a tmp dir so we
      // exercise the unsupported-format error path regardless of whether the
      // build links FFmpeg (in that case libavformat raises a decode error).
      const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sonare-test-'));
      const tmpPath = path.join(tmpDir, 'fake.m4a');
      fs.writeFileSync(tmpPath, Buffer.from('not really an m4a file'));
      try {
        let caught: Error | null = null;
        try {
          Audio.fromFile(tmpPath);
        } catch (e) {
          caught = e as Error;
        }
        expect(caught).not.toBeNull();
        expect(caught?.message.length ?? 0).toBeGreaterThan(20);
        if (!hasFfmpegSupport()) {
          // Without FFmpeg the message must point to the actionable hint
          // produced by load_audio (extension + SONARE_WITH_FFMPEG note).
          expect(caught?.message).toContain("'.m4a'");
          expect(caught?.message).toContain('SONARE_WITH_FFMPEG');
        }
        // With FFmpeg the message comes from libavformat; just require it's
        // non-trivial (asserted above via length > 20).
      } finally {
        fs.rmSync(tmpDir, { recursive: true, force: true });
      }
    });

    it('Audio.fromMemory on garbage bytes throws non-empty error', () => {
      let caught: Error | null = null;
      try {
        Audio.fromMemory(Buffer.from('garbage'));
      } catch (e) {
        caught = e as Error;
      }
      expect(caught).not.toBeNull();
      expect(caught?.message.length ?? 0).toBeGreaterThan(5);
    });
  });

  describe('FFmpeg decode (skipped without build support or ffmpeg CLI)', () => {
    const ffmpegCli = findFfmpegCli();
    const canRun = hasFfmpegSupport() && ffmpegCli !== null;

    it.skipIf(!canRun)('Audio.fromFile decodes a real .m4a file', () => {
      const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sonare-m4a-'));
      const wavPath = path.join(tmpDir, 'tone.wav');
      const m4aPath = path.join(tmpDir, 'tone.m4a');
      try {
        // Generate 0.5s 440Hz mono 22050Hz WAV via ffmpeg, transcode to AAC/m4a.
        execFileSync(
          ffmpegCli as string,
          [
            '-f',
            'lavfi',
            '-i',
            'sine=frequency=440:duration=0.5:sample_rate=22050',
            '-ac',
            '1',
            '-y',
            wavPath,
          ],
          { stdio: 'pipe' },
        );
        execFileSync(
          ffmpegCli as string,
          ['-i', wavPath, '-c:a', 'aac', '-b:a', '64k', '-y', m4aPath],
          { stdio: 'pipe' },
        );

        const audio = Audio.fromFile(m4aPath);
        const samples = audio.getData();
        const sr = audio.getSampleRate();
        expect(sr).toBeGreaterThan(0);
        expect(samples).toBeInstanceOf(Float32Array);
        expect(samples.length).toBeGreaterThan(1000);
        // Sanity: sample values should be bounded in [-1, 1] with audible level.
        let peak = 0;
        for (let i = 0; i < samples.length; i++) {
          peak = Math.max(peak, Math.abs(samples[i]));
        }
        expect(peak).toBeGreaterThan(0.01);
        expect(peak).toBeLessThanOrEqual(1.0);
        audio.destroy();
      } finally {
        fs.rmSync(tmpDir, { recursive: true, force: true });
      }
    });
  });
});
