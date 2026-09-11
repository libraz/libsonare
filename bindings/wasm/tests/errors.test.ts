/**
 * WASM error-surface tests: native (C++) failures must reach JS as a
 * SonareError carrying name / numeric code / codeName, matching the C ABI.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, SonareError, synthPresetPatch } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('SonareError', () => {
  it('exposes an ErrorCode enum aligned with the C ABI', () => {
    expect(ErrorCode.Ok).toBe(0);
    expect(ErrorCode.FileNotFound).toBe(1);
    expect(ErrorCode.InvalidParameter).toBe(4);
    expect(ErrorCode.NotSupported).toBe(6);
    expect(ErrorCode.InvalidState).toBe(7);
    expect(ErrorCode.Unknown).toBe(99);
  });

  it('rethrows a native C++ exception as a coded SonareError', () => {
    let caught: unknown;
    try {
      synthPresetPatch('definitely-not-a-real-preset');
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect(isSonareError(caught)).toBe(true);

    const err = caught as SonareError;
    expect(err.name).toBe('SonareError');
    expect(err.code).toBe(ErrorCode.InvalidParameter);
    expect(err.codeName).toBe('InvalidParameter');
    // The native detail message survives the pointer round-trip.
    expect(err.message).toContain('preset');
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
 * The exception decoder is a hand-written switch over `sonare::ErrorCode`, and
 * every enumerator it omits reaches JS as Unknown/99 with the true cause lost.
 * Nothing can drive that from a test: the one enumerator that was missing,
 * `EncodeFailed`, is thrown only by the core's file-writing paths, which this
 * surface has no filesystem to reach. So the mapping is asserted against the
 * enum it claims to mirror instead, which is what a behavioural test could not
 * have caught in the first place.
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

/** The body of `js_sonare_exception_info`'s switch, in `src/wasm/bindings.cpp`. */
function wasmExceptionSwitchBody(): string {
  const text = repoFile('src/wasm/bindings.cpp');
  const fn = text.indexOf('val js_sonare_exception_info(');
  expect(fn).toBeGreaterThan(-1);
  const start = text.indexOf('switch (se->code()) {', fn);
  expect(start).toBeGreaterThan(-1);
  // The arms carry no nested braces, so the first `}` back at the switch's own
  // indentation closes it.
  const end = text.indexOf('\n      }', start);
  expect(end).toBeGreaterThan(start);
  return text.slice(start, end);
}

/** `{ Enumerator: { code, codeName } }` as the decoder actually spells them. */
function wasmExceptionArms(): Map<string, { code: number; codeName: string }> {
  const arms = new Map<string, { code: number; codeName: string }>();
  const pattern =
    /case sonare::ErrorCode::(\w+):\s*\n\s*code = (\d+);\s*\n\s*code_name = "(\w+)";/g;
  for (const match of wasmExceptionSwitchBody().matchAll(pattern)) {
    expect(arms.has(match[1])).toBe(false); // one arm per enumerator, not two
    arms.set(match[1], { code: Number(match[2]), codeName: match[3] });
  }
  return arms;
}

/** `{ Name: ordinal }` from a TypeScript `export enum ErrorCode { ... }`. */
function tsErrorCodeEnum(relative: string): Map<string, number> {
  const body = captureOne(
    repoFile(relative),
    /export enum ErrorCode \{([\s\S]*?)\n\}/,
    `export enum ErrorCode in ${relative}`,
  );
  const out = new Map<string, number>();
  for (const match of body.matchAll(/^\s*(\w+)\s*=\s*(\d+)\s*,/gm)) {
    out.set(match[1], Number(match[2]));
  }
  return out;
}

/** SONARE_ERROR_FILE_NOT_FOUND / FILE_NOT_FOUND -> FileNotFound. */
const toPascalCase = (name: string): string =>
  name
    .replace(/^SONARE_(ERROR_)?/, '')
    .toLowerCase()
    .replace(/_(\w)/g, (_, c: string) => c.toUpperCase())
    .replace(/^(\w)/, (_, c: string) => c.toUpperCase());

