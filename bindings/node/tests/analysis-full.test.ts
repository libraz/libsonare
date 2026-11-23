import { describe, expect, it } from 'vitest';
import { analyze, analyzeDynamics, analyzeMelody } from '../src/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function generateChordTone(sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] =
      0.25 *
      (Math.sin((2 * Math.PI * 261.63 * i) / sr) +
        Math.sin((2 * Math.PI * 329.63 * i) / sr) +
        Math.sin((2 * Math.PI * 392.0 * i) / sr));
  }
  return samples;
}

describe('analyze() full result', () => {
  it('reads the dynamic range of a tone followed by silence as +Infinity', () => {
    const tone = generateSine(440, SR, 2);
    const gapped = new Float32Array(tone.length * 2);
    gapped.set(tone, 0);
    // The JSON route carries the infinite end as null and the facade restores it.
    expect(analyze(gapped, SR).dynamics.dynamicRangeDb).toBe(Number.POSITIVE_INFINITY);
    expect(analyzeDynamics(gapped, SR).dynamicRangeDb).toBe(Number.POSITIVE_INFINITY);
  });

  it('returns the enriched analysis shape with all new fields', () => {
    const samples = generateChordTone(SR, 3);
    const result = analyze(samples, SR);

    // Legacy fields preserved.
    expect(typeof result.bpm).toBe('number');
    expect(typeof result.bpmConfidence).toBe('number');
    expect(Array.isArray(result.bpmCandidates)).toBe(true);
    expect(result.bpmCandidates[0]?.relation).toBe('primary');
    expect(result.bpmCandidates[0]?.bpm).toBe(result.bpm);
    expect(result.key).toBeDefined();
    expect(typeof result.key.root).toBe('number');
    expect(result.timeSignature).toBeDefined();
    expect(typeof result.timeSignature.numerator).toBe('number');
    expect(Array.isArray(result.timeSignatureCandidates)).toBe(true);
    expect(result.timeSignatureCandidates.length).toBeGreaterThan(0);

    // beats is an array; each beat carries a time and a numeric strength.
    expect(Array.isArray(result.beats)).toBe(true);
    if (result.beats.length > 0) {
      expect(typeof result.beats[0].timeSec).toBe('number');
      expect(typeof result.beats[0].strength).toBe('number');
    }

    // New aggregate fields are present and well-typed.
    expect(Array.isArray(result.chords)).toBe(true);
    expect(Array.isArray(result.sections)).toBe(true);

    expect(result.timbre).toBeDefined();
    expect(typeof result.timbre.brightness).toBe('number');
    expect(typeof result.timbre.complexity).toBe('number');

    expect(result.dynamics).toBeDefined();
    expect(typeof result.dynamics.dynamicRangeDb).toBe('number');
    expect(typeof result.dynamics.isCompressed).toBe('boolean');

    expect(result.rhythm).toBeDefined();
    expect(result.rhythm.timeSignature).toBeDefined();
    // A sustained chord has no tracked beats to measure syncopation on.
    expect('syncopation' in result.rhythm).toBe(false);
    expect(typeof result.rhythm.grooveType).toBe('string');

    expect(result.melody).toBeDefined();
    expect(typeof result.melody.meanFrequencyHz).toBe('number');
    expect(Array.isArray(result.melody.pitches)).toBe(true);

    expect(result.form).toBeDefined();
    expect(typeof result.form).toBe('string');
  });

  it('beats are ordered in time', () => {
    const samples = generateChordTone(SR, 3);
    const result = analyze(samples, SR);
    for (let i = 1; i < result.beats.length; i++) {
      expect(result.beats[i].timeSec).toBeGreaterThanOrEqual(result.beats[i - 1].timeSec);
    }
  });
});

describe('analyze() with onProgress full result', () => {
  it('fires the progress callback and returns the full shape', () => {
    const samples = generateChordTone(SR, 3);
    const progress: number[] = [];
    const stages: string[] = [];
    const result = analyze(samples, SR, {
      onProgress: (p, stage) => {
        progress.push(p);
        stages.push(stage);
      },
    });

    // Callback must have fired at least once.
    expect(progress.length).toBeGreaterThan(0);
    for (const p of progress) {
      expect(p).toBeGreaterThanOrEqual(0);
      expect(p).toBeLessThanOrEqual(1);
    }

    // Full shape parity with analyze().
    expect(Array.isArray(result.beats)).toBe(true);
    expect(Array.isArray(result.chords)).toBe(true);
    expect(Array.isArray(result.sections)).toBe(true);
    expect(result.timbre).toBeDefined();
    expect(result.dynamics).toBeDefined();
    expect(result.rhythm).toBeDefined();
    expect(result.melody).toBeDefined();
    expect(typeof result.form).toBe('string');
  });
});

describe('analyzeMelody pYIN / center', () => {
  it('runs with the pYIN tracker via the options object', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR, { usePyin: true });
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequencyHz).toBe('number');
    expect(typeof melody.pitchStability).toBe('number');
    for (const point of melody.points) {
      expect(typeof point.timeSec).toBe('number');
      if (point.frequencyHz !== undefined) {
        expect(typeof point.frequencyHz).toBe('number');
      }
      expect(typeof point.confidence).toBe('number');
    }
  });

  it('accepts explicit tuning + tracker options', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR, {
      f0MinHz: 65.0,
      f0MaxHz: 2093.0,
      frameLength: 2048,
      hopLength: 256,
      threshold: 0.1,
      usePyin: true,
      center: false,
    });
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequencyHz).toBe('number');
  });

  it('defaults to plain YIN when no tracker option is given', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR);
    expect(Array.isArray(melody.points)).toBe(true);
  });
});
