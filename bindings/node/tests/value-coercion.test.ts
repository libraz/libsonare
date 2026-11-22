import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { ProjectAutomationCurve, ProjectAutomationPoint } from '../src/types_project.js';
import {
  AUTOMATION_CURVE_VALUES,
  automationPointValue,
  meterTapValue,
  panLawValue,
  resolveEnumOrdinal,
} from '../src/value_coercion.js';

interface PanLawCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  rejected: string[];
  rejected_numeric: number[];
}

interface AutomationCurveCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  rejected: string[];
  rejected_numeric: number[];
}

const panLawCorpus = JSON.parse(
  readFileSync(new URL('../../../tests/conformance/pan_law_names.json', import.meta.url), 'utf8'),
) as PanLawCorpus;

// The same corpus the WASM suite reads: the automation-curve vocabulary is one
// declaration both packages resolve against, not a table per surface.
const curveCorpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/automation_curve_names.json', import.meta.url),
    'utf8',
  ),
) as AutomationCurveCorpus;

describe('enum ordinal coercion', () => {
  it('accepts only declared spellings and refuses every number as a wrong type', () => {
    expect(resolveEnumOrdinal('a', { a: 0, b: 1 }, 'test enum')).toBe(0);
    for (const value of [1, 2, 0.5]) {
      expect(() => resolveEnumOrdinal(value, { a: 0, b: 1 }, 'test enum')).toThrow(TypeError);
    }
  });

  it('does not pass a raw ordinal through to the C ABI', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => meterTapValue(1 as any, 'tap')).toThrow(TypeError);
  });

  it('resolves exactly the shared pan-law spellings and refuses every other', () => {
    for (const { value, ordinal } of panLawCorpus.accepted) {
      expect(panLawValue(value as never, 'panLaw')).toBe(ordinal);
    }
    for (const value of panLawCorpus.rejected) {
      expect(() => panLawValue(value as never, 'panLaw')).toThrow(RangeError);
    }
    for (const value of panLawCorpus.rejected_numeric) {
      expect(() => panLawValue(value as never, 'panLaw')).toThrow(TypeError);
    }
  });

  it('resolves every shared automation-curve spelling', () => {
    // The tables the public types are derived from must hold exactly the shared
    // corpus, so widening one without the other is caught here rather than
    // surfacing as a curve one binding renders and the other rejects.
    expect(AUTOMATION_CURVE_VALUES).toEqual(
      Object.fromEntries(curveCorpus.accepted.map(({ value, ordinal }) => [value, ordinal])),
    );
    const curve = (value: unknown) =>
      automationPointValue(
        {
          ppq: 0,
          value: 1,
          curveToNext: value as ProjectAutomationCurve,
        },
        0,
      ).curveToNext;
    for (const { value, ordinal } of curveCorpus.accepted) {
      expect(curve(value)).toBe(ordinal);
    }
    // A Project curve is a token on every path, so an ordinal is a type error.
    for (const value of curveCorpus.rejected_numeric) {
      expect(() => curve(value)).toThrow(TypeError);
    }
    for (const value of curveCorpus.rejected) {
      expect(() => curve(value)).toThrow(RangeError);
    }
  });

  it('resolves curveToNext and refuses the curve key by name', () => {
    expect(automationPointValue({ ppq: 1, value: 0.5, curveToNext: 'sCurve' }, 0)).toEqual({
      ppq: 1,
      value: 0.5,
      curveToNext: 3,
    });
    // An omitted curve is Linear, matching the documented default.
    expect(automationPointValue({ ppq: 1, value: 0.5 }, 0)).toEqual({
      ppq: 1,
      value: 0.5,
      curveToNext: 0,
    });
    expect(() =>
      automationPointValue(
        {
          ppq: 1,
          value: 0.5,
          curve: 'hold',
        } as unknown as ProjectAutomationPoint,
        2,
      ),
    ).toThrow(new TypeError('unknown key points[2].curve (expected curveToNext, ppq, value)'));
  });
});

describe('resolveEnumOrdinal resolves declared names only', () => {
  const values = { off: 0, on: 1 };

  it('refuses names inherited from the prototype as a RangeError', () => {
    for (const name of ['toString', 'constructor', '__proto__', 'hasOwnProperty']) {
      expect(() => resolveEnumOrdinal(name, values, 'mode')).toThrow(RangeError);
    }
    expect(resolveEnumOrdinal('on', values, 'mode')).toBe(1);
  });

  it('refuses an unknown name as RangeError and a non-name as TypeError', () => {
    expect(() => resolveEnumOrdinal('nope', values, 'mode')).toThrow(RangeError);
    for (const bad of [null, undefined, true, {}, [], 0, 1]) {
      expect(() => resolveEnumOrdinal(bad, values, 'mode')).toThrow(TypeError);
    }
  });
});
