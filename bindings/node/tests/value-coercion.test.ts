import { describe, expect, it } from 'vitest';

import {
  meterTapValue,
  projectLoopModeValue,
  resolveEnumOrdinal,
  sendTimingValue,
  trackKindValue,
  warpModeValue,
} from '../src/value_coercion.js';

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
});
