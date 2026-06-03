import { describe, expect, it } from 'vitest';
import { analyze, analyzeMelody, analyzeWithProgress } from '../src/index.js';

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

describe('analyze() full result (MED-11 / MED-13)', () => {
  it('returns the enriched analysis shape with all new fields', () => {
    const samples = generateChordTone(SR, 3);
    const result = analyze(samples, SR);

    // Legacy fields preserved.
    expect(typeof result.bpm).toBe('number');
    expect(typeof result.bpmConfidence).toBe('number');
    expect(result.key).toBeDefined();
    expect(typeof result.key.root).toBe('string');
    expect(result.timeSignature).toBeDefined();
    expect(typeof result.timeSignature.numerator).toBe('number');

    // beatTimes is still a Float32Array (backward compatibility).
    expect(result.beatTimes).toBeInstanceOf(Float32Array);

    // beats is an array; each beat now also carries a numeric strength.
    expect(Array.isArray(result.beats)).toBe(true);
    if (result.beats.length > 0) {
      expect(typeof result.beats[0].time).toBe('number');
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
    expect(typeof result.rhythm.syncopation).toBe('number');
    expect(typeof result.rhythm.grooveType).toBe('string');

    expect(result.melody).toBeDefined();
    expect(typeof result.melody.meanFrequency).toBe('number');
    expect(Array.isArray(result.melody.pitches)).toBe(true);

    expect(result.form).toBeDefined();
    expect(typeof result.form).toBe('string');
  });

  it('beatTimes matches beats[].time', () => {
    const samples = generateChordTone(SR, 3);
    const result = analyze(samples, SR);
    expect(result.beatTimes.length).toBe(result.beats.length);
    for (let i = 0; i < result.beats.length; i++) {
      expect(result.beatTimes[i]).toBeCloseTo(result.beats[i].time, 3);
    }
  });
});

describe('analyzeWithProgress full result (MED-11 / MED-13)', () => {
  it('fires the progress callback and returns the full shape', () => {
    const samples = generateChordTone(SR, 3);
    const progress: number[] = [];
    const stages: string[] = [];
    const result = analyzeWithProgress(samples, SR, (p, stage) => {
      progress.push(p);
      stages.push(stage);
    });

    // Callback must have fired at least once.
    expect(progress.length).toBeGreaterThan(0);
    for (const p of progress) {
      expect(p).toBeGreaterThanOrEqual(0);
      expect(p).toBeLessThanOrEqual(1);
    }

    // Full shape parity with analyze().
    expect(result.beatTimes).toBeInstanceOf(Float32Array);
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

describe('analyzeMelody pYIN / center (MED-12)', () => {
  it('runs with the pYIN tracker via the options object', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR, 65.0, 2093.0, 2048, 256, 0.1, { usePyin: true });
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequency).toBe('number');
    expect(typeof melody.pitchStability).toBe('number');
    for (const point of melody.points) {
      expect(typeof point.time).toBe('number');
      expect(typeof point.frequency).toBe('number');
      expect(typeof point.confidence).toBe('number');
    }
  });

  it('accepts usePyin / center positionally', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR, 65.0, 2093.0, 2048, 256, 0.1, true, false);
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequency).toBe('number');
  });

  it('defaults to plain YIN when no tracker option is given', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR);
    expect(Array.isArray(melody.points)).toBe(true);
  });
});
