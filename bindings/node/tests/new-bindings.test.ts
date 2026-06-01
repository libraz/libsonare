import { describe, expect, it } from 'vitest';
import {
  analyzeTimbre,
  decompose,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  polyFeatures,
  realtimeVoiceChangerPresetConfig,
  remix,
  scaleQuantizeMidi,
  spectralContrast,
  voiceCharacterPresetId,
  zeroCrossings,
} from '../src/index';

const SR = 22050;

function sine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / SR);
  return out;
}

function allFinite(arr: ArrayLike<number>): boolean {
  for (let i = 0; i < arr.length; i++) if (!Number.isFinite(arr[i])) return false;
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
    for (let i = 0; i < s.length; i++) s[i] = Math.abs(Math.sin(i));
    const r = decompose(s, nF, nT, nC, 20, 2.0);
    expect(r.w.data.length).toBe(nF * nC);
    expect(r.h.data.length).toBe(nC * nT);
    expect(allFinite(r.w.data) && allFinite(r.h.data)).toBe(true);
  });

  it('nnFilter preserves shape', () => {
    const nF = 12;
    const nT = 20;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) s[i] = Math.abs(Math.cos(i));
    const r = nnFilter(s, nF, nT);
    expect(r.rows).toBe(nF);
    expect(r.cols).toBe(nT);
    expect(allFinite(r.data)).toBe(true);
  });

  it('remix concatenates interval slices', () => {
    const x = sine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix(x, new Int32Array([half, x.length, 0, half]));
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = sine(0.5, 440);
    const out = phaseVocoder(x, 2.0);
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

  it('pitchYin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR); // fully unvoiced
    const nanRes = pitchYin(silence, SR, 2048, 512, 65, 2093, 0.3, false);
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

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
    expect(() => phaseVocoder(x, 2.0, 'bad' as unknown as number)).toThrow();
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
});
