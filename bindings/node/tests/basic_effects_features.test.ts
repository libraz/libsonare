import { describe, expect, it } from 'vitest';
import {
  Audio,
  bassChroma,
  chroma,
  chromaCens,
  chromaCqt,
  cqt,
  cqtToAudio,
  framesToTime,
  griffinLim,
  harmonic,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringNormalizeLoudness,
  masteringPresetNames,
  masteringProcessorNames,
  melDelta,
  melSpectrogram,
  melToAudio,
  melToHz,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
  midiToHz,
  normalize,
  noteSegments,
  noteToHz,
  onsetBacktrack,
  percussive,
  piptrack,
  pitchPyin,
  pitchShift,
  pitchYin,
  powerToDb,
  reassignedSpectrogram,
  resample,
  rmsEnergy,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralFlux,
  spectralRolloff,
  stft,
  stftDb,
  timeStretch,
  timeToFrames,
  trim,
  trimSilence,
  vqt,
  vqtToAudio,
  zeroCrossingRate,
} from '../src/index.js';

const SR = 22050;

const matrix = (data: Float32Array, rows: number, cols: number) => ({ data, rows, cols });

function generateSine(freq: number, sr: number, duration: number, amp = 1): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('effects', () => {
  const tone = generateSine(440, SR, 1.0);

  it('melDelta returns the slope of a linear feature row', () => {
    const result = melDelta({
      features: matrix(new Float32Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]), 2, 5),
      width: 5,
    });
    expect([...result.data]).toEqual(Array(10).fill(1));
  });

  it('piptrack returns equally shaped pitch and magnitude matrices', () => {
    const result = piptrack(tone, SR, { nFft: 1024, hopLength: 256, fminHz: 100, fmaxHz: 1000 });
    expect(result.pitches.rows).toBe(513);
    expect(result.pitches.data.length).toBe(result.pitches.rows * result.pitches.cols);
    expect(result.magnitudes.rows).toBe(result.pitches.rows);
    expect(result.magnitudes.cols).toBe(result.pitches.cols);
    expect(result.magnitudes.data.length).toBe(result.pitches.data.length);
  });

  it('reassignedSpectrogram returns equally shaped coordinate matrices', () => {
    const result = reassignedSpectrogram(tone, SR, { nFft: 1024, hopLength: 256 });
    const { rows, cols } = result.magnitude;
    expect(rows).toBe(513);
    expect(cols).toBeGreaterThan(0);
    expect(result.times.data.length).toBe(rows * cols);
    expect(result.frequencies.data.length).toBe(rows * cols);
    expect(result.magnitude.data.length).toBe(rows * cols);
    expect([result.times.rows, result.times.cols]).toEqual([rows, cols]);
    expect([result.frequencies.rows, result.frequencies.cols]).toEqual([rows, cols]);
  });

  it('segmentCrossSimilarity returns a matrix with the requested column shape', () => {
    const result = segmentCrossSimilarity({
      x: matrix(new Float32Array([1, 0, 1, 0, 1, 1]), 2, 3),
      y: matrix(new Float32Array([1, 1, 0, 0, 1, 1]), 2, 3),
    });
    expect(result.rows).toBe(3);
    expect(result.cols).toBe(3);
    expect(result.data).toHaveLength(9);
  });

  it('exposes the remaining librosa.segment operations', () => {
    const data = new Float32Array([1, 0, 1, 0, 1, 1]);
    const features = matrix(data, 2, 3);
    const recurrence = segmentRecurrenceMatrix({ data: features });
    expect(recurrence.data).toHaveLength(9);
    const lag = segmentRecurrenceToLag({ recurrence, pad: true });
    expect(lag.rows).toBe(6);
    expect(lag.cols).toBe(3);
    expect(segmentLagToRecurrence({ lag }).data).toHaveLength(9);
    // librosa 0.11.0 recurrence_to_lag on rec[r][c] = 4r + c: pad defaults to true, lag-by-time.
    const asymmetric = Float32Array.from({ length: 16 }, (_, i) => i);
    const padded = segmentRecurrenceToLag({ recurrence: matrix(asymmetric, 4, 4) });
    expect(padded.rows).toBe(8);
    expect(padded.cols).toBe(4);
    expect(Array.from(padded.data)).toEqual([
      0, 5, 10, 15, 4, 9, 14, 0, 8, 13, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 2, 7, 0, 1,
      6, 11,
    ]);
    expect(Array.from(segmentLagToRecurrence({ lag: padded }).data)).toEqual(
      Array.from(asymmetric),
    );
    expect(segmentSubsegment({ data: features, boundaries: new Int32Array([0, 3]) })[0]).toBe(0);
    expect(segmentAgglomerative({ data: features, k: 2 })).toHaveLength(3);
    expect(segmentPathEnhance({ recurrence, win: 1 }).data).toHaveLength(9);
  });

  it('refuses a feature matrix the segment primitives cannot order', () => {
    // These entries take a feature matrix rather than audio, so nothing upstream
    // has checked it, and one non-finite cell poisons every distance drawn from
    // its column. The facade scans the matrix and names the offending index.
    const rows = 4;
    const cols = 64;
    const clean = new Float32Array(rows * cols);
    for (let i = 0; i < clean.length; i++) {
      clean[i] = Math.sin(i * 0.37) * 0.5 + 0.5;
    }
    const poisoned = Float32Array.from(clean);
    poisoned[30] = Number.NaN;

    const poisonedMatrix = matrix(poisoned, rows, cols);
    const cleanMatrix = matrix(clean, rows, cols);

    expect(() => segmentRecurrenceMatrix({ data: poisonedMatrix })).toThrow(
      /data\.data contains a non-finite value at index 30/,
    );
    expect(() => segmentCrossSimilarity({ x: poisonedMatrix, y: cleanMatrix })).toThrow(
      /x\.data contains a non-finite value at index 30/,
    );
    expect(() =>
      segmentSubsegment({ data: poisonedMatrix, boundaries: new Int32Array([0, 32]) }),
    ).toThrow(/data\.data contains a non-finite value at index 30/);
    expect(() => segmentAgglomerative({ data: poisonedMatrix, k: 2 })).toThrow(
      /data\.data contains a non-finite value at index 30/,
    );

    // The same matrix without the one cell is accepted and every output is a
    // number, so the refusals above are not a dimension check firing.
    const accepted = segmentRecurrenceMatrix({ data: cleanMatrix });
    expect(accepted.data).toHaveLength(cols * cols);
    expect(accepted.data.every((v) => Number.isFinite(v))).toBe(true);

    // The reshaping entries scan their matrix too, so a poisoned cell is refused
    // there as well instead of being carried into the result.
    expect(() =>
      segmentRecurrenceToLag({ recurrence: matrix(poisoned, 16, 16), pad: false }),
    ).toThrow(/recurrence\.data contains a non-finite value at index 30/);
  });

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
    const fast = timeStretch(tone, SR, { speedRatio: 2.0 });
    expect(fast.length).toBeLessThan(tone.length);
    const slow = timeStretch(tone, SR, { speedRatio: 0.5 });
    expect(slow.length).toBeGreaterThan(tone.length);
  });

  it('pitchShift returns Float32Array', () => {
    const result = pitchShift(tone, SR, { pitchSemitones: 2 });
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('pitchShift rejects an unsupported effective rate before processing input', () => {
    expect(() => pitchShift(new Float32Array(4), 48000, { pitchSemitones: -120 })).toThrow();
    expect(pitchShift(tone, SR, { pitchSemitones: 0 })).toBeInstanceOf(Float32Array);
  });

  it('normalize sets peak close to target', () => {
    const quiet = new Float32Array(SR);
    for (let i = 0; i < SR; i++) {
      quiet[i] = 0.1 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }
    const result = normalize(quiet, SR, { targetDb: 0.0 });
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

  it('masteringNormalizeLoudness returns processed samples and loudness metadata', () => {
    const quiet = new Float32Array(SR);
    for (let i = 0; i < quiet.length; i++) {
      quiet[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }

    const result = masteringNormalizeLoudness(quiet, SR, {
      targetLufs: -18.0,
      ceilingDbtp: -1.0,
      truePeakOversample: 4,
    });
    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(quiet.length);
    expect(result.sampleRate).toBe(SR);
    expect(Number.isFinite(result.inputLufs)).toBe(true);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
    expect(Number.isFinite(result.appliedGainDb)).toBe(true);
    expect(typeof result.loudnessTargetLimited).toBe('boolean');
    expect(result.outputLufs).toBeCloseTo(-18.0, 1);

    const ceilingLimited = masteringNormalizeLoudness(quiet, SR, {
      targetLufs: -2.0,
      ceilingDbtp: -1.0,
      truePeakOversample: 4,
    });
    expect(ceilingLimited.loudnessTargetLimited).toBe(true);

    const audio = Audio.fromBuffer(quiet, SR);
    try {
      const fromAudio = audio.masteringNormalizeLoudness({
        targetLufs: -18.0,
        ceilingDbtp: -1.0,
        truePeakOversample: 4,
      });
      expect(fromAudio.samples.length).toBe(quiet.length);
      expect(fromAudio.sampleRate).toBe(SR);
    } finally {
      audio.destroy();
    }
  });

  it('reports the same loudnessTargetLimited from masteringNormalizeLoudness and masteringApplyProcessor', () => {
    // Peak-normalized material asked for -6 LUFS under a -3 dBTP ceiling: the
    // ceiling, not the target, decides the level. The named processor has to
    // report the same computed truth as masteringNormalizeLoudness().
    const peak = new Float32Array(SR);
    for (let i = 0; i < peak.length; i++) {
      peak[i] = Math.sin((2 * Math.PI * 440 * i) / SR);
    }
    const options = { targetLufs: -6, ceilingDbtp: -3, truePeakOversample: 4 };
    const simple = masteringNormalizeLoudness(peak, SR, options);
    const named = masteringApplyProcessor(peak, SR, {
      processor: 'maximizer.loudnessOptimize',
      params: options as never,
    });
    expect(simple.loudnessTargetLimited).toBe(true);
    expect(named.loudnessTargetLimited).toBe(simple.loudnessTargetLimited);
    expect(named.outputLufs).toBeLessThan(options.targetLufs);
  });

  it('reports the same loudnessTargetLimited from the mono and stereo named processors', () => {
    // A lone full-scale transient peak-normalizes the source without lifting its
    // program loudness, so the -3 dBTP ceiling blocks the requested -6 LUFS
    // boost on both paths by a margin far wider than the 3 dB BS.1770
    // channel-summing offset between mono and dual-mono stereo.
    const peaky = new Float32Array(SR);
    for (let i = 0; i < peaky.length; i++) {
      peaky[i] = 0.05 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }
    peaky[peaky.length >> 1] = 1;
    const params = { targetLufs: -6, ceilingDbtp: -3, truePeakOversample: 4 };
    const mono = masteringApplyProcessor(peaky, SR, {
      processor: 'maximizer.loudnessOptimize',
      params: params as never,
    });
    const stereo = masteringApplyProcessorStereo({
      processor: 'maximizer.loudnessOptimize',
      left: peaky,
      right: peaky,
      sampleRate: SR,
      params: params as never,
    });
    expect(mono.loudnessTargetLimited).toBe(true);
    expect(stereo.loudnessTargetLimited).toBe(mono.loudnessTargetLimited);
    expect(stereo.outputLufs).toBeLessThan(params.targetLufs);
  });

  it('refuses an explicit truePeakOversample of 0 and keeps the default when it is omitted', () => {
    const input = Float32Array.from(
      { length: 4096 },
      (_, i) => 0.3 * Math.sin((2 * Math.PI * 440 * i) / SR),
    );
    const implicit = masteringNormalizeLoudness(input, SR, { targetLufs: -18, ceilingDbtp: -1 });
    expect(implicit.samples.length).toBe(input.length);
    expect(() =>
      masteringNormalizeLoudness(input, SR, {
        targetLufs: -18,
        ceilingDbtp: -1,
        truePeakOversample: 0,
      }),
    ).toThrow(RangeError);
  });

  it('named mastering processors are available', () => {
    const quiet = new Float32Array(SR / 2);
    for (let i = 0; i < quiet.length; i++) {
      quiet[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }

    const names = masteringProcessorNames();
    expect(names).toContain('dynamics.compressor');
    expect(names).toContain('eq.equalizer');
    expect(names).toContain('saturation.ampSim');
    expect(names).toContain('stereo.imager');

    const result = masteringApplyProcessor(quiet, SR, {
      processor: 'dynamics.compressor',
      params: { thresholdDb: -24, ratio: 1.5 },
    });
    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(quiet.length);

    const stereo = masteringApplyProcessorStereo({
      processor: 'stereo.imager',
      left: quiet,
      right: quiet,
      sampleRate: SR,
      params: { width: 1.1 },
    });
    expect(stereo.left).toBeInstanceOf(Float32Array);
    expect(stereo.right.length).toBe(quiet.length);

    const eq = masteringApplyProcessor(quiet, SR, {
      processor: 'eq.equalizer',
      params: {
        'band0.enabled': true,
        'band0.frequencyHz': 440,
        'band0.gainDb': 6,
        'band0.q': 1,
        autoGain: true,
      },
    });
    expect(eq.samples).toBeInstanceOf(Float32Array);
    expect(eq.samples.length).toBe(quiet.length);
    expect(Number.isFinite(eq.outputLufs)).toBe(true);

    const amp = masteringApplyProcessor(quiet, SR, {
      processor: 'saturation.ampSim',
      params: {
        drive: 0.8,
        bassDb: 2,
        midDb: -3,
        trebleDb: 1.5,
        presenceDb: 3,
        cab: true,
        levelDb: -6,
      },
    });
    expect(amp.samples).toBeInstanceOf(Float32Array);
    expect(amp.samples.length).toBe(quiet.length);
    expect(Number.isFinite(amp.outputLufs)).toBe(true);

    const leftEq = masteringApplyProcessorStereo({
      processor: 'eq.equalizer',
      left: quiet,
      right: quiet,
      sampleRate: SR,
      params: {
        'band0.enabled': true,
        'band0.frequencyHz': 440,
        'band0.gainDb': 12,
        'band0.q': 1,
        'band0.placement': 'left',
      },
    });
    const leftPeak = Math.max(...leftEq.left.map((sample) => Math.abs(sample)));
    const rightPeak = Math.max(...leftEq.right.map((sample) => Math.abs(sample)));
    expect(leftPeak).toBeGreaterThan(rightPeak * 1.5);

    const linearEq = masteringApplyProcessorStereo({
      processor: 'eq.equalizer',
      left: quiet,
      right: quiet,
      sampleRate: SR,
      params: {
        phaseMode: 'linearPhase',
        'band0.enabled': true,
        'band0.frequencyHz': 440,
        'band0.gainDb': 3,
        'band0.q': 1,
      },
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
    const result = trim(samples, SR, { thresholdDb: -40.0 });
    expect(result.length).toBeLessThan(samples.length);
    expect(result.length).toBeGreaterThan(0);
  });
});

describe('features', () => {
  const tone = generateSine(440, SR, 1.0);

  it('accepts request objects for transform and feature extraction APIs', () => {
    const positionalStft = stft(tone, SR, { nFft: 512, hopLength: 128 });
    const requestedStft = stft({ samples: tone, sampleRate: SR, nFft: 512, hopLength: 128 });
    expect(requestedStft.magnitude).toEqual(positionalStft.magnitude);

    const melOptions = {
      nFft: 512,
      hopLength: 128,
      nMels: 24,
      fminHz: 80,
      fmaxHz: 6000,
      htk: true,
    };
    const positionalMel = melSpectrogram(tone, SR, melOptions);
    const requestedMel = melSpectrogram({ samples: tone, sampleRate: SR, ...melOptions });
    expect(requestedMel.power).toEqual(positionalMel.power);

    const mfccOptions = { ...melOptions, nMfcc: 10, lifter: 4 };
    const positionalMfcc = mfcc(tone, SR, mfccOptions);
    const requestedMfcc = mfcc({ samples: tone, sampleRate: SR, ...mfccOptions });
    expect(requestedMfcc.coefficients).toEqual(positionalMfcc.coefficients);

    const cqtOptions = { hopLength: 128, fminHz: 130.8128, nBins: 12, binsPerOctave: 12 };
    const positionalCqt = cqt(tone, SR, cqtOptions);
    const requestedCqt = cqt({ samples: tone, sampleRate: SR, ...cqtOptions });
    expect(requestedCqt.magnitude).toEqual(positionalCqt.magnitude);

    expect(spectralCentroid({ samples: tone, sampleRate: SR, nFft: 512, hopLength: 128 })).toEqual(
      spectralCentroid(tone, SR, { nFft: 512, hopLength: 128 }),
    );
    const yinOptions = {
      frameLength: 512,
      hopLength: 128,
      f0MinHz: 80,
      f0MaxHz: 1000,
      threshold: 0.2,
    };
    expect(pitchYin({ samples: tone, sampleRate: SR, ...yinOptions }).f0).toEqual(
      pitchYin(tone, SR, yinOptions).f0,
    );
    expect(framesToTime(8, SR, 128)).toBeCloseTo((8 * 128) / SR, 6);
    const db = powerToDb({ values: new Float32Array([0.25]), ref: 0.5, amin: 1e-8, topDb: 60 });
    expect(db[0]).toBeCloseTo(10 * Math.log10(0.25 / 0.5), 4);
    expect(
      trimSilence({ samples: tone, sampleRate: SR, topDb: 40, frameLength: 512, hopLength: 128 })
        .audio,
    ).toEqual(trimSilence(tone, SR, { topDb: 40, frameLength: 512, hopLength: 128 }).audio);
  });

  it('stft returns magnitude and power', () => {
    const result = stft(tone, SR, { nFft: 2048, hopLength: 512 });
    expect(result.magnitude.rows).toBe(1025);
    expect(result.magnitude.cols).toBeGreaterThan(0);
    expect(result.magnitude.data).toBeInstanceOf(Float32Array);
    expect(result.power.data).toBeInstanceOf(Float32Array);
    expect(result.magnitude.data.length).toBe(result.magnitude.rows * result.magnitude.cols);
  });

  it('stftDb returns dB values', () => {
    const result = stftDb(tone, SR, { nFft: 2048, hopLength: 512 });
    expect(result.rows).toBe(1025);
    expect(result.data).toBeInstanceOf(Float32Array);
    for (let i = 0; i < result.data.length; i++) {
      expect(Number.isFinite(result.data[i])).toBe(true);
    }
    expect(stftDb({ samples: tone, sampleRate: SR, nFft: 2048, hopLength: 512 })).toEqual(result);
  });

  it('melSpectrogram returns power and dB', () => {
    const result = melSpectrogram(tone, SR, { nFft: 2048, hopLength: 512, nMels: 128 });
    expect(result.power.rows).toBe(128);
    expect(result.power.cols).toBeGreaterThan(0);
    expect(result.power.data).toBeInstanceOf(Float32Array);
    expect(result.db.data).toBeInstanceOf(Float32Array);
  });

  it('melSpectrogram honours an explicit Mel range (fminHz/fmaxHz/htk)', () => {
    const framing = { nFft: 2048, hopLength: 512, nMels: 40 };
    const base = melSpectrogram(tone, SR, framing);
    const ranged = melSpectrogram(tone, SR, { ...framing, fminHz: 500, fmaxHz: 4000 });
    const htk = melSpectrogram(tone, SR, { ...framing, htk: true });
    expect(ranged.power.data.length).toBe(base.power.data.length);
    let dRange = 0;
    let dHtk = 0;
    for (let i = 0; i < base.power.data.length; i++) {
      dRange += (base.power.data[i] - ranged.power.data[i]) ** 2;
      dHtk += (base.power.data[i] - htk.power.data[i]) ** 2;
    }
    expect(dRange).toBeGreaterThan(1e-6);
    expect(dHtk).toBeGreaterThan(1e-6);
  });

  it('melToStft honours HTK mel spacing', () => {
    const nFft = 1024;
    const hop = 256;
    const nMels = 40;
    const mel = melSpectrogram(tone, SR, { nFft, hopLength: hop, nMels, htk: true });
    const slaney = melToStft({ power: mel.power, sampleRate: SR, nFft, htk: false });
    const htk = melToStft({ power: mel.power, sampleRate: SR, nFft, htk: true });

    expect(htk.rows).toBe(nFft / 2 + 1);
    expect(htk.cols).toBe(mel.power.cols);
    let diff = 0;
    for (let i = 0; i < htk.data.length; i++) {
      expect(Number.isFinite(htk.data[i])).toBe(true);
      diff += (slaney.data[i] - htk.data[i]) ** 2;
    }
    expect(diff).toBeGreaterThan(1e-6);
  });

  it('inverse feature wrappers reject non-finite matrices and a validate key', () => {
    // The facade scan names the function and the offending index.
    const melData = new Float32Array(8 * 4).fill(0.1);
    melData[3] = Number.NaN;
    const mel = matrix(melData, 8, 4);
    expect(() => melToStft({ power: mel, sampleRate: SR, nFft: 256 })).toThrow(
      /melToStft: power\.data contains a non-finite value at index 3/,
    );
    expect(() =>
      melToAudio({ power: mel, sampleRate: SR, nFft: 256, hopLength: 64, nIter: 2 }),
    ).toThrow(/melToAudio: power\.data contains a non-finite value at index 3/);
    expect(() =>
      melToStft({ power: mel, sampleRate: SR, nFft: 256, validate: false } as never),
    ).toThrow("melToStft: unknown request key 'validate'");

    const coeffData = new Float32Array(5 * 4).fill(0.1);
    coeffData[7] = Number.POSITIVE_INFINITY;
    const mfccCoeffs = matrix(coeffData, 5, 4);
    expect(() => mfccToMel({ coefficients: mfccCoeffs, nMels: 8 })).toThrow(
      /mfccToMel: coefficients\.data contains a non-finite value at index 7/,
    );
    expect(() =>
      mfccToAudio({
        coefficients: mfccCoeffs,
        nMels: 8,
        sampleRate: SR,
        nFft: 256,
        hopLength: 64,
        nIter: 2,
      }),
    ).toThrow(/mfccToAudio: coefficients\.data contains a non-finite value at index 7/);
    expect(() =>
      mfccToMel({ coefficients: mfccCoeffs, nMels: 8, validate: false } as never),
    ).toThrow("mfccToMel: unknown request key 'validate'");

    const magnitude = new Float32Array(3 * 5).fill(1);
    magnitude[4] = Number.NaN;
    expect(() =>
      griffinLim({
        magnitude: matrix(magnitude, 3, 5),
        sampleRate: SR,
        nFft: 4,
        hopLength: 1,
        nIter: 1,
        momentum: 0,
      }),
    ).toThrow(/griffinLim: magnitude\.data contains a non-finite value at index 4/);
  });

  it('passes the forward lifter back to MFCC inversion', () => {
    const lifted = new Float32Array(5 * 4);
    for (let coefficient = 0; coefficient < 5; coefficient++) {
      const lift = 1 + 11 * Math.sin((Math.PI * coefficient) / 22);
      lifted.fill((0.2 + 0.1 * coefficient) * lift, coefficient * 4, coefficient * 4 + 4);
    }
    const coefficients = matrix(lifted, 5, 4);
    const restored = mfccToMel({ coefficients, nMels: 8, lifter: 22 });
    const untreated = mfccToMel({ coefficients, nMels: 8 });
    expect(
      restored.data.some((value, index) => Math.abs(value - untreated.data[index]) > 1e-5),
    ).toBe(true);
  });

  it('reconstructs CQT and VQT magnitudes with shape validation', () => {
    // Shared deterministic oracle fixture with Python/WASM.
    const source = generateSine(261.6256, 8000, 0.256, 0.5);
    const cq = cqt(source, 8000, {
      hopLength: 128,
      fminHz: 130.8128,
      nBins: 12,
      binsPerOctave: 12,
    });
    const cqtAudio = cqtToAudio({
      magnitude: cq.magnitude,
      sampleRate: 8000,
      hopLength: 128,
      fminHz: 130.8128,
      binsPerOctave: 12,
      nIter: 2,
    });
    expect(cqtAudio.length).toBe(source.length);
    expect(Array.from(cqtAudio).every(Number.isFinite)).toBe(true);
    expect(Math.max(...cqtAudio.map(Math.abs))).toBeGreaterThan(1e-5);

    const vq = vqt(source, 8000, {
      hopLength: 128,
      fminHz: 130.8128,
      nBins: 12,
      binsPerOctave: 12,
      gamma: 0,
    });
    const vqtAudio = vqtToAudio({
      magnitude: vq.magnitude,
      sampleRate: 8000,
      hopLength: 128,
      fminHz: 130.8128,
      binsPerOctave: 12,
      gamma: 0,
      nIter: 2,
    });
    expect(vqtAudio.length).toBe(source.length);
    expect(Array.from(vqtAudio).every(Number.isFinite)).toBe(true);
    const truncated = { ...cq.magnitude, data: cq.magnitude.data.subarray(1) };
    expect(() => cqtToAudio({ magnitude: truncated, sampleRate: 8000 })).toThrow();
    expect(() =>
      vqtToAudio({
        magnitude: vq.magnitude,
        sampleRate: 8000,
        hopLength: 128,
        fminHz: 130.8128,
        binsPerOctave: 12,
        gamma: 0,
        nIter: 257,
      }),
    ).toThrow();
  });

  it('mfcc returns coefficients', () => {
    const result = mfcc(tone, SR, { nFft: 2048, hopLength: 512, nMels: 64, nMfcc: 20 });
    const { rows, cols, data } = result.coefficients;
    expect(rows).toBe(20);
    expect(cols).toBeGreaterThan(0);
    expect(data).toBeInstanceOf(Float32Array);
    expect(data.length).toBe(rows * cols);
  });

  it('mfcc honours an explicit Mel range (fminHz/fmaxHz)', () => {
    const framing = { nFft: 2048, hopLength: 512, nMels: 64, nMfcc: 13 };
    const base = mfcc(tone, SR, framing);
    const ranged = mfcc(tone, SR, { ...framing, fminHz: 500, fmaxHz: 4000 });
    expect(ranged.coefficients.data.length).toBe(base.coefficients.data.length);
    let d = 0;
    for (let i = 0; i < base.coefficients.data.length; i++) {
      d += (base.coefficients.data[i] - ranged.coefficients.data[i]) ** 2;
    }
    expect(d).toBeGreaterThan(1e-6);
  });

  it('mfcc defaults nMfcc to 20', () => {
    const result = mfcc(tone, SR, { nFft: 2048, hopLength: 512, nMels: 64 });
    expect(result.coefficients.rows).toBe(20);
  });

  it('chroma returns 12 pitch classes', () => {
    const result = chroma(tone, SR, { nFft: 2048, hopLength: 512 });
    expect(result.features.rows).toBe(12);
    expect(result.features.cols).toBeGreaterThan(0);
    expect(result.features.data).toBeInstanceOf(Float32Array);
    expect(result.meanEnergy.length).toBe(12);
  });

  it('chromaCens and bassChroma return 12 pitch classes', () => {
    for (const result of [chromaCens(tone, SR), bassChroma(tone, SR)]) {
      expect(result.features.rows).toBe(12);
      expect(result.features.cols).toBeGreaterThan(0);
      expect(result.features.data).toBeInstanceOf(Float32Array);
      expect(result.features.data.length).toBe(result.features.rows * result.features.cols);
      expect(result.meanEnergy.length).toBe(12);
    }
  });

  it('chromaCens and chromaCqt forward binsPerOctave to the transform', () => {
    // The constant-Q chroma variants keep this option because the entry point
    // honours it. Falling back to the non-extended core call would leave the
    // option accepted and ignored, which is what this catches.
    for (const variant of [chromaCens, chromaCqt]) {
      const base = variant({ samples: tone, sampleRate: SR });
      const coarser = variant({ samples: tone, sampleRate: SR, binsPerOctave: 12 });
      expect(coarser.features.data.length).toBe(base.features.data.length);
      expect(Array.from(coarser.features.data)).not.toEqual(Array.from(base.features.data));
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
    expect(spectralBandwidth(tone, SR, { nFft: 2048, hopLength: 512, p: 1 })).not.toEqual(result);
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

  it('spectralFlux returns Float32Array and accepts a lag', () => {
    const result = spectralFlux(tone, SR);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
    expect(spectralFlux(tone, SR, { nFft: 2048, hopLength: 512, lag: 2 })).not.toEqual(result);
    expect(
      spectralFlux({ samples: tone, sampleRate: SR, nFft: 2048, hopLength: 512, lag: 1 }),
    ).toEqual(result);
  });

  it('masteringPresetNames exposes the native preset catalog', () => {
    const presets = masteringPresetNames();
    expect(presets).toContain('pop');
    expect(presets).toContain('aiMusic');
  });

  it('onsetBacktrack returns preceding energy minima', () => {
    expect(
      Array.from(
        onsetBacktrack({
          events: [6],
          energy: new Float32Array([1, 0.8, 0.5, 0.2, 0.6, 0.9, 1.2]),
        }),
      ),
    ).toEqual([3]);
  });

  it('griffinLim reconstructs STFT magnitude', () => {
    const spectrum = stft(tone, SR, { nFft: 1024, hopLength: 256 });
    const result = griffinLim({
      magnitude: spectrum.magnitude,
      sampleRate: SR,
      nFft: 1024,
      hopLength: 256,
      nIter: 1,
      momentum: 0,
    });
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
    expect(result.f0.length).toBeGreaterThan(0);
    expect(result).not.toHaveProperty('nFrames');
    expect(result.medianF0).toBeGreaterThan(400);
    expect(result.medianF0).toBeLessThan(480);
    expect(result.f0).toBeInstanceOf(Float32Array);
    expect(result.voicedProb).toBeInstanceOf(Float32Array);
  });

  it('pitchPyin detects 440 Hz', () => {
    const result = pitchPyin(tone, SR);
    expect(result.f0.length).toBeGreaterThan(0);
    expect(result).not.toHaveProperty('nFrames');
    expect(result.medianF0).toBeGreaterThan(400);
    expect(result.medianF0).toBeLessThan(480);
  });

  it('noteSegments preserves vibrato and separates unvoiced intervals', () => {
    const segments = noteSegments({
      f0Hz: new Float32Array([440, 445, 435, 444, 436, 0, 0, 660, 666, 654, 665, 655, 0]),
      voicedProb: new Float32Array([1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0]),
      frameRate: 100,
    });
    expect(segments).toHaveLength(2);
    expect(segments[0]).toMatchObject({
      frameStart: 0,
      frameEnd: 5,
      startSec: 0,
    });
    expect(segments[1]).toMatchObject({
      frameStart: 7,
      frameEnd: 12,
    });
    expect(segments[0].endSec).toBeCloseTo(0.05);
    expect(segments[1].startSec).toBeCloseTo(0.07);
    expect(segments[1].endSec).toBeCloseTo(0.12);
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

  it.each(['H4', 'C#x', '', 'A4junk', 'C2147483647', 'C10', 'A-2'])(
    'noteToHz(%j) is refused with a RangeError',
    (name) => {
      expect(() => noteToHz(name)).toThrow(RangeError);
      expect(() => noteToHz(name)).toThrow(/^noteToHz: (cannot parse note name|octave of) /);
    },
  );

  it('noteToHz spans the MIDI note range octaves C-1 through G9', () => {
    expect(noteToHz('C-1')).toBeCloseTo(8.1758, 3);
    expect(noteToHz('G9')).toBeCloseTo(12543.85, 1);
  });

  it.each([0, -1, -440, Number.NaN, Number.POSITIVE_INFINITY])(
    'hzToNote(%d) is refused with a RangeError',
    (hz) => {
      expect(() => hzToNote(hz)).toThrow(RangeError);
      expect(() => hzToNote(hz)).toThrow(/^hzToNote: hz must be finite and positive/);
    },
  );

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
