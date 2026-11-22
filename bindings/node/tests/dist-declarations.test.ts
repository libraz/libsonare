/**
 * A const the main entry's declarations reach is a value a consumer can import.
 *
 * A type documented against a token list (`SoloProcessor`, "see `SOLO_PROCESSORS`")
 * is only half-published when the list itself never reaches the entry's runtime
 * exports, and nothing else turns that red.
 */

import { readFile } from 'node:fs/promises';
import { join, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const DIST = fileURLToPath(new URL('../dist/', import.meta.url));
const SPECIFIER = /\b(?:from|import)\s*\(?\s*['"]([^'"]+)['"]/g;

/**
 * Modules whose declared consts reach the main entry's declarations only through a
 * type import, each with the reason none of them is a public value there.
 */
const NOT_PUBLIC_VALUES: Readonly<Record<string, string>> = {
  'realtime_engine.d.ts': 'the engine ABI mirror the module-load check reads',
  'request_keys.d.ts': 'the request-shape tables behind the key refusals',
  'types_project.d.ts': 'the project ABI mirror the module-load check reads',
  'validation.d.ts': 'domain bounds behind the argument refusals',
};

/** Every declaration file the entry's declarations reach, keyed by its path under dist/. */
async function declarationClosure(entry: string): Promise<Map<string, string>> {
  const reached = new Map<string, string>();
  const pending = [entry];
  while (pending.length > 0) {
    const file = pending.pop() as string;
    if (reached.has(file)) continue;
    const text = await readFile(join(DIST, file), 'utf8');
    reached.set(file, text);
    for (const [, specifier] of text.matchAll(SPECIFIER)) {
      if (!specifier.startsWith('.')) continue;
      const target = join(file, '..', specifier).split(sep).join('/');
      pending.push(target.replace(/\.js$/, '.d.ts'));
    }
  }
  return reached;
}

describe('a const the main entry declares', () => {
  it('is a runtime export of that entry unless its module is listed as not public', async () => {
    const runtime = new Set(Object.keys(await import('../dist/index.js')));
    const closure = await declarationClosure('index.d.ts');
    expect(closure.size).toBeGreaterThan(1);
    const unexported: string[] = [];
    const excusing = new Set<string>();
    for (const [file, text] of closure) {
      for (const [, name] of text.matchAll(/^export declare const (\w+)/gm)) {
        if (runtime.has(name)) continue;
        if (file in NOT_PUBLIC_VALUES) excusing.add(file);
        else unexported.push(`${file}: ${name}`);
      }
    }
    expect(unexported).toEqual([]);
    expect(Object.keys(NOT_PUBLIC_VALUES).filter((file) => !excusing.has(file))).toEqual([]);
  });
});
