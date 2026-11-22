/**
 * The declaration tree in `dist/` resolves without `src/` beside it.
 *
 * A host that vendors `dist/` alone has nothing at `../src/`, and under
 * `skipLibCheck` an unresolved specifier degrades every type behind it to
 * `any` without an error, so an escaping specifier is refused here instead.
 */

import { readFileSync } from 'node:fs';
import { readdir, readFile } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
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
 * Modules whose declared consts reach an entry's declarations only through a type
 * import, each with the reason none of them is a public value of any entry.
 */
const NOT_PUBLIC_VALUES: Readonly<Record<string, string>> = {
  'internal/engine_command_ring.d.ts': 'engine command ring layout, private to the facade',
  'internal/worklet_messages.d.ts': 'worklet message defaults, private to the facade',
  'project_types.d.ts': 'the project ABI mirror the module-load check reads',
  'realtime_engine.d.ts': 'the engine ABI mirror the module-load check reads',
  'request_keys.d.ts': 'the request-shape tables behind the key refusals',
  'validation.d.ts': 'domain bounds behind the argument refusals',
  'worklet/engine-rings.d.ts': 'ring layout; the worklet entry curates what it publishes',
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

/** Each typed entry of the package's exports map: its declaration and runtime module under dist/. */
const ENTRIES: readonly { declaration: string; runtime: string }[] = Object.values(
  JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8')).exports as Record<
    string,
    unknown
  >,
)
  .filter(
    (target): target is { types: string; import: string } =>
      typeof target === 'object' && target !== null && 'types' in target && 'import' in target,
  )
  .map((target) => ({
    declaration: target.types.replace(/^\.\/dist\//, ''),
    runtime: target.import.replace(/^\.\/dist\//, ''),
  }));

const runtimeKeys = async (runtime: string): Promise<string[]> =>
  Object.keys(await import(/* @vite-ignore */ join(DIST, runtime)));

describe('a const an entry declares', () => {
  it('is a runtime export of some entry unless its module is listed as not public', async () => {
    expect(ENTRIES.length).toBeGreaterThan(1);
    const runtime = new Set((await Promise.all(ENTRIES.map((e) => runtimeKeys(e.runtime)))).flat());
    const unexported = new Set<string>();
    const excusing = new Set<string>();
    for (const entry of ENTRIES) {
      const closure = await declarationClosure(entry.declaration);
      expect(closure.size).toBeGreaterThan(1);
      for (const [file, text] of closure) {
        for (const [, name] of text.matchAll(/^export declare const (\w+)/gm)) {
          if (runtime.has(name)) continue;
          if (file in NOT_PUBLIC_VALUES) excusing.add(file);
          else unexported.add(`${file}: ${name}`);
        }
      }
    }
    expect([...unexported]).toEqual([]);
    expect(Object.keys(NOT_PUBLIC_VALUES).filter((file) => !excusing.has(file))).toEqual([]);
  });
});

/**
 * The names a consumer can read as values from @p declaration: each export the
 * checker resolves to a value, kept when a probe module reading it off a namespace
 * import type-checks, since a name re-exported with `export type *` resolves to a
 * value and still cannot be read as one.
 */
function valueExports(declaration: string): string[] {
  const options: ts.CompilerOptions = {
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    noEmit: true,
    skipLibCheck: true,
    strict: true,
    target: ts.ScriptTarget.ES2022,
    types: [],
  };
  const file = join(DIST, declaration);
  const program = ts.createProgram([file], options);
  const checker = program.getTypeChecker();
  const source = program.getSourceFile(file);
  const moduleSymbol = source && checker.getSymbolAtLocation(source);
  if (!moduleSymbol) throw new Error(`${declaration} did not resolve`);
  const candidates = checker
    .getExportsOfModule(moduleSymbol)
    .filter((symbol) => {
      const target =
        symbol.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(symbol) : symbol;
      return (target.flags & ts.SymbolFlags.Value) !== 0;
    })
    .map((symbol) => symbol.name);
  const probe = join(DIST, '__value_probe.ts');
  const specifier = `./${declaration.replace(/\.d\.ts$/, '.js')}`;
  const text = [`import * as ns from '${specifier}';`, ...candidates.map((n) => `void ns.${n};`)];
  const host = ts.createCompilerHost(options);
  const getSourceFile = host.getSourceFile.bind(host);
  const fileExists = host.fileExists.bind(host);
  host.getSourceFile = (name, version, ...rest) =>
    name === probe
      ? ts.createSourceFile(name, `${text.join('\n')}\n`, version)
      : getSourceFile(name, version, ...rest);
  host.fileExists = (name) => name === probe || fileExists(name);
  const probing = ts.createProgram([probe], options, host);
  const probeSource = probing.getSourceFile(probe) as ts.SourceFile;
  const unreadable = new Set(
    ts
      .getPreEmitDiagnostics(probing, probeSource)
      .filter((d) => d.file === probeSource && d.start !== undefined)
      .map(
        (d) => candidates[probeSource.getLineAndCharacterOfPosition(d.start as number).line - 1],
      ),
  );
  return candidates.filter((name) => !unreadable.has(name));
}

describe.each(ENTRIES)('the $declaration entry', ({ declaration, runtime }) => {
  it('declares exactly the values its runtime module exports', async () => {
    const declared = valueExports(declaration).sort();
    expect(declared.length).toBeGreaterThan(0);
    expect(declared).toEqual((await runtimeKeys(runtime)).sort());
  });
});
