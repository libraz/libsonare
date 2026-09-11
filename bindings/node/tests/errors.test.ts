import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeAsync,
  ErrorCode,
  isSonareError,
  mixStereo,
  SonareError,
  synthPresetPatch,
} from '../src/index.js';

describe('SonareError', () => {
  it('exposes an ErrorCode enum aligned with the C ABI', () => {
    expect(ErrorCode.Ok).toBe(0);
    expect(ErrorCode.FileNotFound).toBe(1);
    expect(ErrorCode.InvalidParameter).toBe(4);
    expect(ErrorCode.NotSupported).toBe(6);
    expect(ErrorCode.InvalidState).toBe(7);
    expect(ErrorCode.Cancelled).toBe(8);
    expect(ErrorCode.Unknown).toBe(99);
  });

  it('carries name, numeric code, and codeName on a C-ABI failure', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(Error);
    expect(isSonareError(caught)).toBe(true);

    const err = caught as SonareError;
    expect(err.name).toBe('SonareError');
    expect(err.code).toBe(ErrorCode.InvalidParameter);
    expect(err.codeName).toBe('InvalidParameter');
    expect(typeof err.message).toBe('string');
    expect(err.message.length).toBeGreaterThan(0);
  });
});

/**
 * errors.ts promises that EVERY C-ABI failure surfaces as a SonareError with
 * the numeric C ordinal, so the same failure reports the same code on every
 * path and every surface. A path that throws a bare Error breaks that promise
 * silently — the caller's `error.code` is simply `undefined`.
 */
describe('every C-ABI failure carries the SonareError code', () => {
  const sine = (n: number): Float32Array =>
    new Float32Array(n).map((_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / 22050));

  const capture = (run: () => unknown): unknown => {
    try {
      run();
      return undefined;
    } catch (error) {
      return error;
    }
  };

  const cases: ReadonlyArray<[string, () => unknown, ErrorCode]> = [
    ['analyze (sync)', () => analyze(sine(2048), 0), ErrorCode.InvalidParameter],
    [
      'mixStereo',
      () => {
        const channel = sine(256);
        return mixStereo([channel], [channel], 0);
      },
      ErrorCode.InvalidParameter,
    ],
    [
      'synthPresetPatch',
      () => synthPresetPatch('definitely-not-a-real-preset'),
      ErrorCode.InvalidParameter,
    ],
  ];

  for (const [label, run, expected] of cases) {
    it(`${label} throws a SonareError with code ${expected}`, () => {
      const caught = capture(run);
      expect(caught, `${label} should have thrown`).toBeDefined();
      expect(isSonareError(caught), `${label} threw a bare Error, not a SonareError`).toBe(true);
      expect((caught as SonareError).code).toBe(expected);
      expect((caught as SonareError).name).toBe('SonareError');
    });
  }

  it('reports the same code for analyze and analyzeAsync on the same input', async () => {
    const samples = sine(2048);
    const sync = capture(() => analyze(samples, 0));
    let async: unknown;
    try {
      await analyzeAsync(samples, 0);
    } catch (error) {
      async = error;
    }
    expect(isSonareError(sync)).toBe(true);
    expect(isSonareError(async)).toBe(true);
    expect((sync as SonareError).code).toBe((async as SonareError).code);
    expect((sync as SonareError).codeName).toBe((async as SonareError).codeName);
  });
});

// `SonareError` used to be a runtime class in the WASM package and a type-only
// interface in the Node package, both re-exported from the index under the same
// name. Importing it in a module shared between the two therefore resolved to
// `undefined` at runtime on one side, and `instanceof` was a compile error
// there. Both packages now export the same value class.
describe('SonareError is a value class with brand-based instanceof', () => {
  it('is a constructible runtime value, not a type-only name', () => {
    expect(typeof SonareError).toBe('function');
    const built = new SonareError(ErrorCode.InvalidParameter, 'InvalidParameter', 'built by hand');
    expect(built).toBeInstanceOf(Error);
    expect(built.name).toBe('SonareError');
    expect(built.code).toBe(4);
    expect(built.codeName).toBe('InvalidParameter');
  });

  it('narrows a native failure through instanceof as well as isSonareError', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(isSonareError(caught)).toBe(true);
    // The two must never disagree: instanceof delegates to the same predicate.
    expect(caught instanceof SonareError).toBe(true);
  });

  it('still narrows an error that lost its prototype crossing a boundary', () => {
    // What a structured clone leaves behind: the shape, not the prototype.
    const cloned = Object.assign(new Error('cloned'), {
      name: 'SonareError',
      code: ErrorCode.InvalidState,
      codeName: 'InvalidState',
    });
    expect(Object.getPrototypeOf(cloned)).toBe(Error.prototype);
    expect(cloned instanceof SonareError).toBe(true);
    expect(isSonareError(cloned)).toBe(true);
  });

  it('rejects a plain Error and a look-alike without a numeric code', () => {
    expect(new Error('plain') instanceof SonareError).toBe(false);
    const noCode = Object.assign(new Error('x'), { name: 'SonareError', codeName: 'Unknown' });
    expect(noCode instanceof SonareError).toBe(false);
    expect(isSonareError(noCode)).toBe(false);
  });
});

