/**
 * Every embind class handle the TypeScript sources declare is a class the module
 * registers, with every declared method and property on its prototype, and every
 * registered class, method and property is declared.
 *
 * A handle is declared as `interface Wasm<Name>` carrying `delete`, and the module
 * registers it as `<Name>`. A declared method the binary lacks type-checks and then
 * throws "is not a function" on its first call, which only a call would reveal.
 */

import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { describe, expect, it } from 'vitest';

const ROOT = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '');

/** Members of a handle interface: callable ones are methods, the rest embind properties. */
type Handle = { methods: string[]; properties: string[] };

/** Every `Wasm<Name>` handle interface under `src/`, keyed by `<Name>`. */
function declaredHandles(root: string): Map<string, Handle> {
  const config = ts.parseJsonConfigFileContent(
    ts.readConfigFile(`${root}/tsconfig.json`, ts.sys.readFile).config,
    ts.sys,
    root,
  );
  const program = ts.createProgram(config.fileNames, { ...config.options, noEmit: true });
  const checker = program.getTypeChecker();
  const handles = new Map<string, Handle>();
  for (const file of program.getSourceFiles()) {
    if (!file.fileName.startsWith(`${root}/src/`)) {
      continue;
    }
    for (const statement of file.statements) {
      if (!ts.isInterfaceDeclaration(statement) || !/^Wasm\w+$/.test(statement.name.text)) {
        continue;
      }
      const symbol = checker.getSymbolAtLocation(statement.name);
      if (!symbol) {
        continue;
      }
      const handle: Handle = { methods: [], properties: [] };
      for (const member of checker.getPropertiesOfType(checker.getDeclaredTypeOfSymbol(symbol))) {
        const type = checker.getNonNullableType(checker.getTypeOfSymbol(member));
        (type.getCallSignatures().length > 0 ? handle.methods : handle.properties).push(
          member.name,
        );
      }
      if (handle.methods.includes('delete')) {
        handles.set(statement.name.text.slice(4), handle);
      }
    }
  }
  return handles;
}

type EmbindClass = { prototype: object };
const isEmbindClass = (value: unknown): value is EmbindClass =>
  typeof value === 'function' &&
  typeof (value as { prototype?: { isAliasOf?: unknown } }).prototype?.isAliasOf === 'function';

/** The descriptor of @p key on @p prototype or the chain above it; reading a getter here would throw. */
function descriptor(prototype: object | null, key: string): PropertyDescriptor | undefined {
  for (let at = prototype; at; at = Object.getPrototypeOf(at)) {
    const found = Object.getOwnPropertyDescriptor(at, key);
    if (found) {
      return found;
    }
  }
  return undefined;
}

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

  it('declares as many handles as the module registers classes', () => {
    expect(handles.size).toBeGreaterThan(10);
    expect([...handles.keys()].sort()).toEqual([...classes.keys()].sort());
  });

  it.each([...handles.keys()].sort())('%s: every declared member is registered', (name) => {
    const prototype = classes.get(name)?.prototype;
    if (!prototype) {
      throw new Error(`${name} is not registered`);
    }
    const { methods, properties } = handles.get(name) ?? { methods: [], properties: [] };
    expect(
      methods.filter((method) => typeof descriptor(prototype, method)?.value !== 'function'),
    ).toEqual([]);
    expect(properties.filter((property) => !descriptor(prototype, property)?.get)).toEqual([]);
  });

  it.each([...classes.keys()].sort())('%s: every registered member is declared', (name) => {
    const handle = handles.get(name);
    const declared = new Set([...(handle?.methods ?? []), ...(handle?.properties ?? [])]);
    const prototype = classes.get(name)?.prototype ?? {};
    const missing = Object.getOwnPropertyNames(prototype)
      .filter((member) => member !== 'constructor' && !declared.has(member))
      .map((member) => `${name}#${member}`);
    expect(missing).toEqual([]);
  });
});
