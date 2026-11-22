/**
 * Every embind class handle the TypeScript sources declare is a class the module
 * registers, with every declared method on its prototype, and every registered
 * class and method is declared.
 *
 * A handle is declared as `interface Wasm<Name>` carrying `delete`, and the module
 * registers it as `<Name>`. A declared method the binary lacks type-checks and then
 * throws "is not a function" on its first call, which only a call would reveal.
 */

import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { describe, expect, it } from 'vitest';

const ROOT = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '');

/** Registered methods no TypeScript caller reaches, so no handle declares them. */
const UNDECLARED: Readonly<Record<string, string>> = {
  'SampleBank#id': 'registered beside the facade, which keeps its own id',
  'ProjectTimeline#id': 'registered beside the facade, which keeps its own id',
  'Project#setMidiFx': 'MIDI FX are set through the realtime engine',
  'PolyphonicAnalysis#noteCount': 'the facade reads the note list whole',
  'PolyphonicAnalysis#frameCount': 'the facade reads the frame list whole',
};

/** Callable member names of every `Wasm<Name>` handle interface under `src/`, keyed by `<Name>`. */
function declaredHandles(root: string): Map<string, string[]> {
  const config = ts.parseJsonConfigFileContent(
    ts.readConfigFile(`${root}/tsconfig.json`, ts.sys.readFile).config,
    ts.sys,
    root,
  );
  const program = ts.createProgram(config.fileNames, { ...config.options, noEmit: true });
  const checker = program.getTypeChecker();
  const handles = new Map<string, string[]>();
  for (const file of program.getSourceFiles()) {
    if (!file.fileName.startsWith(`${root}/src/`)) continue;
    for (const statement of file.statements) {
      if (!ts.isInterfaceDeclaration(statement) || !/^Wasm\w+$/.test(statement.name.text)) {
        continue;
      }
      const symbol = checker.getSymbolAtLocation(statement.name);
      if (!symbol) continue;
      const methods = checker
        .getPropertiesOfType(checker.getDeclaredTypeOfSymbol(symbol))
        .filter(
          (member) =>
            checker.getNonNullableType(checker.getTypeOfSymbol(member)).getCallSignatures().length >
            0,
        )
        .map((member) => member.name);
      if (methods.includes('delete')) handles.set(statement.name.text.slice(4), methods);
    }
  }
  return handles;
}

type EmbindClass = { prototype: Record<string, unknown> };
const isEmbindClass = (value: unknown): value is EmbindClass =>
  typeof value === 'function' && typeof (value as EmbindClass).prototype?.isAliasOf === 'function';

async function registeredClasses(): Promise<Map<string, EmbindClass>> {
  const createModule = (await import('../dist/sonare.js')).default;
  const module = (await createModule()) as unknown as Record<string, unknown>;
  return new Map(
    Object.entries(module).filter((entry): entry is [string, EmbindClass] =>
      isEmbindClass(entry[1]),
    ),
  );
}

describe('embind class handles', async () => {
  const handles = declaredHandles(ROOT);
  const classes = await registeredClasses();
  const undeclaredUsed = new Set<string>();

  it('declares as many handles as the module registers classes', () => {
    expect(handles.size).toBeGreaterThan(10);
    expect([...handles.keys()].sort()).toEqual([...classes.keys()].sort());
  });

  it.each([...handles.keys()].sort())('%s: every declared method is registered', (name) => {
    const prototype = classes.get(name)?.prototype;
    if (!prototype) throw new Error(`${name} is not registered`);
    expect(
      (handles.get(name) ?? []).filter((method) => typeof prototype[method] !== 'function'),
    ).toEqual([]);
  });

  it.each([...classes.keys()].sort())('%s: every registered method is declared', (name) => {
    const declared = new Set(handles.get(name) ?? []);
    const prototype = classes.get(name)?.prototype ?? {};
    const missing = Object.getOwnPropertyNames(prototype)
      .filter((method) => method !== 'constructor' && !declared.has(method))
      .map((method) => `${name}#${method}`)
      .filter((key) => {
        if (!(key in UNDECLARED)) return true;
        undeclaredUsed.add(key);
        return false;
      });
    expect(missing).toEqual([]);
  });

  it('excuses no method a handle now declares', () => {
    expect(Object.keys(UNDECLARED).filter((key) => !undeclaredUsed.has(key))).toEqual([]);
  });
});
