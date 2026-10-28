import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  analyze,
  analyzeAsync,
  ErrorCode,
  isSonareError,
  SonareError,
  synthPresetPatch,
} from '../src/index.js';
import { addonSources, addonSourceText } from './_addon_sources.js';

/** A failure that is the library's own, not a refused argument: a released handle. */
function destroyedAudioFailure(): void {
  const audio = Audio.fromBuffer(new Float32Array(2205), 22050);
  audio.destroy();
  audio.rmsDb();
}

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

  it('carries name, numeric code, and codeName on a library failure', () => {
    let caught: unknown;
    try {
      destroyedAudioFailure();
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(Error);
    expect(isSonareError(caught)).toBe(true);

    const err = caught as SonareError;
    expect(err.name).toBe('SonareError');
    expect(err.code).toBe(ErrorCode.InvalidState);
    expect(err.codeName).toBe('InvalidState');
    expect(typeof err.message).toBe('string');
    expect(err.message.length).toBeGreaterThan(0);
  });
});

/**
 * The failure class follows the cause: a library or object-state failure is a
 * SonareError carrying the numeric C ordinal, and a refused caller argument (the
 * C ABI's InvalidParameter) is a RangeError on every path, sync and async.
 */
describe('failure class follows the cause', () => {
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

  // Each refusal has to come from INSIDE the core. Empty or non-finite audio and
  // a wrong sample rate no longer do: the facade refuses them on its own
  // authority. These reach the core with arguments the facade has nothing to say about.
  const badHop = { samples: sine(22050), sampleRate: 22050, hopLength: 0 };
  const refusals: ReadonlyArray<[string, () => unknown]> = [
    ['analyze (sync)', () => analyze(badHop)],
    ['synthPresetPatch', () => synthPresetPatch('definitely-not-a-real-preset')],
  ];

  for (const [label, run] of refusals) {
    it(`${label} refuses its argument with a plain RangeError`, () => {
      const caught = capture(run);
      expect(caught, `${label} should have thrown`).toBeDefined();
      expect(caught).toBeInstanceOf(RangeError);
      expect(isSonareError(caught), `${label} threw a coded SonareError`).toBe(false);
      expect((caught as Error).message.length).toBeGreaterThan(0);
    });
  }

  it('reports a released handle as a SonareError with InvalidState', () => {
    const caught = capture(destroyedAudioFailure);
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
    expect((caught as SonareError).name).toBe('SonareError');
  });

  it('reports the same class for analyze and analyzeAsync on the same input', async () => {
    const sync = capture(() => analyze(badHop));
    let async: unknown;
    try {
      await analyzeAsync(badHop);
    } catch (error) {
      async = error;
    }
    expect(sync).toBeInstanceOf(RangeError);
    expect(async).toBeInstanceOf(RangeError);
    expect(isSonareError(async)).toBe(false);
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
      destroyedAudioFailure();
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
 * A file in the CORE tree. Addon sources go through `addonSourceText` instead,
 * so `src/addon/` keeps one definition across the test tree.
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

/** `{ Enumerator: SONARE_ERROR_* }` from a `switch (ErrorCode)`. */
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

const canonicalArms = (): Map<string, string> =>
  switchArms(repoFile('src/c_api/sonare_c_error_mapping.h'), 'c_error_from_error_code(');

function delegatesToCanonical(text: string, signature: string): boolean {
  const start = text.indexOf(signature);
  expect(start, signature).toBeGreaterThan(-1);
  const body = text.slice(start, text.indexOf('\n}', start));
  return /return\s+(?:sonare_c_detail::)?c_error_from_error_code\(e\.code\(\)\);/.test(body);
}

const canonicalSwitchText = (): string => {
  const text = repoFile('src/c_api/sonare_c_error_mapping.h');
  const start = text.indexOf('c_error_from_error_code(');
  const open = text.indexOf('switch (', start);
  const end = text.indexOf('\n  }', open);
  expect(start).toBeGreaterThan(-1);
  expect(open).toBeGreaterThan(start);
  expect(end).toBeGreaterThan(open);
  return text.slice(open, end);
};

// Checked statically: EncodeFailed cannot be reached from any addon entry
// point, so no call through the addon can exercise its mapping.
describe('the canonical exception map covers every core ErrorCode', () => {
  it('reads the enum and the canonical switch', () => {
    // Self-check: every assertion below is vacuous if a regex stopped matching.
    const enumerators = coreErrorCodeEnumerators();
    expect(enumerators[0]).toBe('Ok');
    expect(enumerators).toContain('EncodeFailed');
    expect(canonicalArms().size).toBe(enumerators.length);
    expect(canonicalArms().get('EncodeFailed')).toBe('SONARE_ERROR_ENCODE_FAILED');
  });

  it('maps every enumerator exactly once, with no default: to hide the next one', () => {
    const enumerators = coreErrorCodeEnumerators();
    expect([...canonicalArms().keys()].sort()).toEqual([...enumerators].sort());
    // A default: would satisfy the count above while still sending a future
    // enumerator to SONARE_ERROR_UNKNOWN, which is the failure this prevents.
    expect(canonicalSwitchText()).not.toMatch(/\bdefault\s*:/);
  });

  it('spells out Ok as Unknown', () => {
    // `Ok` is not an error, and the canonical switch spells it out rather than
    // leaving it to a default arm: an exception carrying it is a programming
    // error, so it is reported as Unknown instead of as the success its ordinal
    // would mean.
    expect(canonicalArms().get('Ok')).toBe('SONARE_ERROR_UNKNOWN');
  });

  it('keeps the C and Node wrappers as delegates', () => {
    expect(
      delegatesToCanonical(
        repoFile('src/c_api/sonare_c_internal.cpp'),
        'SonareError map_sonare_exception(',
      ),
    ).toBe(true);
    expect(
      delegatesToCanonical(
        addonSourceText('sonare_wrap_utils.cpp'),
        'SonareError CErrorFromException(',
      ),
    ).toBe(true);
  });
});

/**
 * A raw std::bad_alloc/invalid_argument/logic_error escaping the core (rather
 * than a sonare::SonareException) used to reach SONARE_NODE_CATCH's generic
 * catch(std::exception) arm as a bare Napi::Error with no code property, while
 * the C ABI's own catch chain (SONARE_C_CATCH) classifies the same exception
 * types to OutOfMemory/InvalidParameter/InvalidState. Both catch macros now
 * delegate that classification to the single shared
 * sonare::error_code_for_std_exception (src/util/error_classification.h),
 * which the WASM exception-introspection switch also calls -- see
 * bindings/wasm/tests/errors.test.ts for that half.
 */
describe('SONARE_NODE_CATCH classifies a raw std::exception through the shared classifier', () => {
  it('routes each catch macro through the one translator, which classifies once', () => {
    const text = addonSourceText('sonare_wrap_utils.h');
    const nodeCatch = text.slice(text.indexOf('#define SONARE_NODE_CATCH('));
    const tryCatch = nodeCatch.slice(0, nodeCatch.indexOf('#define SONARE_NODE_CATCH_VOID('));
    const voidCatch = nodeCatch.slice(nodeCatch.indexOf('#define SONARE_NODE_CATCH_VOID('));
    const hits = (s: string) => (s.match(/ThrowCurrentException/g) ?? []).length;
    expect(hits(tryCatch)).toBe(1);
    expect(hits(voidCatch)).toBe(1);
    const helper = addonSourceText('sonare_wrap_utils.cpp');
    expect((helper.match(/error_code_for_std_exception/g) ?? []).length).toBe(1);
  });

  it('no addon file outside the translator catches std::exception to re-derive a code', () => {
    const offenders = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_utils.cpp')
      .filter((source) =>
        /catch\s*\(\s*const\s+(sonare::SonareException|std::exception)\s*&/.test(
          source.text.replace(/\/\/.*$/gm, ''),
        ),
      )
      .map((source) => source.file);
    // The mastering worker's Execute() runs off the JS thread and records the code for OnOK.
    expect(offenders).toEqual(['effects/mastering.cpp']);
  });

  it('no longer special-cases std::bad_alloc separately from other std::exception', () => {
    // The special case was the defect: it left invalid_argument/logic_error on
    // the generic arm with no code property. Merging into one classified arm
    // is the fix, so a reintroduced bad_alloc-only catch is a regression.
    const text = addonSourceText('sonare_wrap_utils.h');
    expect(text).not.toMatch(/catch\s*\(\s*const\s+std::bad_alloc\s*&?\s*\)/);
  });
});