describe('the WASM exception decoder covers every core ErrorCode', () => {
  it('reads the enum and the decoder it mirrors', () => {
    // Self-check: every assertion below is vacuous if a regex stopped matching,
    // so pin what the scanners found before trusting them.
    const enumerators = coreErrorCodeEnumerators();
    expect(enumerators[0]).toBe('Ok');
    expect(enumerators).toContain('EncodeFailed');
    expect(enumerators.length).toBeGreaterThanOrEqual(10);
    expect(wasmExceptionArms().size).toBe(enumerators.length);
    expect(tsErrorCodeEnum('bindings/wasm/src/errors.ts').get('Unknown')).toBe(99);
  });

  it('maps every enumerator exactly once, with no default: to hide the next one', () => {
    const enumerators = coreErrorCodeEnumerators();
    const arms = wasmExceptionArms();
    expect([...arms.keys()].sort()).toEqual([...enumerators].sort());
    // A default: would satisfy the count above while still mapping a future
    // enumerator to Unknown, which is the failure this is here to prevent.
    expect(wasmExceptionSwitchBody()).not.toMatch(/\bdefault\s*:/);
  });

  it('gives each enumerator the ordinal and codeName the TS enum publishes', () => {
    const enumerators = coreErrorCodeEnumerators();
    const arms = wasmExceptionArms();
    const published = tsErrorCodeEnum('bindings/wasm/src/errors.ts');
    const nameForOrdinal = new Map([...published].map(([name, value]) => [value, name]));
    enumerators.forEach((enumerator, ordinal) => {
      const arm = arms.get(enumerator);
      expect(arm, enumerator).toBeDefined();
      // The core's declaration order IS the C ABI's numbering, so an arm that
      // reports a different number has silently renumbered the surface.
      expect(arm?.code, enumerator).toBe(ordinal);
      expect(arm?.codeName, enumerator).toBe(nameForOrdinal.get(ordinal));
    });
  });

  it('publishes one ErrorCode table across the C ABI, Node, WASM and Python', () => {
    const cBody = captureOne(
      repoFile('include/sonare/sonare_c_types_enums.h'),
      /typedef enum SONARE_ENUM_BASE \{([\s\S]*?)\n\} SonareError;/,
      'the SonareError enum',
    );
    const cTable = new Map<string, number>();
    for (const match of cBody.matchAll(/^\s*(SONARE_\w+)\s*=\s*(\d+)/gm)) {
      cTable.set(toPascalCase(match[1]), Number(match[2]));
    }

    const pyBody = captureOne(
      repoFile('bindings/python/src/libsonare/_runtime.py'),
      /class ErrorCode\(IntEnum\):([\s\S]*?)\n\n\nclass /,
      'class ErrorCode in the Python runtime',
    );
    const pyTable = new Map<string, number>();
    for (const match of pyBody.matchAll(/^ {4}([A-Z][A-Z0-9_]*)\s*=\s*(\d+)$/gm)) {
      pyTable.set(toPascalCase(match[1]), Number(match[2]));
    }

    const sorted = (table: Map<string, number>): [string, number][] =>
      [...table].sort(([a], [b]) => a.localeCompare(b));
    const expected = sorted(cTable);
    expect(expected.length).toBe(coreErrorCodeEnumerators().length + 1); // + Unknown
    expect(sorted(tsErrorCodeEnum('bindings/wasm/src/errors.ts'))).toEqual(expected);
    expect(sorted(tsErrorCodeEnum('bindings/node/src/errors.ts'))).toEqual(expected);
    expect(sorted(pyTable)).toEqual(expected);
  });
});

/** One `throw sonare::SonareException(ErrorCode::X, ...)` in a binding source. */
interface StubThrow {
  file: string;
  line: number;
  code: string;
}

/**
 * Every `SonareException` thrown from the DISABLED half of a `SONARE_WITH_*`
 * guard, found by walking `#if` / `#else` / `#endif` nesting rather than by
 * matching message text.
 *
 * The nesting walk is the point. A throw in the ENABLED half is ordinary
 * validation and is none of this rule's business, and no textual pattern can
 * tell the two apart — which is exactly how 13 of these hid: a grep anchored on
 * "support is not enabled" was blind to the sites spelled "is not available in
 * this build", so the count looked like 19 when it was 32.
 */
function compiledOutFeatureStubs(): StubThrow[] {
  const root = new URL('../../../src/wasm/', import.meta.url).pathname;
  const out: StubThrow[] = [];
  const walk = (dir: string): void => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const full = join(dir, entry.name);
      if (entry.isDirectory()) {
        walk(full);
      } else if (entry.name.endsWith('.cpp') || entry.name.endsWith('.h')) {
        scan(full);
      }
    }
  };
  const scan = (path: string): void => {
    const lines = readFileSync(path, 'utf8').split('\n');
    // Each frame is [guards a SONARE_WITH_* feature, currently in the disabled half].
    const stack: [boolean, boolean][] = [];
    lines.forEach((raw, index) => {
      const line = raw.trim();
      if (line.startsWith('#if')) {
        const feature = /SONARE_WITH_\w+/.test(line);
        // `#if !defined(X)` / `#ifndef X` put the stub in the THEN half.
        const negated =
          /^#if\s+!\s*defined\(SONARE_WITH_/.test(line) || /^#ifndef\s+SONARE_WITH_/.test(line);
        stack.push([feature, negated]);
      } else if (line.startsWith('#else') && stack.length > 0) {
        const top = stack[stack.length - 1];
        top[1] = !top[1];
      } else if (line.startsWith('#endif')) {
        stack.pop();
      } else if (line.includes('SonareException') && stack.some(([f, d]) => f && d)) {
        const code = lines
          .slice(index, index + 3)
          .join('\n')
          .match(/ErrorCode::(\w+)/)?.[1];
        if (code !== undefined) {
          out.push({ file: path.slice(root.length), line: index + 1, code });
        }
      }
    });
  };
  walk(root);
  return out;
}

describe('a compiled-out WASM feature reports NotSupported, not InvalidState', () => {
  it('finds the stub throws at all, across more than one file', () => {
    // Self-check: the assertion below passes vacuously if the nesting walk stops
    // matching, so pin that it still finds stubs in several translation units.
    const stubs = compiledOutFeatureStubs();
    expect(stubs.length).toBeGreaterThanOrEqual(36);
    expect(new Set(stubs.map((stub) => stub.file)).size).toBeGreaterThanOrEqual(4);
  });

  it('throws NotImplemented from every one of them', () => {
    // The C ABI answers SONARE_ERROR_NOT_SUPPORTED from every one of its own
    // disabled-feature branches, and ErrorCode::NotImplemented is the enumerator
    // that reaches JS as code 6 / 'NotSupported'. InvalidState (7) is a
    // different, recoverable condition and tells the caller the wrong thing.
    const wrong = compiledOutFeatureStubs().filter((stub) => stub.code !== 'NotImplemented');
    expect(wrong).toEqual([]);
  });
});
