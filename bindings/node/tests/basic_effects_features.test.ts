import { describe, expect, it } from 'vitest';
import {
  Audio,
  bassChroma,
  chroma,
  chromaCens,
  framesToTime,
  harmonic,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  mastering,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
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
    const fast = timeStretch(tone, 2.0, SR);
    expect(fast.length).toBeLessThan(tone.length);
    const slow = timeStretch(tone, 0.5, SR);
    expect(slow.length).toBeGreaterThan(tone.length);
  });

  it('pitchShift returns Float32Array', () => {
    const result = pitchShift(tone, 2, SR);
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

    const result = mastering(quiet, SR, {
      targetLufs: -18.0,
      ceilingDb: -1.0,
      truePeakOversample: 4,
    });
    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(quiet.length);
    expect(result.sampleRate).toBe(SR);
    expect(Number.isFinite(result.inputLufs)).toBe(true);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
    expect(Number.isFinite(result.appliedGainDb)).toBe(true);
    expect(result.outputLufs).toBeCloseTo(-18.0, 1);

    const audio = Audio.fromBuffer(quiet, SR);
    try {
      const fromAudio = audio.mastering({
        targetLufs: -18.0,
        ceilingDb: -1.0,
        truePeakOversample: 4,
      });
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
    expect(names).toContain('eq.equalizer');
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

    const eq = masteringProcess('eq.equalizer', quiet, SR, {
      'band0.enabled': 1,
      'band0.frequencyHz': 440,
      'band0.gainDb': 6,
      'band0.q': 1,
      autoGain: 1,
    });
    expect(eq.samples).toBeInstanceOf(Float32Array);
    expect(eq.samples.length).toBe(quiet.length);
    expect(Number.isFinite(eq.outputLufs)).toBe(true);

    const leftEq = masteringProcessStereo('eq.equalizer', quiet, quiet, SR, {
      'band0.enabled': 1,
      'band0.frequencyHz': 440,
      'band0.gainDb': 12,
      'band0.q': 1,
      'band0.placement': 1,
    });
    const leftPeak = Math.max(...leftEq.left.map((sample) => Math.abs(sample)));
    const rightPeak = Math.max(...leftEq.right.map((sample) => Math.abs(sample)));
    expect(leftPeak).toBeGreaterThan(rightPeak * 1.5);

    const linearEq = masteringProcessStereo('eq.equalizer', quiet, quiet, SR, {
      phaseMode: 3,
      'band0.enabled': 1,
      'band0.frequencyHz': 440,
      'band0.gainDb': 3,
      'band0.q': 1,
    });
    expect(linearEq.latencySamples).toBeGreaterThan(0);
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

  it('melSpectrogram honours an explicit Mel range (fmin/fmax/htk)', () => {
    const base = melSpectrogram(tone, SR, 2048, 512, 40);
    const ranged = melSpectrogram(tone, SR, 2048, 512, 40, 500, 4000);
    const htk = melSpectrogram(tone, SR, 2048, 512, 40, 0, 0, true);
    expect(ranged.power.length).toBe(base.power.length);
    let dRange = 0;
    let dHtk = 0;
    for (let i = 0; i < base.power.length; i++) {
      dRange += (base.power[i] - ranged.power[i]) ** 2;
      dHtk += (base.power[i] - htk.power[i]) ** 2;
    }
    expect(dRange).toBeGreaterThan(1e-6);
    expect(dHtk).toBeGreaterThan(1e-6);
  });

  it('mfcc returns coefficients', () => {
    const result = mfcc(tone, SR, 2048, 512, 64, 20);
    expect(result.nMfcc).toBe(20);
    expect(result.nFrames).toBeGreaterThan(0);
    expect(result.coefficients).toBeInstanceOf(Float32Array);
    expect(result.coefficients.length).toBe(result.nMfcc * result.nFrames);
  });

  it('mfcc honours an explicit Mel range (fmin/fmax)', () => {
    const base = mfcc(tone, SR, 2048, 512, 64, 13);
    const ranged = mfcc(tone, SR, 2048, 512, 64, 13, 500, 4000);
    expect(ranged.coefficients.length).toBe(base.coefficients.length);
    let d = 0;
    for (let i = 0; i < base.coefficients.length; i++) {
      d += (base.coefficients[i] - ranged.coefficients[i]) ** 2;
    }
    expect(d).toBeGreaterThan(1e-6);
  });

  it('mfcc defaults nMfcc to 20', () => {
    const result = mfcc(tone, SR, 2048, 512, 64);
    expect(result.nMfcc).toBe(20);
  });

  it('chroma returns 12 pitch classes', () => {
    const result = chroma(tone, SR, 2048, 512);
    expect(result.nChroma).toBe(12);
    expect(result.nFrames).toBeGreaterThan(0);
    expect(result.features).toBeInstanceOf(Float32Array);
    expect(result.meanEnergy.length).toBe(12);
  });

  it('chromaCens and bassChroma return 12 pitch classes', () => {
    for (const result of [chromaCens(tone, SR), bassChroma(tone, SR)]) {
      expect(result.nChroma).toBe(12);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.features).toBeInstanceOf(Float32Array);
      expect(result.features.length).toBe(result.nChroma * result.nFrames);
      expect(result.meanEnergy.length).toBe(12);
    }
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
