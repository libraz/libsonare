/**
 * Analysis parity tests for the WASM binding.
 *
 * Covers the full analysis surface shared with Node / Python / C-ABI:
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
  analyzeDynamics,
  analyzeMelody,
  CHORD_QUALITIES,
  chordFunctionalAnalysis,
  chordFunctions,
  detectChords,
  init,
  type KeyMode,
  PitchClass,
} from '../src/index';
import { getSonareModule } from '../src/module_state';
import { sine as genSine } from './_helpers';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  return genSine(freqHz, durationSec, { amp: 0.25, sampleRate: SR });
}

function collectSchemaPaths(value: unknown, prefix = '', out = new Set<string>()): Set<string> {
  if (Array.isArray(value)) {
    if (value.length > 0) {
      collectSchemaPaths(value[0], `${prefix}[]`, out);
    }
    return out;
  }
  if (value !== null && typeof value === 'object') {
    for (const key of Object.keys(value).sort()) {
      const path = prefix ? `${prefix}.${key}` : key;
      out.add(path);
      collectSchemaPaths((value as Record<string, unknown>)[key], path, out);
    }
  }
  return out;
}

describe('WASM wave3 analysis parity', () => {
  beforeAll(async () => {
    await init();
  });

  describe('unified analyze() sub-result fields', () => {
    it('keeps the native object field set in parity with the JSON schema snapshot', () => {
      const module = getSonareModule();
      const expected = [...module._analysisResultSchemaPaths()].sort();
      const actual = [...collectSchemaPaths(module._analysisResultSchemaFixture())].sort();

      expect(actual).toEqual(expected);
    });

    it('reads the dynamic range of a tone followed by silence as +Infinity', () => {
      const tone = makeSine(2, 440);
      const gapped = new Float32Array(tone.length * 2);
      gapped.set(tone, 0);
      // The JSON route carries the infinite end as null and the facade restores it.
      expect(analyze(gapped, SR).dynamics.dynamicRangeDb).toBe(Number.POSITIVE_INFINITY);
      expect(analyzeDynamics(gapped, SR).dynamicRangeDb).toBe(Number.POSITIVE_INFINITY);
    });

    it('exposes tempo/meter hypotheses, dynamics, rhythm stability, and a melody contour', () => {
      const samples = makeSine(4, 261.63); // C4
      const result = analyze(samples, SR);

      expect(Array.isArray(result.bpmCandidates)).toBe(true);
      expect(result.bpmCandidates[0]?.relation).toBe('primary');
      expect(result.bpmCandidates[0]?.bpm).toBe(result.bpm);
      expect(Array.isArray(result.timeSignatureCandidates)).toBe(true);
      expect(result.timeSignatureCandidates.length).toBeGreaterThan(0);

      // Dynamics: peakDb / rmsDb were previously omitted.
      expect(Number.isFinite(result.dynamics.peakDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.rmsDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.dynamicRangeDb)).toBe(true);
      expect(Number.isFinite(result.dynamics.crestFactor)).toBe(true);
      expect(typeof result.dynamics.isCompressed).toBe('boolean');

      // Rhythm: tempoStability + timeSignature were previously omitted.
      // A steady tone has no beat-to-beat intervals, so the stability may be unmeasured (absent).
      if ('tempoStability' in result.rhythm) {
        expect(Number.isFinite(result.rhythm.tempoStability)).toBe(true);
      }
      expect(typeof result.rhythm.grooveType).toBe('string');
      expect(result.rhythm.timeSignature).toBeDefined();
      expect(Number.isFinite(result.rhythm.timeSignature.numerator)).toBe(true);
      expect(Number.isFinite(result.rhythm.timeSignature.denominator)).toBe(true);
      expect(Number.isFinite(result.rhythm.timeSignature.confidence)).toBe(true);

      // Melody: a whole new sub-object for parity with the other bindings.
      expect(result.melody).toBeDefined();
      // A melody measure the analysis could not take is absent, never a placeholder number.
      for (const key of [
        'pitchRangeOctaves',
        'pitchStability',
        'meanFrequencyHz',
        'vibratoRateHz',
      ] as const) {
        expect(key in result.melody ? Number.isFinite(result.melody[key]) : true, key).toBe(true);
      }
      expect(Array.isArray(result.melody.pitches)).toBe(true);
      for (const p of result.melody.pitches) {
        expect(Number.isFinite(p.timeSec)).toBe(true);
        // frequencyHz is absent on an unvoiced frame.
        expect(p.frequencyHz === undefined || Number.isFinite(p.frequencyHz)).toBe(true);
        expect(Number.isFinite(p.confidence)).toBe(true);
      }
    });
  });

  describe('detectChords enum-range validation', () => {
    const samples = makeSine(4, 261.63);

    it('succeeds on valid input', () => {
      const result = detectChords(samples, SR);
      expect(Array.isArray(result.chords)).toBe(true);
      for (const chord of result.chords) {
        expect(typeof chord.rootName).toBe('string');
        expect(typeof chord.bassName).toBe('string');
      }
    });

    it('uses threshold to emit a continuous N.C. segment', () => {
      const silence = new Float32Array(4096);
      const common = {
        minDurationSec: 0,
        smoothingWindowSec: 0.01,
        useTriadsOnly: true,
        nFft: 2048,
        hopLength: 512,
        useBeatSync: false,
      } as const;
      const low = detectChords(silence, SR, { ...common, threshold: 0 });
      const high = detectChords(silence, SR, { ...common, threshold: 0.99 });
      expect(low.chords).toHaveLength(1);
      expect(low.chords[0]?.quality).not.toBe('unknown');
      expect(high.chords).toHaveLength(1);
      expect(high.chords[0]?.quality).toBe('unknown');
      expect(high.chords[0]?.name).toBe('N.C.');
      expect(high.chords[0]?.startSec).toBe(0);
      expect(high.chords[0]?.endSec).toBeGreaterThan(0);
    });

    it('throws on an out-of-range key root when key context is enabled', () => {
      expect(() =>
        detectChords(samples, SR, {
          useKeyContext: true,
          keyRoot: 99 as PitchClass,
          keyMode: 'major',
        }),
      ).toThrow();
    });

    it('throws on an out-of-range key mode when key context is enabled', () => {
      expect(() =>
        detectChords(samples, SR, {
          useKeyContext: true,
          keyRoot: PitchClass.C,
          keyMode: 'bogus' as KeyMode,
        }),
      ).toThrow();
    });

    it('throws on an out-of-range chromaMethod (raw module path)', () => {
      const module = getSonareModule();
      // The TS wrapper only accepts 'stft' | 'nnls', so exercise the WASM
      // enum-range guard directly with an out-of-range integer (only 0, 1 and 2
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
          0,
          false,
          3,
          0,
        ),
      ).toThrow();
    });

    // Mirrors the C-ABI's sonare_detect_chords_ex range checks: a negative
    // smoothingWindow and a negative hmmBeamWidth are rejected on every
    // surface, not just passed through to the DSP layer. A window of 0 turns
    // smoothing off and still detects chords.
    it('throws on a negative smoothingWindow and accepts 0', () => {
      expect(() => detectChords(samples, SR, { smoothingWindowSec: -1 })).toThrow(
        new RangeError('detectChords: smoothingWindowSec must be finite and at least 0'),
      );
      expect(detectChords(samples, SR, { smoothingWindowSec: 0 }).chords.length).toBeGreaterThan(0);
    });

    it('throws on a negative hmmBeamWidth', () => {
      expect(() => detectChords(samples, SR, { useHmm: true, hmmBeamWidth: -1 })).toThrow();
    });
  });

  describe('chordFunctionalAnalysis enum-range validation', () => {
    const samples = makeSine(4, 261.63);

    it('succeeds on valid input', () => {
      const labels = chordFunctionalAnalysis({
        samples,
        sampleRate: SR,
        keyRoot: PitchClass.C,
        keyMode: 'major',
      });
      expect(Array.isArray(labels)).toBe(true);
      expect(labels).toEqual(
        chordFunctions({
          chords: detectChords(samples, SR),
          key: { root: PitchClass.C, mode: 'major' },
        }),
      );
    });

    it('throws on an out-of-range key root (range-checked unconditionally)', () => {
      expect(() =>
        chordFunctionalAnalysis({
          samples,
          sampleRate: SR,
          keyRoot: 99 as PitchClass,
          keyMode: 'major',
        }),
      ).toThrow();
    });

    it('throws on an out-of-range key mode (range-checked unconditionally)', () => {
      expect(() =>
        chordFunctionalAnalysis({
          samples,
          sampleRate: SR,
          keyRoot: PitchClass.C,
          keyMode: 'bogus' as KeyMode,
        }),
      ).toThrow();
    });

    it('throws on an out-of-range chromaMethod (raw module path)', () => {
      const module = getSonareModule();
      expect(() =>
        module.chordFunctionalAnalysis(
          samples,
          PitchClass.C,
          0,
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
          3,
          0,
        ),
      ).toThrow();
    });

    it('throws on a negative smoothingWindow and accepts 0', () => {
      const request = { samples, sampleRate: SR, keyRoot: PitchClass.C, keyMode: 'major' as const };
      expect(() => chordFunctionalAnalysis({ ...request, smoothingWindowSec: -1 })).toThrow(
        new RangeError('chordFunctionalAnalysis: smoothingWindowSec must be finite and at least 0'),
      );
      expect(chordFunctionalAnalysis({ ...request, smoothingWindowSec: 0 }).length).toBeGreaterThan(
        0,
      );
    });

    it('throws on a negative hmmBeamWidth', () => {
      expect(() =>
        chordFunctionalAnalysis({
          samples,
          sampleRate: SR,
          keyRoot: PitchClass.C,
          keyMode: 'major',
          useHmm: true,
          hmmBeamWidth: -1,
        }),
      ).toThrow();
    });
  });

  describe('analyzeMelody pYIN / center options', () => {
    it('runs with usePyin: true and returns a contour', () => {
      const samples = makeSine(2, 440); // A4
      const result = analyzeMelody(samples, SR, {
        f0MinHz: 65.0,
        f0MaxHz: 2093.0,
        frameLength: 2048,
        hopLength: 256,
        threshold: 0.1,
        usePyin: true,
        center: true,
      });
      expect(Number.isFinite(result.pitchRangeOctaves)).toBe(true);
      expect(Number.isFinite(result.pitchStability)).toBe(true);
      expect(Number.isFinite(result.meanFrequencyHz)).toBe(true);
      expect(Number.isFinite(result.vibratoRateHz)).toBe(true);
      expect(Array.isArray(result.points)).toBe(true);
      expect(result.points.length).toBeGreaterThan(0);
      for (const p of result.points) {
        expect(Number.isFinite(p.timeSec)).toBe(true);
        // frequencyHz is absent on an unvoiced frame.
        expect(p.frequencyHz === undefined || Number.isFinite(p.frequencyHz)).toBe(true);
        expect(Number.isFinite(p.confidence)).toBe(true);
      }
    });

    it('still works with default options (backward compatible)', () => {
      const samples = makeSine(2, 440);
      const result = analyzeMelody(samples, SR);
      expect(Array.isArray(result.points)).toBe(true);
      expect(Number.isFinite(result.meanFrequencyHz)).toBe(true);
    });
  });
});

describe('chord entry shape', () => {
  beforeAll(async () => {
    await init();
  });

  // The same field set and types come from detectChords and analyze, on Node and WASM alike.
  const CHORD_FIELDS = [
    'bass',
    'bassName',
    'confidence',
    'durationSec',
    'endSec',
    'name',
    'quality',
    'root',
    'rootName',
    'startSec',
  ];
  const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  it('gives detectChords and analyze chords one ordinal + name shape for a C major triad', () => {
    const triad = new Float32Array(SR * 4);
    for (let i = 0; i < triad.length; i++) {
      triad[i] =
        0.25 *
        (Math.sin((2 * Math.PI * 261.63 * i) / SR) +
          Math.sin((2 * Math.PI * 329.63 * i) / SR) +
          Math.sin((2 * Math.PI * 392.0 * i) / SR));
    }
    const detected = detectChords(triad, SR).chords;
    const analysed = analyze(triad, SR).chords;
    expect(detected.length).toBeGreaterThan(0);
    expect(analysed.length).toBeGreaterThan(0);
    for (const chord of detected) {
      expect(Object.keys(chord).sort()).toEqual(CHORD_FIELDS);
    }
    for (const chord of analysed) {
      expect(Object.keys(chord).sort()).toEqual([...CHORD_FIELDS, 'romanNumeral'].sort());
    }
    for (const chord of [...detected, ...analysed]) {
      expect(typeof chord.root).toBe('number');
      expect(typeof chord.bass).toBe('number');
      expect(CHORD_QUALITIES).toContain(chord.quality);
      expect(chord.rootName).toBe(PITCH_NAMES[chord.root]);
      expect(chord.bassName).toBe(PITCH_NAMES[chord.bass]);
      expect(chord.durationSec).toBeCloseTo(chord.endSec - chord.startSec, 5);
    }
    const first = detected.find((chord) => chord.name !== 'N.C.');
    expect(first?.rootName).toBe('C');
    expect(first?.root).toBe(0);
  });
});
