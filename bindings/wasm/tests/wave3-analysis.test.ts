/**
 * Wave 3 analysis parity tests for the WASM binding.
 *
 * Covers the gaps closed in this wave:
 *  - the unified `analyze()` result now exposes `dynamics.peakDb` /
 *    `dynamics.rmsDb`, `rhythm.tempoStability` / `rhythm.timeSignature`, and a
 *    `melody` contour (matching the Node / Python / C-ABI `analyze()` surface);
 *  - `detectChords` / `chordFunctionalAnalysis` reject out-of-range
 *    key / mode / chromaMethod values (mirroring the C-ABI range checks) while
 *    still succeeding on valid input;
 *  - `analyzeMelody` accepts the pYIN / center tracker options.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeMelody,
  chordFunctionalAnalysis,
  detectChords,
  init,
  Mode,
  PitchClass,
} from '../src/index';
import { getSonareModule } from '../src/module_state';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.25 * Math.sin((2 * Math.PI * freqHz * i) / SR);
  }
  return out;
}

describe('WASM wave3 analysis parity', () => {
  beforeAll(async () => {
    await init();
  });

  describe('unified analyze() sub-result fields', () => {
    it('exposes peak/RMS dynamics, rhythm stability + time signature, and a melody contour', () => {
      const samples = makeSine(4, 261.63); // C4
      const result = analyze(samples, SR);

      // Dynamics: peakDb / rmsDb were previously omitted.
      expect(Number.isFinite(result.dynamics.peakDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.rmsDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.dynamicRangeDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.crestFactor)).toBe(true);
      expect(typeof result.dynamics.isCompressed).toBe('boolean');

      // Rhythm: tempoStability + timeSignature were previously omitted.
      expect(Number.isFinite(result.rhythm.tempoStability)).toBe(true);
      expect(typeof result.rhythm.grooveType).toBe('string');
      expect(result.rhythm.timeSignature).toBeDefined();
      expect(Number.isFinite(result.rhythm.timeSignature.numerator)).toBe(true);
      expect(Number.isFinite(result.rhythm.timeSignature.denominator)).toBe(true);
      expect(Number.isFinite(result.rhythm.timeSignature.confidence)).toBe(true);

      // Melody: a whole new sub-object for parity with the other bindings.
      expect(result.melody).toBeDefined();
      expect(Number.isFinite(result.melody.pitchRangeOctaves)).toBe(true);
      expect(Number.isFinite(result.melody.pitchStability)).toBe(true);
      expect(Number.isFinite(result.melody.meanFrequency)).toBe(true);
      expect(Number.isFinite(result.melody.vibratoRate)).toBe(true);
      expect(Array.isArray(result.melody.pitches)).toBe(true);
      for (const p of result.melody.pitches) {
        expect(Number.isFinite(p.time)).toBe(true);
        expect(Number.isFinite(p.frequency)).toBe(true);
        expect(Number.isFinite(p.confidence)).toBe(true);
      }
    });
  });

  describe('detectChords enum-range validation', () => {
    const samples = makeSine(4, 261.63);

    it('succeeds on valid input', () => {
      const result = detectChords(samples, SR);
      expect(Array.isArray(result.chords)).toBe(true);
    });

    it('throws on an out-of-range key root when key context is enabled', () => {
      expect(() =>
        detectChords(samples, SR, {
          useKeyContext: true,
          keyRoot: 99 as PitchClass,
          keyMode: Mode.Major,
        }),
      ).toThrow();
    });

    it('throws on an out-of-range key mode when key context is enabled', () => {
      expect(() =>
        detectChords(samples, SR, {
          useKeyContext: true,
          keyRoot: PitchClass.C,
          keyMode: 42 as Mode,
        }),
      ).toThrow();
    });

    it('throws on an out-of-range chromaMethod (raw module path)', () => {
      const module = getSonareModule();
      // The TS wrapper only accepts 'stft' | 'nnls', so exercise the WASM
      // enum-range guard directly with an out-of-range integer (only 0 and 1
      // are defined).
      expect(() =>
        module.detectChords(
          samples,
          SR,
          0.3,
          2.0,
          0.5,
          false,
          2048,
          512,
          true,
          false,
          24,
          false,
          PitchClass.C,
          Mode.Major,
          false,
          2,
        ),
      ).toThrow();
    });
  });

  describe('chordFunctionalAnalysis enum-range validation', () => {
    const samples = makeSine(4, 261.63);

    it('succeeds on valid input', () => {
      const labels = chordFunctionalAnalysis(samples, PitchClass.C, Mode.Major, SR);
      expect(Array.isArray(labels)).toBe(true);
    });

    it('throws on an out-of-range key root (range-checked unconditionally)', () => {
      expect(() => chordFunctionalAnalysis(samples, 99 as PitchClass, Mode.Major, SR)).toThrow();
    });

    it('throws on an out-of-range key mode (range-checked unconditionally)', () => {
      expect(() => chordFunctionalAnalysis(samples, PitchClass.C, 42 as Mode, SR)).toThrow();
    });

    it('throws on an out-of-range chromaMethod (raw module path)', () => {
      const module = getSonareModule();
      expect(() =>
        module.chordFunctionalAnalysis(
          samples,
          PitchClass.C,
          Mode.Major,
          SR,
          0.3,
          2.0,
          0.5,
          false,
          2048,
          512,
          true,
          false,
          24,
          false,
          false,
          2,
        ),
      ).toThrow();
    });
  });

  describe('analyzeMelody pYIN / center options', () => {
    it('runs with usePyin: true and returns a contour', () => {
      const samples = makeSine(2, 440); // A4
      const result = analyzeMelody(samples, SR, 65.0, 2093.0, 2048, 256, 0.1, {
        usePyin: true,
        center: true,
      });
      expect(Number.isFinite(result.pitchRangeOctaves)).toBe(true);
      expect(Number.isFinite(result.pitchStability)).toBe(true);
      expect(Number.isFinite(result.meanFrequency)).toBe(true);
      expect(Number.isFinite(result.vibratoRate)).toBe(true);
      expect(Array.isArray(result.points)).toBe(true);
      expect(result.points.length).toBeGreaterThan(0);
      for (const p of result.points) {
        expect(Number.isFinite(p.time)).toBe(true);
        expect(Number.isFinite(p.frequency)).toBe(true);
        expect(Number.isFinite(p.confidence)).toBe(true);
      }
    });

    it('still works with default options (backward compatible)', () => {
      const samples = makeSine(2, 440);
      const result = analyzeMelody(samples, SR);
      expect(Array.isArray(result.points)).toBe(true);
      expect(Number.isFinite(result.meanFrequency)).toBe(true);
    });
  });
});
