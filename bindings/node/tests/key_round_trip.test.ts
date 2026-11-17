import { describe, expect, it } from 'vitest';
import {
  Audio,
  chordFunctionalAnalysis,
  chordFunctions,
  detectChords,
  detectKey,
} from '../src/index.js';

const SAMPLE_RATE = 22050;

function triad(notes: number[], seconds: number): Float32Array {
  const out = new Float32Array(Math.floor(SAMPLE_RATE * seconds));
  for (let i = 0; i < out.length; i++) {
    let sum = 0;
    for (const f of notes) {
      sum += Math.sin((2 * Math.PI * f * i) / SAMPLE_RATE);
    }
    out[i] = 0.25 * sum;
  }
  return out;
}

function progression(): Float32Array {
  const parts = [
    triad([261.63, 329.63, 392.0], 1.5),
    triad([349.23, 440.0, 523.25], 1.5),
    triad([392.0, 493.88, 587.33], 1.5),
    triad([261.63, 329.63, 392.0], 1.5),
  ];
  const out = new Float32Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

const samples = progression();
const base = { samples, sampleRate: SAMPLE_RATE, useBeatSync: false, useTriadsOnly: true };

describe('a detected key goes straight back in', () => {
  it('reports the root as a pitch-class number', () => {
    const key = detectKey(samples, SAMPLE_RATE);
    expect(Number.isInteger(key.root)).toBe(true);
    expect(key.root).toBeGreaterThanOrEqual(0);
    expect(key.root).toBeLessThan(12);
    expect(key.name).toContain(key.mode);
  });

  it('detectChords takes the key as detectKey reports it', () => {
    const key = detectKey(samples, SAMPLE_RATE);
    const fromKey = detectChords({
      ...base,
      useKeyContext: true,
      keyRoot: key.root,
      keyMode: key.mode,
    });
    expect(fromKey.chords.length).toBeGreaterThan(0);
    expect(
      detectChords(samples, SAMPLE_RATE, {
        useBeatSync: false,
        useTriadsOnly: true,
        useKeyContext: true,
        keyRoot: key.root,
        keyMode: key.mode,
      }),
    ).toEqual(fromKey);
  });

  it('detectChords positional form matches the request form for a modal key', () => {
    const options = {
      useBeatSync: false,
      useTriadsOnly: true,
      useKeyContext: true,
      keyRoot: 10,
      keyMode: 'dorian',
    } as const;
    expect(detectChords(samples, SAMPLE_RATE, options)).toEqual(
      detectChords({ samples, sampleRate: SAMPLE_RATE, ...options }),
    );
  });

  it('chordFunctionalAnalysis agrees across the request, positional and Audio forms', () => {
    const options = {
      useBeatSync: false,
      useTriadsOnly: true,
      keyRoot: 0,
      keyMode: 'major',
    } as const;
    const byRequest = chordFunctionalAnalysis({ samples, sampleRate: SAMPLE_RATE, ...options });
    expect(byRequest.length).toBeGreaterThan(0);
    expect(chordFunctionalAnalysis(samples, SAMPLE_RATE, options)).toEqual(byRequest);
    const key = detectKey(samples, SAMPLE_RATE);
    expect(() =>
      chordFunctionalAnalysis({ ...base, keyRoot: key.root, keyMode: key.mode }),
    ).not.toThrow();
    const audio = Audio.fromBuffer(samples, SAMPLE_RATE);
    try {
      expect(audio.chordFunctionalAnalysis(options)).toEqual(byRequest);
    } finally {
      audio.destroy();
    }
  });

  it('chordFunctions takes a detected key whole', () => {
    const key = detectKey(samples, SAMPLE_RATE);
    expect(chordFunctions({ chords: [], key })).toEqual([]);
  });

  it('refuses a pitch-class name or out-of-range root, and a wrong-type mode, naming the field', () => {
    expect(() => detectChords({ ...base, keyRoot: 12 as never })).toThrow(RangeError);
    expect(() => detectChords({ ...base, keyRoot: 12 as never })).toThrow(/keyRoot/);
    expect(() => detectChords({ ...base, keyRoot: 'G' as never })).toThrow(TypeError);
    expect(() => detectChords({ ...base, keyRoot: 'G' as never })).toThrow(/keyRoot/);
    expect(() => detectChords({ ...base, keyMode: 'nope' as never })).toThrow(RangeError);
    expect(() => detectChords({ ...base, keyMode: 'nope' as never })).toThrow(/keyMode/);
    expect(() => detectChords({ ...base, keyMode: 1 as never })).toThrow(TypeError);
    expect(() => detectChords({ ...base, keyRoot: {} as never })).toThrow(TypeError);
    expect(() => detectChords({ ...base, keyMode: {} as never })).toThrow(/keyMode/);
    expect(() =>
      chordFunctionalAnalysis({ ...base, keyRoot: 'C' as never, keyMode: 'major' }),
    ).toThrow(/keyRoot/);
    expect(() =>
      chordFunctionalAnalysis({ ...base, keyRoot: 0, keyMode: 'nope' as never }),
    ).toThrow(/keyMode/);
    expect(() => chordFunctionalAnalysis({ ...base } as never)).toThrow(/keyRoot/);
  });
});
