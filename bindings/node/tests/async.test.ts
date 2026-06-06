/**
 * Regression tests for the Node binding's AsyncWorker-backed entry points
 * (analyzeAsync / masterAudioAsync / masterAudioStereoAsync). The async
 * versions move the DSP off the JS event loop; the contract is that they
 * return a Promise that resolves to a result shape identical to the sync
 * version on the same input.
 */

import { describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeAsync,
  masterAudio,
  masterAudioAsync,
  masterAudioStereoAsync,
} from '../src/index';

const SR = 22050;

function generateSine(freq: number, durationSec: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.25 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('Node async API', () => {
  describe('analyzeAsync', () => {
    it('returns a Promise that resolves to the analysis result', async () => {
      const samples = generateSine(440, 2);
      const result = await analyzeAsync(samples, SR);
      expect(result).toBeDefined();
      expect(Number.isFinite(result.bpm)).toBe(true);
      expect(typeof result.key).toBe('object');
    });

    it('resolves to the same shape as the synchronous version', async () => {
      const samples = generateSine(440, 1);
      const [syncResult, asyncResult] = [analyze(samples, SR), await analyzeAsync(samples, SR)];
      expect(Object.keys(asyncResult).sort()).toEqual(Object.keys(syncResult).sort());
      expect(typeof asyncResult.key.root).toBe('string');
      expect(typeof asyncResult.key.mode).toBe('string');
      expect(typeof asyncResult.key.name).toBe('string');
      expect(typeof asyncResult.key.shortName).toBe('string');
      expect(Object.keys(asyncResult.key).sort()).toEqual(Object.keys(syncResult.key).sort());
    });

    it('rejects invalid arguments instead of throwing synchronously', async () => {
      const promise = analyzeAsync(undefined as unknown as Float32Array, SR);
      await expect(promise).rejects.toThrow(/Expected \(Float32Array, sampleRate\?\)/);
    });

    it('does not block the JS event loop while running', async () => {
      const samples = generateSine(440, 5); // a few seconds of input
      let timerFired = false;
      const timer = setTimeout(() => {
        timerFired = true;
      }, 1);
      const result = await analyzeAsync(samples, SR);
      clearTimeout(timer);
      // The timer is set to 1 ms; if the worker thread were running on the JS
      // thread, the timer would not have fired until after the worker returned.
      expect(timerFired).toBe(true);
      expect(result.bpm).toBeGreaterThan(0);
    });
  });

  describe('masterAudioAsync', () => {
    it('mono variant resolves to a mastered MonoChainResult', async () => {
      const samples = generateSine(220, 1);
      const result = await masterAudioAsync(samples, SR, 'pop');
      expect(result.samples.length).toBeGreaterThan(0);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(Array.isArray(result.stages)).toBe(true);
      expect(result.stages.length).toBeGreaterThan(0);
    });

    it('stereo variant resolves to a mastered StereoChainResult', async () => {
      const left = generateSine(220, 1);
      const right = generateSine(330, 1);
      const result = await masterAudioStereoAsync(left, right, SR, 'pop');
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(Array.isArray(result.stages)).toBe(true);
    });

    it('shape matches the synchronous version on the same input', async () => {
      const samples = generateSine(220, 0.5);
      const [syncResult, asyncResult] = [
        masterAudio(samples, SR, 'pop'),
        await masterAudioAsync(samples, SR, 'pop'),
      ];
      expect(Object.keys(asyncResult).sort()).toEqual(Object.keys(syncResult).sort());
    });
  });
});
