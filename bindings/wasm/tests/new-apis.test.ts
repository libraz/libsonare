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
  Mixer,
  mixingScenePresetJson,
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

  describe('Mixer (scene-based routing)', () => {
    const BLOCK = 512;

    function blockEnergy(r: { left: Float32Array; right: Float32Array }): number {
      let sum = 0;
      for (let i = 0; i < r.left.length; i++) {
        sum += r.left[i] * r.left[i] + r.right[i] * r.right[i];
      }
      return sum;
    }

    it('routes a send through reverb and back to master', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      mixer.compile();

      // Strip 0 = vocal, strip 1 = reverb return. Impulse into vocal, silence into return.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);

      const energies: number[] = [];
      for (let block = 0; block < 16; block++) {
        const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
        energies.push(blockEnergy(out));
        vocalL[0] = 0.0;
        vocalR[0] = 0.0;
      }

      // Block 0 carries the dry hit; later blocks carry the reverb tail.
      expect(energies[0]).toBeGreaterThan(1e-6);
      const tail = energies.slice(4).reduce((a, b) => a + b, 0);
      expect(tail).toBeGreaterThan(1e-6);

      mixer.delete();
    });

    it('schedules insert-parameter automation without throwing', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      mixer.compile();

      expect(mixer.stripCount()).toBeGreaterThan(0);

      // Strip 0 (vocal) has pre-fader inserts (insert 0 = eq.parametric).
      // Schedule a linear ramp on param 0 over the first second of audio.
      expect(() => mixer.scheduleInsertAutomation(0, 0, 0, 0, 0.0, 'linear')).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, 0, 48000, 1.0, 'exponential'),
      ).not.toThrow();

      // Out-of-range strip index must throw.
      expect(() => mixer.scheduleInsertAutomation(999, 0, 0, 0, 0.0)).toThrow();

      // Processing after scheduling still produces output.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);
      const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      mixer.delete();
    });

    it('round-trips the scene topology to JSON', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      const scene = mixer.toSceneJson();
      expect(scene).toContain('"vocal-verb"');
      expect(scene).toContain('"vocal-verb-return"');
      expect(scene).toContain('"destinationBusId":"vocal-verb"');

      const restored = Mixer.fromSceneJson(scene, 48000, BLOCK);
      restored.compile();
      restored.delete();
      mixer.delete();
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
