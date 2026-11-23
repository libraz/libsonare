/**
 * A value the library returned that a binding table cannot name is a version skew: a coded
 * `InvalidFormat` with a fixed text, never an argument refusal and never a default.
 */

import { describe, expect, it } from 'vitest';
import { convertChordAnalysisResult } from '../src/analysis_helpers.js';
import { ErrorCode, isSonareError, type SonareError } from '../src/errors.js';
import {
  libraryToken,
  ordinalToken,
  skewError,
  unknownOrdinal,
  unknownToken,
} from '../src/internal/core_tokens.js';
import { tokenName } from '../src/internal/project_tokens.js';

const QUALITIES = ['major', 'minor', 'unknown'] as const;

function caught(fn: () => unknown): SonareError {
  try {
    fn();
  } catch (error) {
    expect(isSonareError(error)).toBe(true);
    return error as SonareError;
  }
  throw new Error('expected the call to throw');
}

describe('core token skew', () => {
  it('maps an ordinal inside the table to its token', () => {
    expect(ordinalToken(QUALITIES, 1, 'chord quality')).toBe('minor');
    expect(libraryToken(['primary', 'half'] as const, 'half', 'BPM candidate relation')).toBe(
      'half',
    );
  });

  it('refuses an ordinal past the table as InvalidFormat with the ordinal text', () => {
    const error = caught(() => ordinalToken(QUALITIES, 99, 'chord quality'));
    expect(error.code).toBe(ErrorCode.InvalidFormat);
    expect(error.message).toBe('unknown chord quality ordinal: 99');
  });

  it('refuses a negative, fractional or non-numeric ordinal the same way', () => {
    for (const bad of [-1, 1.5, Number.NaN, null, undefined, '1']) {
      const error = caught(() => ordinalToken(QUALITIES, bad, 'section type'));
      expect(error.code).toBe(ErrorCode.InvalidFormat);
      expect(error.message).toBe(`unknown section type ordinal: ${String(bad)}`);
    }
  });

  it('refuses a token outside the set as InvalidFormat with the token text', () => {
    const error = caught(() =>
      libraryToken(['primary', 'half'] as const, 'sideways', 'BPM candidate relation'),
    );
    expect(error.code).toBe(ErrorCode.InvalidFormat);
    expect(error.message).toBe('unknown BPM candidate relation token: sideways');
  });

  it('builds the failures directly', () => {
    expect(unknownOrdinal('sidechain refusal', 12).message).toBe(
      'unknown sidechain refusal ordinal: 12',
    );
    expect(unknownToken('key mode', 'weird').message).toBe('unknown key mode token: weird');
    expect(skewError('analysis result section is missing its type field').code).toBe(
      ErrorCode.InvalidFormat,
    );
  });

  it('refuses a chord root or bass outside the twelve pitch classes rather than reading it as C', () => {
    const chord = { root: 0, bass: 0, quality: 0, startSec: 0, endSec: 1, confidence: 1 };
    for (const field of ['root', 'bass'] as const) {
      const error = caught(() =>
        convertChordAnalysisResult({
          chords: [{ ...chord, [field]: 12 }],
          tuningCents: 0,
        } as unknown as Parameters<typeof convertChordAnalysisResult>[0]),
      );
      expect(error.code).toBe(ErrorCode.InvalidFormat);
      expect(error.message).toBe('unknown pitch class ordinal: 12');
    }
  });

  it('reads a project ordinal table through the same failure', () => {
    const error = caught(() => tokenName({ off: 0, loop: 1 } as const, 7, 'loop mode'));
    expect(error.code).toBe(ErrorCode.InvalidFormat);
    expect(error.message).toBe('unknown loop mode ordinal: 7');
  });
});
