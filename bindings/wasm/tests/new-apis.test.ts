/**
 * Tests for the v1.2 feature additions exposed in WASM:
 * onset envelope, Fourier tempogram, tempogram ratio, NNLS chroma,
 * and EBU R128 LUFS metering.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  fourierTempogram,
  init,
  lufs,
  momentaryLufs,
  nnlsChroma,
  onsetEnvelope,
  plp,
  shortTermLufs,
  tempogram,
  tempogramRatio,
} from '../dist/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number, amp = 0.5): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function allFinite(arr: Float32Array | number[]): boolean {
  for (const x of arr) {
    if (!Number.isFinite(x)) {
      return false;
    }
  }
  return true;
}

describe('v1.2 feature additions (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const signal = generateSine(220, SR, 3.0);

  describe('onset envelope', () => {
    it('returns a finite envelope', () => {
      const env = onsetEnvelope(signal, SR);
      expect(env.length).toBeGreaterThan(0);
      expect(allFinite(env)).toBe(true);
    });
  });

  describe('tempogram family', () => {
    it('Fourier tempogram returns an [nBins x nFrames] matrix', () => {
      const env = onsetEnvelope(signal, SR);
      const ft = fourierTempogram(env, SR);
      expect(ft.nBins).toBeGreaterThan(0);
      expect(ft.nFrames).toBeGreaterThan(0);
      expect(ft.data.length).toBe(ft.nBins * ft.nFrames);
      expect(allFinite(ft.data)).toBe(true);
    });

    it('tempogram ratio returns one value per default factor', () => {
      const env = onsetEnvelope(signal, SR);
      const tg = tempogram(env, SR);
      const ratios = tempogramRatio(tg.data, tg.winLength, SR);
      expect(ratios.length).toBe(5);
      expect(allFinite(ratios)).toBe(true);
    });

    it('plp returns a pulse curve aligned to the envelope', () => {
      const env = onsetEnvelope(signal, SR);
      const pulse = plp(env, SR);
      expect(pulse.length).toBe(env.length);
      expect(allFinite(pulse)).toBe(true);
    });
  });

  describe('NNLS chroma', () => {
    it('returns a 12 x nFrames matrix', () => {
      const result = nnlsChroma(signal, SR);
      expect(result.nChroma).toBe(12);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.data.length).toBe(result.nChroma * result.nFrames);
      expect(allFinite(result.data)).toBe(true);
    });
  });

  describe('LUFS metering', () => {
    it('returns finite integrated/momentary/short-term/range values', () => {
      const result = lufs(signal, SR);
      expect(Number.isFinite(result.integratedLufs)).toBe(true);
      expect(Number.isFinite(result.momentaryLufs)).toBe(true);
      expect(Number.isFinite(result.shortTermLufs)).toBe(true);
      expect(Number.isFinite(result.loudnessRange)).toBe(true);
    });

    it('reports a louder signal as higher integrated LUFS', () => {
      const quiet = lufs(generateSine(220, SR, 3.0, 0.1), SR);
      const loud = lufs(generateSine(220, SR, 3.0, 0.8), SR);
      expect(loud.integratedLufs).toBeGreaterThan(quiet.integratedLufs);
    });

    it('momentary/short-term series are non-empty and finite', () => {
      expect(momentaryLufs(signal, SR).length).toBeGreaterThan(0);
      expect(shortTermLufs(signal, SR).length).toBeGreaterThan(0);
      expect(allFinite(momentaryLufs(signal, SR))).toBe(true);
    });
  });

  describe('Audio class methods', () => {
    it('exposes onsetEnvelope/nnlsChroma/lufs/momentaryLufs/shortTermLufs', () => {
      const audio = Audio.fromBuffer(signal, SR);
      expect(audio.onsetEnvelope().length).toBeGreaterThan(0);
      expect(audio.nnlsChroma().nChroma).toBe(12);
      expect(Number.isFinite(audio.lufs().integratedLufs)).toBe(true);
      expect(audio.momentaryLufs().length).toBeGreaterThan(0);
      expect(audio.shortTermLufs().length).toBeGreaterThan(0);
    });
  });
});
