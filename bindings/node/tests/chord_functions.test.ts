import { describe, expect, it } from 'vitest';
import { type ChordFunctionsInput, chordFunctions, detectChords } from '../src/index.js';

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

describe('chordFunctions', () => {
  it('labels a detected C-F-G-C progression in C major without re-detecting', () => {
    const detected = detectChords({
      samples: progression(),
      sampleRate: SAMPLE_RATE,
      useBeatSync: false,
      useTriadsOnly: true,
    });
    const result = chordFunctions({ chords: detected, key: { root: 0, mode: 'major' } });
    expect(result.length).toBe(detected.chords.length);
    result.forEach((entry, i) => {
      expect(entry.startSec).toBe(detected.chords[i].startSec);
      expect(entry.endSec).toBe(detected.chords[i].endSec);
      expect(entry.name).toBe(detected.chords[i].name);
    });
    const collapsed = result.filter(
      (entry, i) => i === 0 || entry.romanNumeral !== result[i - 1].romanNumeral,
    );
    expect(collapsed.map((entry) => entry.romanNumeral)).toEqual(['I', 'IV', 'V', 'I']);
    expect(collapsed.map((entry) => entry.function)).toEqual([
      'tonic',
      'subdominant',
      'dominant',
      'tonic',
    ]);
    expect(chordFunctions({ chords: detected.chords, key: { root: 0, mode: 'major' } })).toEqual(
      result,
    );
  });

  it('labels hand-built chords, including unknown and chromatic ones', () => {
    const chords: (ChordFunctionsInput & {
      startSec: number;
      endSec: number;
      confidence: number;
    })[] = [
      { root: 9, quality: 'minor', startSec: 0, endSec: 1, confidence: 0.9 },
      { root: 2, quality: 'minor', startSec: 1, endSec: 2, confidence: 0.9 },
      { root: 4, quality: 'major', startSec: 2, endSec: 3, confidence: 0.9 },
      { root: 0, quality: 'unknown', startSec: 3, endSec: 4, confidence: 0 },
      { root: 10, quality: 'major', startSec: 4, endSec: 5, confidence: 0.9 },
    ];
    const result = chordFunctions({ chords, key: { root: 9, mode: 'minor' } });
    expect(result.map((entry) => entry.function)).toEqual([
      'tonic',
      'subdominant',
      'dominant',
      'none',
      'chromatic',
    ]);
    expect(result[3].romanNumeral).toBe('N.C.');
    expect(result[0]).toMatchObject({ startSec: 0, endSec: 1, confidence: 0.9 });
  });

  it('refuses an invalid key or chord', () => {
    expect(() =>
      chordFunctions({ chords: [], key: { root: 12 as never, mode: 'major' } }),
    ).toThrow();
    expect(() =>
      chordFunctions({ chords: [], key: { root: 'C' as never, mode: 'major' } }),
    ).toThrow();
    expect(() =>
      chordFunctions({
        chords: [{ root: 0, quality: 'bogus' as never }],
        key: { root: 0, mode: 'major' },
      }),
    ).toThrow();
    expect(chordFunctions({ chords: [], key: { root: 0, mode: 'major' } })).toEqual([]);
  });
});

describe('detectChords chromaMethod', () => {
  const samples = progression();
  const base = { samples, sampleRate: SAMPLE_RATE, useBeatSync: false };

  it('defaults to the triads-only, HMM, CQT configuration', () => {
    expect(detectChords(base)).toEqual(
      detectChords({ ...base, useTriadsOnly: true, useHmm: true, chromaMethod: 'cqt' }),
    );
  });

  it('accepts cqt', () => {
    expect(() => detectChords({ ...base, chromaMethod: 'cqt' })).not.toThrow();
  });

  it('rejects an unknown chromaMethod', () => {
    expect(() => detectChords({ ...base, chromaMethod: 'x' as 'cqt' })).toThrow();
  });
});

describe('chordFunctions refuses a malformed request by the field the caller wrote', () => {
  const key = { root: 0, mode: 'major' } as const;
  const cases: ReadonlyArray<readonly [string, unknown, typeof TypeError, string]> = [
    ['a non-object key', { chords: [], key: 0 }, TypeError, 'key must be an object'],
    [
      'a non-object chord list',
      { chords: 0, key },
      TypeError,
      'chords must be an array or a chord analysis result',
    ],
    [
      'a result whose chords are not a list',
      { chords: { chords: 5 }, key },
      TypeError,
      'chords must be an array',
    ],
    ['a non-object chord', { chords: [5], key }, TypeError, 'chords[0] must be an object'],
    [
      'a chord root outside the pitch classes',
      {
        chords: [
          { root: 0, quality: 'major' },
          { root: 12, quality: 'major' },
        ],
        key,
      },
      RangeError,
      'chords[1].root must be an integer in [0, 11]',
    ],
  ];
  it.each(cases)('refuses %s', (_case, request, errorClass, text) => {
    let caught: unknown;
    try {
      chordFunctions(request as never);
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(errorClass);
    expect((caught as Error).message).toBe(`chordFunctions: ${text}`);
  });
});