/**
 * `CErrorFromException` is the addon's hand-written switch from a core
 * `sonare::ErrorCode` to the C-ABI code the caller sees, and it duplicates the
 * C ABI's own `map_sonare_exception`. An enumerator missing from it reaches JS
 * as Unknown/99 with the cause lost, and no test can drive that: the enumerator
 * that was missing, `EncodeFailed`, is thrown only by the core's file-writing
 * paths, and this addon exposes no entry point that writes a file. So the
 * switch is asserted against the enum it mirrors, and against the C-ABI table
 * that is its oracle.
 */
const repoFile = (relative: string): string =>
  readFileSync(new URL(`../../../${relative}`, import.meta.url).pathname, 'utf8');

/** Captures a single required group, failing loudly when the shape moved. */
function captureOne(text: string, pattern: RegExp, what: string): string {
  const found = text.match(pattern);
  if (found === null) {
    throw new Error(`could not locate ${what}`);
  }
  return found[1];
}

/** Enumerator names of `sonare::ErrorCode`, in declaration order. */
function coreErrorCodeEnumerators(): string[] {
  const body = captureOne(
    repoFile('src/util/types.h'),
    /enum class ErrorCode : int \{([\s\S]*?)\n\};/,
    'enum class ErrorCode in src/util/types.h',
  );
  return body
    .split('\n')
    .map((line) => line.replace(/\/\/.*$/, '').trim())
    .map((line) => line.match(/^([A-Za-z_]\w*)\s*(?:=\s*\d+)?\s*,?$/)?.[1])
    .filter((name): name is string => name !== undefined);
}

/** `{ Enumerator: SONARE_ERROR_* }` from a `switch (e.code())` / `switch (err)`. */
function switchArms(text: string, signature: string): Map<string, string> {
  const start = text.indexOf(signature);
  expect(start, signature).toBeGreaterThan(-1);
  const open = text.indexOf('switch (', start);
  expect(open).toBeGreaterThan(-1);
  const end = text.indexOf('\n  }', open);
  expect(end).toBeGreaterThan(open);
  const body = text.slice(open, end);
  const arms = new Map<string, string>();
  for (const match of body.matchAll(/case sonare::ErrorCode::(\w+):\s*\n\s*return (\w+);/g)) {
    expect(arms.has(match[1]), match[1]).toBe(false); // one arm per enumerator
    arms.set(match[1], match[2]);
  }
  return arms;
}

const addonArms = (): Map<string, string> =>
  switchArms(
    repoFile('bindings/node/src/addon/sonare_wrap_utils.cpp'),
    'SonareError CErrorFromException(',
  );

/** The C ABI's own exception map, which the addon's copy must agree with. */
const oracleArms = (): Map<string, string> =>
  switchArms(repoFile('src/c_api/sonare_c_internal.cpp'), 'SonareError map_sonare_exception(');

describe('the addon exception map covers every core ErrorCode', () => {
  it('reads the enum and both switches it has to agree with', () => {
    // Self-check: every assertion below is vacuous if a regex stopped matching.
    const enumerators = coreErrorCodeEnumerators();
    expect(enumerators[0]).toBe('Ok');
    expect(enumerators).toContain('EncodeFailed');
    expect(addonArms().size).toBe(enumerators.length);
    expect(oracleArms().get('EncodeFailed')).toBe('SONARE_ERROR_ENCODE_FAILED');
  });

  it('maps every enumerator exactly once, with no default: to hide the next one', () => {
    const enumerators = coreErrorCodeEnumerators();
    expect([...addonArms().keys()].sort()).toEqual([...enumerators].sort());
    // A default: would satisfy the count above while still sending a future
    // enumerator to SONARE_ERROR_UNKNOWN, which is the failure this prevents.
    const text = repoFile('bindings/node/src/addon/sonare_wrap_utils.cpp');
    const start = text.indexOf('SonareError CErrorFromException(');
    expect(text.slice(start, text.indexOf('\n}', start))).not.toMatch(/\bdefault\s*:/);
  });

  it('agrees with the C ABI oracle on every enumerator the oracle maps', () => {
    const addon = addonArms();
    const oracle = oracleArms();
    for (const [enumerator, code] of oracle) {
      expect(addon.get(enumerator), enumerator).toBe(code);
    }
    // `Ok` is not an error, so the oracle leaves it to its default arm; the
    // addon spells it out because it carries no default.
    expect(oracle.has('Ok')).toBe(false);
    expect(addon.get('Ok')).toBe('SONARE_ERROR_UNKNOWN');
  });
});
