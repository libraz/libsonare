import { describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  decompose,
  decomposeStems,
  detectAcoustic,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchShift,
  pitchTuning,
  pitchYin,
  polyFeatures,
  realtimeVoiceChangerPresetConfig,
  remix,
  remixAlignedIntervals,
  scaleQuantizeMidi,
  spectralContrast,
  timeStretch,
  voiceCharacterPresetId,
  zeroCrossings,
} from '../src/index';

const SR = 22050;

function sine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / SR);
  }
  return out;
}

function allFinite(arr: ArrayLike<number>): boolean {
  for (let i = 0; i < arr.length; i++) {
    if (!Number.isFinite(arr[i])) {
      return false;
    }
  }
  return arr.length > 0;
}

describe('newly exposed Node functions', () => {
  it('spectralContrast returns (nBands+1) x nFrames', () => {
    const r = spectralContrast(sine(1, 440), SR, 2048, 512, 6);
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);
    expect(r.data.length).toBe(r.rows * r.cols);
    expect(allFinite(r.data)).toBe(true);
  });

  it('polyFeatures returns (order+1) x nFrames', () => {
    const r = polyFeatures(sine(1, 440), SR, 2048, 512, 1);
    expect(r.rows).toBe(2);
    expect(allFinite(r.data)).toBe(true);
  });

  it('zeroCrossings returns sorted in-range indices', () => {
    const x = sine(0.05, 440);
    const idx = zeroCrossings(x);
    expect(idx.length).toBeGreaterThan(0);
    for (let i = 1; i < idx.length; i++) {
      expect(idx[i]).toBeGreaterThan(idx[i - 1]);
      expect(idx[i]).toBeLessThan(x.length);
    }
  });

  it('pitchTuning is finite', () => {
    const t = pitchTuning(new Float32Array([440, 880, 660]));
    expect(Number.isFinite(t)).toBe(true);
    expect(t).toBeGreaterThanOrEqual(-0.5);
    expect(t).toBeLessThan(0.5);
  });

  it('estimateTuning is finite', () => {
    expect(Number.isFinite(estimateTuning(sine(1, 440), SR))).toBe(true);
  });

  it('decompose factorizes a non-negative spectrogram', () => {
    const nF = 16;
    const nT = 24;
    const nC = 3;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose(s, nF, nT, nC, 20, 2.0);
    expect(r.w.data.length).toBe(nF * nC);
    expect(r.h.data.length).toBe(nC * nT);
    expect(allFinite(r.w.data) && allFinite(r.h.data)).toBe(true);
  });

  it('decompose accepts an nndsvd warm-start initialiser', () => {
    const nF = 16;
    const nT = 24;
    const nC = 3;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose(s, nF, nT, nC, 20, 2.0, 'nndsvd');
    expect(r.w.data.length).toBe(nF * nC);
    expect(r.h.data.length).toBe(nC * nT);
    expect(allFinite(r.w.data) && allFinite(r.h.data)).toBe(true);
  });

  it('nnFilter preserves shape', () => {
    const nF = 12;
    const nT = 20;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.cos(i));
    }
    const r = nnFilter(s, nF, nT);
    expect(r.rows).toBe(nF);
    expect(r.cols).toBe(nT);
    expect(allFinite(r.data)).toBe(true);
  });

  it('decompose/nnFilter reject dims that exceed the input length (no OOB read)', () => {
    const small = new Float32Array(10);
    expect(() => decompose(small, 1000, 1000, 3, 20, 2.0)).toThrow();
    expect(() => nnFilter(small, 1000, 1000)).toThrow();
  });

  it('timeStretch/pitchShift reject a missing required argument instead of producing NaN', () => {
    const x = sine(0.1, 220);
    // sampleRate then the transform factor; both are required, so a bare call must throw.
    expect(() => (timeStretch as (s: Float32Array) => Float32Array)(x)).toThrow();
    expect(() => (pitchShift as (s: Float32Array) => Float32Array)(x)).toThrow();
    // Valid calls still work.
    expect(allFinite(timeStretch(x, SR, 1.25))).toBe(true);
    expect(allFinite(pitchShift(x, SR, 2))).toBe(true);
  });

  it('timeStretch rejects non-finite and oversized projected rates, then recovers', () => {
    const x = sine(0.1, 220);
    expect(() => timeStretch(x, SR, Number.NaN)).toThrow();
    expect(() => timeStretch(x, SR, Number.POSITIVE_INFINITY)).toThrow();
    expect(() => timeStretch(x, SR, 1.1754943508222875e-38)).toThrow();
    expect(allFinite(timeStretch(x, SR, 1))).toBe(true);
  });

  it('realtimeVoiceChangerPresetConfig exposes the ISP true-peak limiter fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('neutral-monitor') as Record<string, unknown>;
    expect(typeof cfg.limiterEnableIspLimiter).toBe('boolean');
    expect(typeof cfg.limiterIspCeilingDbtp).toBe('number');
    expect(Number.isFinite(cfg.limiterIspCeilingDbtp as number)).toBe(true);
  });

  it('remix concatenates interval slices', () => {
    const x = sine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix(x, new Int32Array([half, x.length, 0, half]));
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('remixAlignedIntervals resolves one clamped pair per interval', () => {
    const x = sine(0.5, 440);
    const pairs = remixAlignedIntervals({
      samples: x,
      intervals: new Int32Array([0, 1000, 5000, 5500]),
    });
    expect(pairs.length).toBe(4);
    expect(pairs[0]).toBeGreaterThanOrEqual(0);
    expect(pairs[1]).toBeGreaterThan(pairs[0]);
    expect(pairs[3]).toBeGreaterThan(pairs[2]);
    expect(pairs[3]).toBeLessThanOrEqual(x.length);
  });

  it('remixAlignedIntervals leaves a signal with no sign change unsnapped', () => {
    const flat = new Float32Array(4096).fill(0.25);
    const pairs = remixAlignedIntervals({ samples: flat, intervals: new Int32Array([100, 200]) });
    expect(Array.from(pairs)).toEqual([100, 200]);
  });

  it('decomposeStems components carry phase and sum back to the input', () => {
    const n = 8192;
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      x[i] = 0.5 * Math.sin((2 * Math.PI * (i < n / 2 ? 220 : 880) * i) / SR);
    }
    const r = decomposeStems({
      samples: x,
      sampleRate: SR,
      nComponents: 2,
      nFft: 1024,
      hopLength: 256,
      nIter: 30,
    });
    expect(r.components.length).toBe(2);
    expect(r.components[0].length).toBe(n);
    expect(r.w.length).toBe((1024 / 2 + 1) * 2);
    expect(r.h.length % 2).toBe(0);
    expect(r.sampleRate).toBe(SR);
    let err = 0;
    let ref = 0;
    for (let i = 1024; i < n - 1024; i++) {
      const sum = r.components[0][i] + r.components[1][i];
      err += (sum - x[i]) ** 2;
      ref += x[i] ** 2;
    }
    expect(Math.sqrt(err / ref)).toBeLessThan(0.05);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = sine(0.5, 440);
    const out = phaseVocoder(x, SR, 2.0);
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('hpssWithResidual splits into three signals', () => {
    const r = hpssWithResidual(sine(1, 440));
    expect(r.harmonic.length).toBe(r.percussive.length);
    expect(r.percussive.length).toBe(r.residual.length);
    expect(allFinite(r.harmonic)).toBe(true);
  });

  it('lufsInterleaved measures dual-mono loudness', () => {
    const x = sine(1, 440);
    const interleaved = new Float32Array(x.length * 2);
    for (let i = 0; i < x.length; i++) {
      interleaved[2 * i] = x[i];
      interleaved[2 * i + 1] = x[i];
    }
    const r = lufsInterleaved(interleaved, 2, SR);
    expect(Number.isFinite(r.integratedLufs)).toBe(true);
    expect(r.integratedLufs).toBeLessThan(0);
  });

  it('ebur128LoudnessRange is finite and non-negative', () => {
    const lra = ebur128LoudnessRange(sine(1, 440));
    expect(Number.isFinite(lra)).toBe(true);
    expect(lra).toBeGreaterThanOrEqual(0);
  });

  it('pitchYin returns librosa-style estimates for unvoiced frames', () => {
    const silence = new Float32Array(SR); // fully unvoiced
    const nanRes = pitchYin(silence, SR, 2048, 512, 65, 2093, 0.3, false);
    expect(nanRes.f0.every((v) => Number.isFinite(v))).toBe(true);
    expect(nanRes.voicedFlag.every((v) => !v)).toBe(true);

    const filled = pitchYin(silence, SR, 2048, 512, 65, 2093, 0.3, true);
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('pitchPyin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR);
    const nanRes = pitchPyin(silence, SR, 2048, 512, 65, 2093, 0.3, false);
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

    const filled = pitchPyin(silence, SR, 2048, 512, 65, 2093, 0.3, true);
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('voiceCharacterPresetId maps a known ordinal to its id', () => {
    expect(voiceCharacterPresetId(1)).toBe('bright-idol');
    expect(voiceCharacterPresetId('neutral-monitor')).toBe('neutral-monitor');
    // Out-of-range ordinal returns null.
    expect(voiceCharacterPresetId(99)).toBeNull();
  });

  it('realtimeVoiceChangerPresetConfig returns a config object with expected fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('bright-idol');
    for (const key of [
      'inputGainDb',
      'outputGainDb',
      'wetMix',
      'retuneSemitones',
      'retuneGrainSize',
      'formantFactor',
      'compressorThresholdDb',
      'reverbSeed',
      'limiterCeilingDb',
      'limiterReleaseMs',
    ]) {
      expect(typeof (cfg as Record<string, number>)[key]).toBe('number');
      expect(Number.isFinite((cfg as Record<string, number>)[key])).toBe(true);
    }
  });

  it('scaleQuantizeMidi rejects an out-of-range modeMask', () => {
    expect(() => scaleQuantizeMidi(0, -1, 69)).toThrow();
    expect(() => scaleQuantizeMidi(0, 5000, 69)).toThrow();
  });

  it('phaseVocoder rejects a non-number sampleRate', () => {
    const x = sine(0.1, 440);
    expect(() => phaseVocoder(x, 'bad' as unknown as number, 2.0)).toThrow();
  });

  it('analyzeTimbre exposes timbreOverTime', () => {
    const r = analyzeTimbre(sine(2, 440), SR);
    expect(r.timbreOverTime.length).toBeGreaterThan(0);
    for (const frame of r.timbreOverTime) {
      expect(Number.isFinite(frame.brightness)).toBe(true);
      expect(Number.isFinite(frame.warmth)).toBe(true);
      expect(Number.isFinite(frame.density)).toBe(true);
      expect(Number.isFinite(frame.roughness)).toBe(true);
      expect(Number.isFinite(frame.complexity)).toBe(true);
    }
  });

  it('analysis functions accept options objects', () => {
    const x = sine(2, 440);
    // maxCandidates is honoured through the options bag.
    expect(analyzeBpm(x, SR, { maxCandidates: 2 }).candidates.length).toBeLessThanOrEqual(2);
    // nMfcc widens the MFCC matrix exposed by the timbre result.
    const timbre = analyzeTimbre(x, SR, { nFft: 1024, hopLength: 256, nMfcc: 20 });
    expect(timbre.spectralCentroid).toBeInstanceOf(Float32Array);
    // Remaining options-based entry points run without throwing on defaults+overrides.
    expect(analyzeRhythm(x, SR, { bpmMin: 80, bpmMax: 160 }).bpm).toBeGreaterThan(0);
    expect(analyzeDynamics(x, SR, { windowSec: 0.2 }).loudnessTimes).toBeInstanceOf(Float32Array);
    expect(detectAcoustic(x, SR, { nOctaveBands: 4 }).rt60Bands).toBeInstanceOf(Float32Array);
  });
});
