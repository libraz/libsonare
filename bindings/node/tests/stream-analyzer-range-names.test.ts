/**
 * StreamAnalyzer builds the core analyzer directly, so a refusal of its mel range names the
 * caller's `fminHz` / `fmaxHz` rather than the core's own member names.
 */

import { describe, expect, it } from 'vitest';
import { StreamAnalyzer } from '../src/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');

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
