import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { EngineAutomationPointCurve, EngineTrackMonitorMode } from '../src/types_engine.js';
import type { PanLawInput } from '../src/types_mastering.js';
import type { ProjectAutomationCurve } from '../src/types_project.js';
import {
  AUTOMATION_CURVE_VALUES,
  engineAutomationCurveValue,
  meterTapValue,
  PROJECT_AUTOMATION_CURVE_VALUES,
  panLawValue,
  projectAutomationCurveValue,
  projectAutomationPointValue,
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

interface AutomationCurveCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  projectOnly: Array<{ value: string; ordinal: number }>;
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

  it('resolves every shared automation-curve spelling', () => {
    // The tables the public types are derived from must hold exactly the shared
    // corpus, so widening one without the other is caught here rather than
    // surfacing as a curve one binding renders and the other rejects.
    expect(AUTOMATION_CURVE_VALUES).toEqual(
      Object.fromEntries(curveCorpus.accepted.map(({ value, ordinal }) => [value, ordinal])),
    );
    expect(PROJECT_AUTOMATION_CURVE_VALUES).toEqual(
      Object.fromEntries(
        [...curveCorpus.accepted, ...curveCorpus.projectOnly].map(({ value, ordinal }) => [
          value,
          ordinal,
        ]),
      ),
    );
    for (const { value, ordinal } of curveCorpus.accepted) {
      expect(engineAutomationCurveValue(value as EngineAutomationPointCurve)).toBe(ordinal);
      expect(projectAutomationCurveValue(value as ProjectAutomationCurve)).toBe(ordinal);
    }
    for (const value of curveCorpus.numeric) {
      expect(projectAutomationCurveValue(value as ProjectAutomationCurve)).toBe(value);
    }
    for (const value of curveCorpus.rejected) {
      expect(() => projectAutomationCurveValue(value as ProjectAutomationCurve)).toThrow(
        RangeError,
      );
    }
  });

  it('accepts the legacy project curve spelling the WASM facade has always taken', () => {
    for (const { value, ordinal } of curveCorpus.projectOnly) {
      expect(projectAutomationCurveValue(value as ProjectAutomationCurve)).toBe(ordinal);
      expect(
        projectAutomationPointValue({ ppq: 0, value: 1, curve: value as ProjectAutomationCurve }),
      ).toEqual({ ppq: 0, value: 1, curve: ordinal, curveToNext: ordinal });
    }
  });

  it('folds the curveToNext alias onto curve, with curve winning', () => {
    // A breakpoint that carries only `curveToNext` must not fall back to Linear:
    // the type documents it as an alias and the Python marshaller reads it too.
    expect(projectAutomationPointValue({ ppq: 1, value: 0.5, curveToNext: 's-curve' })).toEqual({
      ppq: 1,
      value: 0.5,
      curve: 3,
      curveToNext: 3,
    });
    expect(
      projectAutomationPointValue({ ppq: 1, value: 0.5, curve: 'hold', curveToNext: 's-curve' }),
    ).toEqual({ ppq: 1, value: 0.5, curve: 2, curveToNext: 2 });
    // An omitted curve is Linear, matching the documented default.
    expect(projectAutomationPointValue({ ppq: 1, value: 0.5 })).toEqual({
      ppq: 1,
      value: 0.5,
      curve: 0,
      curveToNext: 0,
    });
  });
});
