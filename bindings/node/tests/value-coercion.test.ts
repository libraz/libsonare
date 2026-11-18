import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { ProjectAutomationCurve } from '../src/types_project.js';
import {
  AUTOMATION_CURVE_VALUES,
  meterTapValue,
  panLawValue,
  projectAutomationPointValue,
  resolveEnumOrdinal,
} from '../src/value_coercion.js';

interface PanLawCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  normalization: Array<{ value: string; ordinal: number }>;
  numeric: number[];
  rejected: string[];
}

interface AutomationCurveCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  numeric: number[];
  rejected: string[];
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
  it('accepts only declared ordinals and spellings', () => {
    expect(resolveEnumOrdinal('a', { a: 0, b: 1 }, 'test enum')).toBe(0);
    expect(resolveEnumOrdinal(1, { a: 0, b: 1 }, 'test enum')).toBe(1);
    expect(() => resolveEnumOrdinal(2, { a: 0, b: 1 }, 'test enum')).toThrow(RangeError);
    expect(() => resolveEnumOrdinal(0.5, { a: 0, b: 1 }, 'test enum')).toThrow(RangeError);
  });

  it('does not pass unknown values through to the C ABI', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercise direct JavaScript callers.
    expect(() => meterTapValue(2 as any)).toThrow(RangeError);
  });

  it('resolves every shared pan-law spelling and conservative normalization', () => {
    for (const { value, ordinal } of [...panLawCorpus.accepted, ...panLawCorpus.normalization]) {
      expect(panLawValue(value as never)).toBe(ordinal);
    }
    for (const value of panLawCorpus.numeric) {
      expect(panLawValue(value)).toBe(value);
    }
    for (const value of panLawCorpus.rejected) {
      expect(() => panLawValue(value as never)).toThrow(RangeError);
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
      projectAutomationPointValue(
        { ppq: 0, value: 1, curve: value as ProjectAutomationCurve },
        'addAutomationLane',
      ).curve;
    for (const { value, ordinal } of curveCorpus.accepted) {
      expect(curve(value)).toBe(ordinal);
    }
    // A Project curve is a token on every path, so an ordinal is a type error.
    for (const value of curveCorpus.numeric) {
      expect(() => curve(value)).toThrow(TypeError);
    }
    for (const value of curveCorpus.rejected) {
      expect(() => curve(value)).toThrow(RangeError);
    }
  });

  it('folds the curveToNext alias onto curve, with curve winning', () => {
    // A breakpoint that carries only `curveToNext` must not fall back to Linear:
    // the type documents it as an alias and the Python marshaller reads it too.
    expect(
      projectAutomationPointValue(
        { ppq: 1, value: 0.5, curveToNext: 'sCurve' },
        'addAutomationLane',
      ),
    ).toEqual({
      ppq: 1,
      value: 0.5,
      curve: 3,
      curveToNext: 3,
    });
    expect(
      projectAutomationPointValue(
        { ppq: 1, value: 0.5, curve: 'hold', curveToNext: 'sCurve' },
        'addAutomationLane',
      ),
    ).toEqual({ ppq: 1, value: 0.5, curve: 2, curveToNext: 2 });
    // An omitted curve is Linear, matching the documented default.
    expect(projectAutomationPointValue({ ppq: 1, value: 0.5 }, 'addAutomationLane')).toEqual({
      ppq: 1,
      value: 0.5,
      curve: 0,
      curveToNext: 0,
    });
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

  it('refuses an unknown name as RangeError and a non-name, non-ordinal as TypeError', () => {
    expect(() => resolveEnumOrdinal('nope', values, 'mode')).toThrow(RangeError);
    for (const bad of [null, undefined, true, {}, []]) {
      expect(() => resolveEnumOrdinal(bad, values, 'mode')).toThrow(TypeError);
    }
  });
});
