/**
 * A typed buffer the WASM binding refuses is named by the facade's spelling.
 *
 * Every Float32Array / Int32Array / Uint8Array the embind glue copies goes through
 * one of common.h's typed-buffer helpers, which take the refusal's subject as a
 * required argument. The scan holds every call to a camelCase literal (or to a
 * name a helper was itself handed), so no refusal says "Float32Array length ..."
 * or a C parameter name. The driven cases show the wrong type is a TypeError with
 * the same text the Node addon gives.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project, RealtimeEngine } from '../dist/index.js';
import { wasmBindingSources } from './_wasm_binding_sources';

const HELPERS = [
  'float32ArrayToVector',
  'float32ArrayWindowToVector',
  'wasmFloat32ArrayLength',
  'wasmArrayLength',
  'wasmPlaneListLength',
  'wasmFloat32OutputLength',
  'int32ArrayToVector',
  'uint8ArrayToVector',
  'accumulateWasmFloat32ArrayLength',
  'validateWasmFloat32ArrayPair',
];

/**
 * Subjects that are not literals: a name the enclosing helper was handed by its own
 * caller, which the scan therefore checks at that caller instead.
 */
const FORWARDED = new Set<string>([
  'array_subject',
  'channel_name.c_str()',
  'data_name',
  'field',
  'first_subject',
  'kPath',
  'key',
  'left_name',
  'left_subject.c_str()',
  'name',
  'name.c_str()',
  'right_name',
  'right_subject.c_str()',
  'second_subject',
  'subject',
  'subject.c_str()',
]);

const LITERAL = /^"[a-z][A-Za-z0-9]*(\.[a-z][A-Za-z0-9]*)*"$/;

interface Call {
  file: string;
  helper: string;
  subject: string;
}

/** The second argument of each helper call, skipping the helpers' own declarations. */
function helperCalls(file: string, text: string): Call[] {
  const calls: Call[] = [];
  const pattern = new RegExp(`\\b(${HELPERS.join('|')})\\(`, 'g');
  for (const m of text.matchAll(pattern)) {
    const before = text.slice(Math.max(0, (m.index ?? 0) - 40), m.index);
    if (/(void|std::size_t|size_t|std::vector<\w+>)\s*$/.test(before)) {
      continue;
    }
    let depth = 1;
    let i = (m.index ?? 0) + m[0].length;
    const args: string[] = [];
    let current = '';
    for (; i < text.length && depth > 0; i += 1) {
      const c = text[i];
      if (c === '(') {
        depth += 1;
      }
      if (c === ')') {
        depth -= 1;
      }
      if (depth === 1 && c === ',') {
        args.push(current.trim());
        current = '';
      } else if (depth > 0) {
        current += c;
      }
    }
    args.push(current.trim());
    calls.push({ file, helper: m[1], subject: args[1] ?? '' });
    if (m[1] === 'validateWasmFloat32ArrayPair') {
      calls.push({ file, helper: m[1], subject: args[3] ?? '' });
    }
  }
  return calls;
}

const TOP_LEVEL = new URL('../../../src/wasm/bindings.cpp', import.meta.url).pathname;

const CALLS = [
  ...wasmBindingSources().flatMap((s) => helperCalls(s.file, s.text)),
  ...helperCalls('../bindings.cpp', readFileSync(TOP_LEVEL, 'utf8')),
];

describe('typed-buffer refusal subjects', () => {
  it('finds the helper calls', () => {
    expect(CALLS.length).toBeGreaterThan(200);
  });

  it('names every call by a camelCase facade spelling or a forwarded name', () => {
    const bad = CALLS.filter((c) => !LITERAL.test(c.subject) && !FORWARDED.has(c.subject)).map(
      (c) => `${c.file}: ${c.helper}(…, ${c.subject})`,
    );
    expect(bad).toEqual([]);
  });

  it('keeps every forwarded name in use', () => {
    const used = new Set(CALLS.map((c) => c.subject));
    expect([...FORWARDED].filter((name) => !used.has(name))).toEqual([]);
  });
});

describe('a wrong-typed buffer is a TypeError naming the facade spelling', () => {
  beforeAll(async () => {
    await init();
  });

  const WRONG: unknown[] = ['nope', 5, { length: 4 }, new Float64Array(4)];

  function expectTypeRefusal(call: () => unknown, text: string): void {
    let caught: unknown;
    try {
      call();
    } catch (error) {
      caught = error;
    }
    expect(caught, 'the call must throw').toBeInstanceOf(TypeError);
    expect((caught as Error).message).toContain(text);
  }

  it.each(WRONG.map((v) => [Object.prototype.toString.call(v), v] as const))(
    'Project.importSmf refuses %s as data',
    (_label, value) => {
      const project = new Project();
      try {
        expectTypeRefusal(() => project.importSmf(value as never), 'data must be a Uint8Array');
      } finally {
        project.destroy();
      }
    },
  );

  it.each(WRONG.map((v) => [Object.prototype.toString.call(v), v] as const))(
    'RealtimeEngine.pushMidiSysex refuses %s as data',
    (_label, value) => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        expectTypeRefusal(
          () => engine.pushMidiSysex(0, value as never),
          'data must be a Uint8Array',
        );
      } finally {
        engine.destroy();
      }
    },
  );

  it('accepts a plain number array where a typed array is expected', () => {
    const project = new Project();
    try {
      let caught: unknown;
      try {
        project.importSmf([0x4d, 0x54, 0x68, 0x64] as never);
      } catch (error) {
        caught = error;
      }
      expect(caught).not.toBeInstanceOf(TypeError);
    } finally {
      project.destroy();
    }
  });
});
