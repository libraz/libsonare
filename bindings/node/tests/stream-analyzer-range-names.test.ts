/**
 * StreamAnalyzer builds the core analyzer directly, so a refusal of its mel range names the
 * caller's `fminHz` / `fmaxHz` rather than the core's own member names.
 */

import { describe, expect, it } from 'vitest';
import { StreamAnalyzer } from '../src/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');

describe('StreamAnalyzer A4 reference', () => {
  it.each([220, 880])('accepts references that round onto the %d Hz endpoint', (bound) => {
    const rounded = bound === 220 ? 219.999999 : 880.000001;
    using analyzer = new StreamAnalyzer({ sampleRate: 22050, referenceHz: rounded });
    expect(analyzer.sampleRate).toBe(22050);
    expect(() => analyzer.setReferenceHz(rounded)).not.toThrow();
  });

  it('names the native reference domain in both calls', () => {
    const domain = 'referenceHz must be finite and in [220, 880]';
    expect(() => new StreamAnalyzer({ sampleRate: 22050, referenceHz: 881 })).toThrow(
      new RangeError(`StreamAnalyzer: ${domain}`),
    );
    using analyzer = new StreamAnalyzer({ sampleRate: 22050 });
    expect(() => analyzer.setReferenceHz(881)).toThrow(RangeError);
    expect(() => analyzer.setReferenceHz(881)).toThrow(domain);
  });
});

describe('StreamAnalyzer mel range refusal names', () => {
  it('names fmaxHz and fminHz when the range is inverted', () => {
    let caught: unknown;
    try {
      new StreamAnalyzer({ sampleRate: 22050, fminHz: 8000, fmaxHz: 4000 });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
    const refusal = (caught as Record<symbol, { subjects: string[] }>)[REFUSAL];
    expect(refusal.subjects).toEqual(['fmaxHz', 'fminHz']);
    expect((caught as Error).message).toMatch(/^StreamAnalyzer: fmaxHz must .* fminHz$/);
  });

  it('accepts a range whose top sits above its bottom', () => {
    expect(
      () => new StreamAnalyzer({ sampleRate: 22050, fminHz: 100, fmaxHz: 4000 }),
    ).not.toThrow();
  });
});
