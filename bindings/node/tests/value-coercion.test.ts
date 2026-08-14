import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { EngineTrackMonitorMode } from '../src/types_engine.js';
import type { PanLawInput } from '../src/types_mastering.js';
import {
  meterTapValue,
  panLawValue,
  projectLoopModeValue,
  resolveEnumOrdinal,
  sendTimingValue,
  trackKindValue,
  trackMonitorModeValue,
  warpModeValue,
} from '../src/value_coercion.js';

interface PanLawCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  normalization: Array<{ value: string; ordinal: number }>;
  numeric: number[];
  rejected: string[];
}

const panLawCorpus = JSON.parse(
  readFileSync(new URL('../../../tests/conformance/pan_law_names.json', import.meta.url), 'utf8'),
) as PanLawCorpus;

describe('enum ordinal coercion', () => {
  it('accepts only declared ordinals and spellings', () => {
    expect(resolveEnumOrdinal('a', { a: 0, b: 1 }, 'test enum')).toBe(0);
    expect(resolveEnumOrdinal(1, { a: 0, b: 1 }, 'test enum')).toBe(1);
    expect(() => resolveEnumOrdinal(2, { a: 0, b: 1 }, 'test enum')).toThrow(RangeError);
    expect(() => resolveEnumOrdinal(0.5, { a: 0, b: 1 }, 'test enum')).toThrow(RangeError);
  });

  it('does not pass unknown values through to the C ABI', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => trackKindValue(3 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => warpModeValue(-1 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => projectLoopModeValue(8 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => meterTapValue(2 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => sendTimingValue(2 as any)).toThrow(RangeError);
  });

  it('accepts only exact track monitor mode spellings and ordinals', () => {
    const accepted: Array<[EngineTrackMonitorMode, number]> = [
      ['off', 0],
      ['pfl', 1],
      ['afl', 2],
      [0, 0],
      [1, 1],
      [2, 2],
    ];
    for (const [mode, ordinal] of accepted) {
      expect(trackMonitorModeValue(mode)).toBe(ordinal);
    }

    const rejected: unknown[] = [
      true,
      false,
      0.5,
      Number.NaN,
      -1,
      3,
      Number.POSITIVE_INFINITY,
      'PFL',
      'mix',
    ];
    for (const mode of rejected) {
      expect(() => trackMonitorModeValue(mode as EngineTrackMonitorMode)).toThrow(RangeError);
    }
  });

  it('resolves every shared pan-law spelling and conservative normalization', () => {
    for (const { value, ordinal } of [...panLawCorpus.accepted, ...panLawCorpus.normalization]) {
      expect(panLawValue(value as PanLawInput)).toBe(ordinal);
    }
    for (const value of panLawCorpus.numeric) {
      expect(panLawValue(value)).toBe(value);
    }
    for (const value of panLawCorpus.rejected) {
      expect(() => panLawValue(value as PanLawInput)).toThrow(RangeError);
    }
  });
});
