/**
 * The request-object equivalence inventory: one row per exported request-object overload, across
 * the per-domain files that run them, and the one overload compared through its handle.
 */

import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index';
import { ALT_SR, overloadedFunctions, SIG2 } from './request_equivalence/harness';
import { ROWS as ANALYSIS } from './request_equivalence/rows_analysis';
import { ROWS as EFFECTS } from './request_equivalence/rows_effects';
import { ROWS as FEATURES } from './request_equivalence/rows_features';
import { ROWS as MASTERING } from './request_equivalence/rows_mastering';
import { ROWS as SPECTRAL } from './request_equivalence/rows_spectral';

const ROWS = [...ANALYSIS, ...FEATURES, ...SPECTRAL, ...EFFECTS, ...MASTERING];

describe('request-object equivalence', () => {
  it('has one row per exported request-object overload', () => {
    const overloaded = ROWS.filter((row) => row.form !== 'requestOnly').map((row) => row.name);
    expect(overloaded.sort()).toEqual(overloadedFunctions());
  });

  it('compares a polyphonic analysis through its notes, not its handle', () => {
    const options = { nFft: 2048, hopLength: 512 };
    const request = sonare.analyzePolyphonic({ samples: SIG2, sampleRate: ALT_SR, ...options });
    const positional = sonare.analyzePolyphonic(SIG2, ALT_SR, options);
    try {
      expect(request.noteCount).toBe(positional.noteCount);
      expect(request.frameCount).toBe(positional.frameCount);
      expect(request.notes).toEqual(positional.notes);
    } finally {
      request.destroy();
      positional.destroy();
    }
  });
});
