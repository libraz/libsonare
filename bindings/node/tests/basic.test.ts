import { execFileSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  amplitudeToDb,
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  chroma,
  dbToAmplitude,
  dbToPower,
  deemphasis,
  detectBeats,
  detectBpm,
  detectChords,
  detectKey,
  detectOnsets,
  fixFrames,
  fixLength,
  frameSignal,
  framesToSamples,
  framesToTime,
  harmonic,
  hasFfmpegSupport,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  mastering,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  melSpectrogram,
  melToHz,
  mfcc,
  midiToHz,
  normalize,
  noteToHz,
  pcen,
  peakPick,
  percussive,
  pitchPyin,
  pitchShift,
  pitchYin,
  plp,
  powerToDb,
  preemphasis,
  resample,
  rmsEnergy,
  samplesToFrames,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  splitSilence,
  StreamingMasteringChain,
  stft,
  stftDb,
  tempogram,
  timeStretch,
  timeToFrames,
  tonnetz,
  trim,
  trimSilence,
  vectorNormalize,
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

    it('analysis primitives expose detailed BPM and rhythm data', () => {
      const samples = new Float32Array(SR * 4);
      const samplesPerBeat = (SR * 60) / 120;
      for (let beat = 0; beat < 8; beat++) {
        const start = Math.floor(beat * samplesPerBeat);
        for (let i = start; i < Math.min(start + 200, samples.length); i++) {
          samples[i] = 1.0;
        }
      }

      const bpm = analyzeBpm(samples, SR);
      expect(bpm.bpm).toBeGreaterThan(0);
      expect(bpm.confidence).toBeGreaterThanOrEqual(0);
      expect(bpm.candidates.length).toBeLessThanOrEqual(5);
      expect(bpm.autocorrelation).toBeInstanceOf(Float32Array);
      expect(bpm.tempogram).toBeInstanceOf(Float32Array);

      const rhythm = analyzeRhythm(samples, SR);
      expect(rhythm.bpm).toBeGreaterThan(0);
      expect(rhythm.timeSignature.numerator).toBeGreaterThan(0);
      expect(['straight', 'shuffle', 'swing']).toContain(rhythm.grooveType);
      expect(rhythm.beatIntervals).toBeInstanceOf(Float32Array);

      const dynamics = analyzeDynamics(samples, SR);
      expect(dynamics.peakDb).toBeLessThanOrEqual(1);
      expect(dynamics.loudnessTimes).toBeInstanceOf(Float32Array);
      expect(dynamics.loudnessRmsDb.length).toBe(dynamics.loudnessTimes.length);

      const tone = new Float32Array(SR * 2);
      for (let i = 0; i < tone.length; i++) {
        tone[i] =
          0.25 *
          (Math.sin((2 * Math.PI * 261.63 * i) / SR) +
            Math.sin((2 * Math.PI * 329.63 * i) / SR) +
            Math.sin((2 * Math.PI * 392.0 * i) / SR));
      }
      const timbre = analyzeTimbre(tone, SR);
      expect(timbre.brightness).toBeGreaterThanOrEqual(0);
      expect(timbre.brightness).toBeLessThanOrEqual(1);
      expect(timbre.spectralCentroid).toBeInstanceOf(Float32Array);

      const chords = detectChords(tone, SR, 0.3, 2.0, 0.5, false, 2048, 512, false);
      expect(Array.isArray(chords.chords)).toBe(true);
    });

    it('rejects non-Float32Array input', () => {
      expect(() => detectBpm(new Float64Array(SR) as unknown as Float32Array, SR)).toThrow(
        /Float32Array/,
      );
    });

    it('converts invalid native arguments into JS exceptions', () => {
      expect(() => timeStretch(new Float32Array(SR), -1, 2.0)).toThrow(/Invalid parameter/);
    });

    it('exposes compatibility numeric and signal utilities', () => {
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
      const deemphasized = deemphasis(emphasized, 0.5, 0);
      expect(deemphasized[2]).toBeCloseTo(1, 5);

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

    it('exposes silence and rhythm compatibility utilities', () => {
      const samples = new Float32Array([0, 0, 1, 1, 0, 0]);
      const trimmed = trimSilence(samples, 20, 2, 1);
      expect(trimmed.audio.length).toBeGreaterThan(0);
      expect(trimmed.startSample).toBeGreaterThanOrEqual(0);
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
      const temp = tempogram(onset, SR, 512, 4);
      expect(temp.data).toBeInstanceOf(Float32Array);
      expect(temp.winLength).toBe(4);
      expect(plp(onset, SR, 512, 30, 300, 4)).toBeInstanceOf(Float32Array);
    });

    it('exposes pair and stereo mastering APIs', () => {
      const sampleRate = 44100;
      const source = generateSine(440, sampleRate, 0.25);
      const reference = generateSine(880, sampleRate, 0.25);

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

    it('mastering returns processed samples and loudness metadata', () => {
      const quiet = new Float32Array(SR);
      for (let i = 0; i < quiet.length; i++) {
        quiet[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }

      const result = mastering(quiet, SR, -18.0, -1.0, 4);
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(quiet.length);
      expect(result.sampleRate).toBe(SR);
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(Number.isFinite(result.appliedGainDb)).toBe(true);
      expect(result.outputLufs).toBeCloseTo(-18.0, 1);

      const audio = Audio.fromBuffer(quiet, SR);
      try {
        const fromAudio = audio.mastering(-18.0, -1.0, 4);
        expect(fromAudio.samples.length).toBe(quiet.length);
        expect(fromAudio.sampleRate).toBe(SR);
      } finally {
        audio.destroy();
      }
    });

    it('named mastering processors are available', () => {
      const quiet = new Float32Array(SR / 2);
      for (let i = 0; i < quiet.length; i++) {
        quiet[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / SR);
      }

      const names = masteringProcessorNames();
      expect(names).toContain('dynamics.compressor');
      expect(names).toContain('stereo.imager');

      const result = masteringProcess('dynamics.compressor', quiet, SR, {
        thresholdDb: -24,
        ratio: 1.5,
      });
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(quiet.length);

      const stereo = masteringProcessStereo('stereo.imager', quiet, quiet, SR, { width: 1.1 });
      expect(stereo.left).toBeInstanceOf(Float32Array);
      expect(stereo.right.length).toBe(quiet.length);
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

    it('analysis primitives work via class', () => {
      const audio = Audio.fromBuffer(generateSine(440, SR, 2.0), SR);
      expect(audio.analyzeBpm().bpm).toBeGreaterThan(0);
      expect(audio.analyzeRhythm().bpm).toBeGreaterThan(0);
      expect(audio.analyzeDynamics().loudnessTimes).toBeInstanceOf(Float32Array);
      expect(audio.analyzeTimbre().spectralCentroid).toBeInstanceOf(Float32Array);
      expect(Array.isArray(audio.detectChords(0.3, 2.0, 0.5, false, 2048, 512, false).chords)).toBe(
        true,
      );
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
