/**
 * Every type a public function or class signature names can be named by a consumer.
 *
 * A parameter or return type the package does not export forces a caller to
 * re-derive it (`Parameters<typeof fn>[1]`) or copy its shape, and the docs
 * print a name nobody can import. The check walks each package entry's exports,
 * reads the parameter, return and public-member types of every exported function
 * and class, and requires each type declared under `src/` to be exported from
 * some entry — directly or through an exported alias that renames it. An
 * unexported alias is looked through, since its parts are what a caller spells;
 * members tagged `@internal` are skipped.
 */

import { dirname, join, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { describe, expect, it } from 'vitest';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = join(ROOT, 'src') + sep;
const ENTRIES = ['index.ts', 'analysis.ts', 'worklet.ts', 'worker.ts', 'vocal_edit_worker.ts'];

function unalias(checker: ts.TypeChecker, symbol: ts.Symbol): ts.Symbol {
  return symbol.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(symbol) : symbol;
}

function missingSignatureTypes(): Map<string, Set<string>> {
  const config = ts.readConfigFile(join(ROOT, 'tsconfig.json'), ts.sys.readFile);
  const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, ROOT);
  const entryFiles = ENTRIES.map((entry) => join(SRC, entry));
  const program = ts.createProgram(entryFiles, { ...parsed.options, noEmit: true });
  const checker = program.getTypeChecker();

  const exportsByEntry = entryFiles.map((file) => {
    const source = program.getSourceFile(file);
    const moduleSymbol = source && checker.getSymbolAtLocation(source);
    return moduleSymbol ? checker.getExportsOfModule(moduleSymbol) : [];
  });
  const nameable = new Set<ts.Symbol>();
  for (const symbol of exportsByEntry.flat()) {
    const target = unalias(checker, symbol);
    nameable.add(target);
    // An exported alias that only renames a type makes that type nameable through it.
    for (const declaration of target.declarations ?? []) {
      if (
        ts.isTypeAliasDeclaration(declaration) &&
        ts.isTypeReferenceNode(declaration.type) &&
        !declaration.type.typeArguments
      ) {
        const renamed = checker.getSymbolAtLocation(declaration.type.typeName);
        if (renamed) {
          nameable.add(unalias(checker, renamed));
        }
      }
    }
  }

  const missing = new Map<string, Set<string>>();
  const lookedThrough = new Set<ts.Symbol>();
  const typeFlags =
    ts.SymbolFlags.Interface |
    ts.SymbolFlags.TypeAlias |
    ts.SymbolFlags.Enum |
    ts.SymbolFlags.Class;
  const visitType = (node: ts.Node, owner: string): void => {
    if (ts.isTypeReferenceNode(node) || ts.isExpressionWithTypeArguments(node)) {
      const name = ts.isTypeReferenceNode(node) ? node.typeName : node.expression;
      const referenced = checker.getSymbolAtLocation(name);
      const symbol = referenced && unalias(checker, referenced);
      const declaration = symbol?.declarations?.[0];
      if (
        symbol &&
        declaration &&
        symbol.flags & typeFlags &&
        declaration.getSourceFile().fileName.startsWith(SRC) &&
        !nameable.has(symbol)
      ) {
        if (ts.isTypeAliasDeclaration(declaration)) {
          if (!lookedThrough.has(symbol)) {
            lookedThrough.add(symbol);
            visitType(declaration.type, owner);
          }
        } else {
          const owners = missing.get(symbol.name) ?? new Set<string>();
          owners.add(owner);
          missing.set(symbol.name, owners);
        }
      }
    }
    ts.forEachChild(node, (child) => visitType(child, owner));
  };
  const visitSignature = (declaration: ts.SignatureDeclarationBase, owner: string): void => {
    for (const parameter of declaration.parameters) {
      if (parameter.type) {
        visitType(parameter.type, owner);
      }
    }
    if (declaration.type) {
      visitType(declaration.type, owner);
    }
  };
  const isInternal = (node: ts.Node): boolean =>
    ts.getJSDocTags(node).some((tag) => tag.tagName.text === 'internal');

  for (const exported of exportsByEntry.flat()) {
    const symbol = unalias(checker, exported);
    for (const declaration of symbol.declarations ?? []) {
      if (!declaration.getSourceFile().fileName.startsWith(SRC)) {
        continue;
      }
      if (ts.isFunctionDeclaration(declaration)) {
        visitSignature(declaration, exported.name);
      } else if (ts.isClassDeclaration(declaration)) {
        for (const member of declaration.members) {
          const flags = ts.getCombinedModifierFlags(member);
          if (
            flags & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected) ||
            (member.name && ts.isPrivateIdentifier(member.name)) ||
            isInternal(member)
          ) {
            continue;
          }
          const owner = `${exported.name}.${member.name?.getText() ?? 'constructor'}`;
          if (
            ts.isMethodDeclaration(member) ||
            ts.isConstructorDeclaration(member) ||
            ts.isGetAccessorDeclaration(member) ||
            ts.isSetAccessorDeclaration(member)
          ) {
            visitSignature(member, owner);
          } else if (ts.isPropertyDeclaration(member) && member.type) {
            visitType(member.type, owner);
          }
        }
      }
    }
  }
  return missing;
}

describe('exported signatures', () => {
  it('name only types some package entry exports', () => {
    const missing = missingSignatureTypes();
    const report = [...missing].map(([name, owners]) => `${name} <- ${[...owners].join(', ')}`);
    expect(report).toEqual([]);
  });

  it('exports the analysis types the docs print', () => {
    // Type-level imports from the package root: a missing export fails `tsc -p tsconfig.test.json`.
    type Named = [
      import('../src/index').MeterEstimate,
      import('../src/index').PiptrackResult,
      import('../src/index').ScopeOptions,
      import('../src/index').OnsetDetectOptions,
      import('../src/index').MusicAnalyzeOptions,
    ];
    const named: Named | undefined = undefined;
    expect(named).toBeUndefined();
  });
});
