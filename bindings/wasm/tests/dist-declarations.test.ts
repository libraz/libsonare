/**
 * The declaration tree in `dist/` resolves without `src/` beside it.
 *
 * A host that vendors `dist/` alone has nothing at `../src/`, and under
 * `skipLibCheck` an unresolved specifier degrades every type behind it to
 * `any` without an error, so an escaping specifier is refused here instead.
 */

import { readdir, readFile } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const DIST = fileURLToPath(new URL('../dist/', import.meta.url));
const SPECIFIER = /\b(?:from|import)\s*\(?\s*['"]([^'"]+)['"]/g;

async function declarations(directory: string): Promise<string[]> {
  const found: string[] = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      found.push(...(await declarations(path)));
    } else if (entry.name.endsWith('.d.ts')) {
      found.push(path);
    }
  }
  return found;
}

describe('dist declarations', () => {
  it('reference nothing outside dist/', async () => {
    const files = await declarations(DIST);
    expect(files.length).toBeGreaterThan(0);
    const escaping: string[] = [];
    for (const file of files) {
      const fromDist = relative(DIST, file).split(sep).join('/');
      const depth = fromDist.split('/').length - 1;
      for (const [, specifier] of (await readFile(file, 'utf8')).matchAll(SPECIFIER)) {
        const ups = specifier.match(/^(?:\.\.\/)+/)?.[0].length ?? 0;
        if (ups / 3 > depth) {
          escaping.push(`${fromDist}: ${specifier}`);
        }
      }
    }
    expect(escaping).toEqual([]);
  });
});

/**
 * Modules whose declared consts reach the main entry's declarations only through a
 * type import, each with the reason none of them is a public value there.
 */
const NOT_PUBLIC_VALUES: Readonly<Record<string, string>> = {
  'internal/engine_command_ring.d.ts': 'engine command ring layout, private to the facade',
  'internal/worklet_messages.d.ts': 'worklet message defaults, private to the facade',
  'opfs_clip_pages.d.ts': 'the worker source is a value of the worklet entry',
  'project_types.d.ts': 'the project ABI mirror the module-load check reads',
  'realtime_engine.d.ts': 'the engine ABI mirror the module-load check reads',
  'request_keys.d.ts': 'the request-shape tables behind the key refusals',
  'validation.d.ts': 'domain bounds behind the argument refusals',
  'worklet/clip-page-ring.d.ts': 'ring layout; the worklet entry curates what it publishes',
  'worklet/engine-rings.d.ts': 'ring layout; the worklet entry curates what it publishes',
  'worklet/external-midi-ring.d.ts': 'ring layout; the worklet entry curates what it publishes',
  'worklet/meter-rings.d.ts': 'ring layout; the worklet entry curates what it publishes',
  'worklet/protocol.d.ts': 'mixer parameter ids private to the worklet protocol',
  'worklet/ring-core.d.ts': 'ring cursor bounds private to the ring implementation',
  'worklet/scope-ring.d.ts': 'ring layout; the worklet entry curates what it publishes',
  'worklet/stream-analyzer-processor.d.ts': 'processor chunk default private to the worklet',
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
