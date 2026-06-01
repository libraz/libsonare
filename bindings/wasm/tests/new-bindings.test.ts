/**
 * Smoke tests for the 12 functions newly exposed to the WASM binding
 * (spectralContrast / polyFeatures / zeroCrossings / pitchTuning /
 * estimateTuning / decompose / nnFilter / remix / phaseVocoder /
 * hpssWithResidual / lufsInterleaved / ebur128LoudnessRange). These guard the
 * TS facade wiring; the DSP itself is validated at the C ABI / C++ layer.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  decompose,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  init,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  polyFeatures,
  remix,
  spectralContrast,
  zeroCrossings,
} from '../src/index';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
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

describe('newly exposed WASM functions', () => {
  beforeAll(async () => {
    await init();
  });

  it('spectralContrast returns an (nBands+1) x nFrames matrix', () => {
    const r = spectralContrast(makeSine(1, 440), SR, 2048, 512, 6);
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);
    expect(r.data.length).toBe(r.rows * r.cols);
    expect(allFinite(r.data)).toBe(true);
  });

  it('polyFeatures returns an (order+1) x nFrames matrix', () => {
    const r = polyFeatures(makeSine(1, 440), SR, 2048, 512, 1);
    expect(r.rows).toBe(2);
    expect(r.cols).toBeGreaterThan(0);
    expect(allFinite(r.data)).toBe(true);
  });

  it('zeroCrossings returns sorted in-range indices', () => {
    const x = makeSine(0.05, 440);
    const idx = zeroCrossings(x);
    expect(idx.length).toBeGreaterThan(0);
    for (let i = 1; i < idx.length; i++) {
      expect(idx[i]).toBeGreaterThan(idx[i - 1]);
      expect(idx[i]).toBeLessThan(x.length);
    }
  });

  it('pitchTuning returns a finite deviation', () => {
    const t = pitchTuning(new Float32Array([440, 880, 660]));
    expect(Number.isFinite(t)).toBe(true);
    expect(t).toBeGreaterThanOrEqual(-0.5);
    expect(t).toBeLessThan(0.5);
  });

  it('estimateTuning returns a finite deviation', () => {
    const t = estimateTuning(makeSine(1, 440), SR);
    expect(Number.isFinite(t)).toBe(true);
  });

  it('decompose factorizes a non-negative spectrogram', () => {
    const nFeatures = 16;
    const nFrames = 24;
    const nComponents = 3;
    const s = new Float32Array(nFeatures * nFrames);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose(s, nFeatures, nFrames, nComponents, 20, 2.0);
    expect(r.w.length).toBe(nFeatures * nComponents);
    expect(r.h.length).toBe(nComponents * nFrames);
    expect(allFinite(r.w)).toBe(true);
    expect(allFinite(r.h)).toBe(true);
  });

  it('nnFilter preserves the spectrogram shape', () => {
    const nFeatures = 12;
    const nFrames = 20;
    const s = new Float32Array(nFeatures * nFrames);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.cos(i));
    }
    const r = nnFilter(s, nFeatures, nFrames);
    expect(r.rows).toBe(nFeatures);
    expect(r.cols).toBe(nFrames);
    expect(allFinite(r.data)).toBe(true);
  });

  it('remix concatenates interval slices', () => {
    const x = makeSine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix(x, new Int32Array([half, x.length, 0, half]));
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = makeSine(0.5, 440);
    const out = phaseVocoder(x, 2.0);
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('hpssWithResidual splits into three signals', () => {
    const r = hpssWithResidual(makeSine(1, 440));
    expect(r.harmonic.length).toBe(r.percussive.length);
    expect(r.percussive.length).toBe(r.residual.length);
    expect(r.harmonic.length).toBeGreaterThan(0);
    expect(allFinite(r.harmonic)).toBe(true);
  });

  it('lufsInterleaved measures dual-mono loudness', () => {
    const x = makeSine(1, 440);
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
    const lra = ebur128LoudnessRange(makeSine(1, 440));
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
});
