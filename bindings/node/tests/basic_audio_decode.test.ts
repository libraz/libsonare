import { execFileSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  detectBpm,
  detectKey,
  detectKeyCandidates,
  hasFfmpegSupport,
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

  it('native detectKey instance method honors modes/profile/genreHint', () => {
    // Regression (binding-node#1): the Audio class native instance methods used
    // to call the non-extended C entry points and silently ignored
    // modes/profile/genreHint, so the same conceptual call diverged from the
    // standalone path. They now forward through the *_with_extended_options
    // entry points, so for identical options the instance method and the
    // standalone function must produce identical results.
    const samples = generateSine(440, SR, 1.0);
    const audio = Audio.fromBuffer(samples, SR);
    const native = (audio as unknown as { native: Record<string, (opts: unknown) => unknown> })
      .native;
    try {
      const opts = { modes: 'major-minor', profile: 'temperley', genreHint: 'edm' };
      const instanceKey = native.detectKey(opts) as { root: string; mode: string };
      const standaloneKey = detectKey(samples, SR, opts);
      expect(instanceKey.root).toBe(standaloneKey.root);
      expect(instanceKey.mode).toBe(standaloneKey.mode);

      const instanceCandidates = native.detectKeyCandidates(opts) as Array<{
        key: { root: string };
      }>;
      const standaloneCandidates = detectKeyCandidates(samples, SR, opts);
      expect(instanceCandidates.length).toBe(standaloneCandidates.length);
      if (standaloneCandidates.length > 0) {
        expect(instanceCandidates[0].key.root).toBe(standaloneCandidates[0].key.root);
      }
    } finally {
      audio.destroy();
    }
  });

  it('analyze via class returns beat aliases and rich key', () => {
    const audio = Audio.fromBuffer(generateSine(440, SR, 1.0), SR);
    const result = audio.analyze();
    expect(result.key.name).toContain(result.key.root);
    expect(result.beats.length).toBe(result.beatTimes.length);
    audio.destroy();
  });

  it('analysis primitives work via class', () => {
    const audio = Audio.fromBuffer(generateSine(440, SR, 2.0), SR);
    expect(audio.analyzeBpm().bpm).toBeGreaterThan(0);
    expect(audio.analyzeRhythm().bpm).toBeGreaterThan(0);
    expect(audio.analyzeDynamics().loudnessTimes).toBeInstanceOf(Float32Array);
    expect(audio.analyzeTimbre().spectralCentroid).toBeInstanceOf(Float32Array);
    expect(
      Array.isArray(
        audio.detectChords({
          minDuration: 0.3,
          smoothingWindow: 2.0,
          threshold: 0.5,
          useTriadsOnly: false,
          nFft: 2048,
          hopLength: 512,
          useBeatSync: false,
        }).chords,
      ),
    ).toBe(true);
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
