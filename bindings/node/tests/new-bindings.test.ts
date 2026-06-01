import { describe, expect, it } from 'vitest';
import {
  decompose,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchTuning,
  polyFeatures,
  remix,
  spectralContrast,
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
});
