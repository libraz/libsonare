import { beforeAll, describe, expect, it } from 'vitest';
import {
  ChordQuality,
  chordFunctionalAnalysis,
  chordFunctions,
  detectChords,
  detectKey,
  init,
  PitchClass,
} from '../src/index';

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
  beforeAll(async () => {
    await init();
  });

  it('detectChords takes the key by name or ordinal, request and positional forms', () => {
    const ordinals = detectChords({ ...base, useKeyContext: true, keyRoot: 7, keyMode: 1 });
    expect(detectChords({ ...base, useKeyContext: true, keyRoot: 'G', keyMode: 'minor' })).toEqual(
      ordinals,
    );
    const key = detectKey(samples, SAMPLE_RATE);
    expect(() =>
      detectChords({ ...base, useKeyContext: true, keyRoot: key.root, keyMode: key.mode }),
    ).not.toThrow();
    const options = { useBeatSync: false, useTriadsOnly: true, useKeyContext: true };
    expect(
      detectChords(samples, SAMPLE_RATE, { ...options, keyRoot: 'G', keyMode: 'minor' }),
    ).toEqual(detectChords(samples, SAMPLE_RATE, { ...options, keyRoot: 7, keyMode: 1 }));
  });

  it('chordFunctionalAnalysis takes name or ordinal', () => {
    const options = { useBeatSync: false, useTriadsOnly: true };
    const byOrdinal = chordFunctionalAnalysis({ ...base, keyRoot: 0, keyMode: 0 });
    expect(chordFunctionalAnalysis({ ...base, keyRoot: 'C', keyMode: 'major' })).toEqual(byOrdinal);
    expect(chordFunctionalAnalysis(samples, 'C', 'major', SAMPLE_RATE, options)).toEqual(byOrdinal);
    const key = detectKey(samples, SAMPLE_RATE);
    expect(() =>
      chordFunctionalAnalysis({ ...base, keyRoot: key.root, keyMode: key.mode }),
    ).not.toThrow();
  });

  it('chordFunctions takes the root by name or ordinal', () => {
    const chords = [{ root: PitchClass.C, quality: ChordQuality.Major }];
    const byOrdinal = chordFunctions({ chords, key: { root: 7, mode: 0 } });
    expect(chordFunctions({ chords, key: { root: 'G', mode: 'major' } })).toEqual(byOrdinal);
  });

  it('refuses an unknown name naming the field, and a wrong type as a TypeError', () => {
    expect(() => detectChords({ ...base, keyRoot: 'H' as never })).toThrow(RangeError);
    expect(() => detectChords({ ...base, keyRoot: 'H' as never })).toThrow(/keyRoot/);
    expect(() => detectChords({ ...base, keyMode: 'nope' as never })).toThrow(RangeError);
    expect(() => detectChords({ ...base, keyMode: 'nope' as never })).toThrow(/keyMode/);
    expect(() => detectChords({ ...base, keyRoot: {} as never })).toThrow(TypeError);
    expect(() => detectChords({ ...base, keyMode: {} as never })).toThrow(TypeError);
    expect(() => chordFunctionalAnalysis({ ...base, keyRoot: 'H' as never })).toThrow(/keyRoot/);
    expect(() =>
      chordFunctionalAnalysis({ ...base, keyRoot: 0, keyMode: 'nope' as never }),
    ).toThrow(/keyMode/);
  });
});
