import { existsSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as ts from 'typescript';
import { describe, expect, it } from 'vitest';

const nodeRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function formatDiagnostic(diagnostic: ts.Diagnostic): string {
  const message = ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n');
  if (!diagnostic.file || diagnostic.start === undefined) {
    return message;
  }
  const position = diagnostic.file.getLineAndCharacterOfPosition(diagnostic.start);
  return `${diagnostic.file.fileName}:${position.line + 1}:${position.character + 1}: ${message}`;
}

function diagnosticErrors(diagnostics: readonly ts.Diagnostic[]): string[] {
  return diagnostics
    .filter((diagnostic) => diagnostic.category === ts.DiagnosticCategory.Error)
    .map(formatDiagnostic);
}

function emitNodeDeclarations(outputDirectory: string): string {
  const configPath = ts.findConfigFile(nodeRoot, ts.sys.fileExists, 'tsconfig.json');
  if (!configPath) {
    throw new Error(`Could not find the Node tsconfig below ${nodeRoot}`);
  }

  const config = ts.readConfigFile(configPath, ts.sys.readFile);
  if (config.error) {
    throw new Error(formatDiagnostic(config.error));
  }

  const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, nodeRoot, {
    declaration: true,
    declarationMap: false,
    emitDeclarationOnly: true,
    noEmit: false,
    outDir: outputDirectory,
    sourceMap: false,
  });
  const program = ts.createProgram(parsed.fileNames, {
    ...parsed.options,
    declaration: true,
    declarationMap: false,
    emitDeclarationOnly: true,
    noEmit: false,
    outDir: outputDirectory,
    sourceMap: false,
  });
  const diagnostics = [
    ...ts.getPreEmitDiagnostics(program),
    ...program.emit().diagnostics,
    ...parsed.errors,
  ];
  const errors = diagnosticErrors(diagnostics);
  if (errors.length > 0) {
    throw new Error(`Node declaration emit failed:\n${errors.join('\n')}`);
  }

  const entryPath = join(outputDirectory, 'index.d.ts');
  if (!existsSync(entryPath)) {
    throw new Error(`Node declaration emit did not produce ${entryPath}`);
  }
  return entryPath;
}

function fullyUnaliasedSymbol(checker: ts.TypeChecker, symbol: ts.Symbol): ts.Symbol {
  const visited = new Set<ts.Symbol>();
  let current = symbol;
  while ((current.flags & ts.SymbolFlags.Alias) !== 0 && !visited.has(current)) {
    visited.add(current);
    const target = checker.getAliasedSymbol(current);
    if (target === current) {
      break;
    }
    current = target;
  }
  return current;
}

function publicSymbol(symbol: ts.Symbol): boolean {
  const declarations = symbol.declarations ?? [];
  return (
    declarations.length === 0 ||
    declarations.some((declaration) => {
      const modifiers = ts.getCombinedModifierFlags(declaration);
      return (modifiers & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)) === 0;
    })
  );
}

function publicDeclaration(declaration: ts.Declaration): boolean {
  const modifiers = ts.getCombinedModifierFlags(declaration);
  return (modifiers & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)) === 0;
}

function checkPublicTypeExports(entryPath: string, generatedRoot: string): string[] {
  const program = ts.createProgram([entryPath], {
    module: ts.ModuleKind.NodeNext,
    moduleResolution: ts.ModuleResolutionKind.NodeNext,
    noEmit: true,
    skipLibCheck: true,
    strict: true,
    target: ts.ScriptTarget.ES2022,
    types: ['node'],
  });
  const errors = diagnosticErrors(ts.getPreEmitDiagnostics(program));
  if (errors.length > 0) {
    return errors.map((error) => `${entryPath}: ${error}`);
  }

  const sourceFile = program.getSourceFile(entryPath);
  if (!sourceFile) {
    return [`Could not load generated declaration entrypoint ${entryPath}`];
  }
  const checker = program.getTypeChecker();
  const entrySymbol = checker.getSymbolAtLocation(sourceFile);
  if (!entrySymbol) {
    return [`Could not resolve generated declaration entrypoint ${entryPath}`];
  }
  const entryLocation = sourceFile;

  const root = resolve(generatedRoot);
  const rootPrefix = root.endsWith(sep) ? root : `${root}${sep}`;
  const generatedFile = (fileName: string): boolean => {
    const absolute = resolve(fileName);
    return absolute === root || absolute.startsWith(rootPrefix);
  };
  const typeDeclarationFlags =
    ts.SymbolFlags.Interface |
    ts.SymbolFlags.TypeAlias |
    ts.SymbolFlags.Class |
    ts.SymbolFlags.Enum;
  const localTypeSymbol = (symbol: ts.Symbol | undefined): ts.Symbol | undefined => {
    if (!symbol) {
      return undefined;
    }
    const target = fullyUnaliasedSymbol(checker, symbol);
    if (!target.name || target.name.startsWith('__')) {
      return undefined;
    }
    if ((target.flags & typeDeclarationFlags) === 0) {
      return undefined;
    }
    if (
      !(target.declarations ?? []).some((declaration) =>
        generatedFile(declaration.getSourceFile().fileName),
      )
    ) {
      return undefined;
    }
    return target;
  };

  const entryExports = checker.getExportsOfModule(entrySymbol);
  const entryExportByName = new Map<string, ts.Symbol>();
  for (const exported of entryExports) {
    entryExportByName.set(exported.name, fullyUnaliasedSymbol(checker, exported));
  }

  const missing: string[] = [];
  const mismatched: string[] = [];
  const seenSymbols = new Set<ts.Symbol>();
  const seenTypes = new Set<ts.Type>();

  function sourceNames(symbol: ts.Symbol): string {
    return (symbol.declarations ?? [])
      .map((declaration) => relative(root, declaration.getSourceFile().fileName))
      .join(', ');
  }

  function requireEntryExport(symbol: ts.Symbol, breadcrumb: string): void {
    const expected = entryExportByName.get(symbol.name);
    if (!expected) {
      missing.push(
        `${breadcrumb}: ${symbol.name} is declared in ${sourceNames(symbol)} but is not exported by ${relative(root, entryPath)}`,
      );
      return;
    }
    if (expected !== symbol) {
      mismatched.push(
        `${breadcrumb}: ${symbol.name} resolves to ${sourceNames(symbol)}, but the entry export resolves to ${sourceNames(expected)}`,
      );
    }
  }

  function typeArguments(type: ts.Type): readonly ts.Type[] {
    if ((type.flags & ts.TypeFlags.Object) === 0) {
      return [];
    }
    try {
      return checker.getTypeArguments(type as ts.TypeReference);
    } catch {
      return [];
    }
  }

  let visitType: (type: ts.Type | undefined, breadcrumb: string) => void;

  function visitTypeParameter(typeParameter: ts.TypeParameter, breadcrumb: string): void {
    visitType(checker.getBaseConstraintOfType(typeParameter), `${breadcrumb} constraint`);
    visitType(checker.getDefaultFromTypeParameter(typeParameter), `${breadcrumb} default`);
  }

  function visitSymbolType(symbol: ts.Symbol, breadcrumb: string, location: ts.Node): void {
    const declaration = symbol.valueDeclaration ?? symbol.declarations?.[0] ?? location;
    visitType(checker.getTypeOfSymbolAtLocation(symbol, declaration), breadcrumb);
  }

  function visitSignature(signature: ts.Signature, breadcrumb: string): void {
    for (const typeParameter of signature.typeParameters ?? []) {
      visitTypeParameter(typeParameter, `${breadcrumb}<${typeParameter.symbol?.name ?? 'T'}>`);
    }
    if (signature.thisParameter) {
      visitSymbolType(signature.thisParameter, `${breadcrumb} this`, entryLocation);
    }
    for (const parameter of signature.parameters) {
      visitSymbolType(parameter, `${breadcrumb} parameter ${parameter.name}`, entryLocation);
    }
    visitType(checker.getTypePredicateOfSignature(signature)?.type, `${breadcrumb} type predicate`);
    visitType(signature.getReturnType(), `${breadcrumb} return`);
  }

  function visitMembers(type: ts.Type, breadcrumb: string): void {
    for (const member of type.getProperties?.() ?? []) {
      if (publicSymbol(member)) {
        visitSymbolType(member, `${breadcrumb}.${member.name}`, entryLocation);
      }
    }
    for (const signature of type.getCallSignatures?.() ?? []) {
      visitSignature(signature, `${breadcrumb} call`);
    }
    for (const signature of type.getConstructSignatures?.() ?? []) {
      visitSignature(signature, `${breadcrumb} construct`);
    }
    visitType(type.getStringIndexType?.(), `${breadcrumb} string index`);
    visitType(type.getNumberIndexType?.(), `${breadcrumb} number index`);
  }

  function visitTypeBody(type: ts.Type, breadcrumb: string): void {
    for (const typeParameter of (type as ts.InterfaceType).typeParameters ?? []) {
      visitTypeParameter(typeParameter, `${breadcrumb}<${typeParameter.symbol?.name ?? 'T'}>`);
    }
    for (const argument of type.aliasTypeArguments ?? []) {
      visitType(argument, `${breadcrumb} type argument`);
    }
    if (type.isUnionOrIntersection?.()) {
      for (const member of type.types) {
        visitType(member, `${breadcrumb} union/intersection member`);
      }
      return;
    }
    if (type.isTypeParameter?.()) {
      visitTypeParameter(type, breadcrumb);
      return;
    }
    if ((type.flags & ts.TypeFlags.IndexedAccess) !== 0) {
      const indexed = type as ts.IndexedAccessType;
      visitType(indexed.objectType, `${breadcrumb} indexed object`);
      visitType(indexed.indexType, `${breadcrumb} indexed key`);
      visitType(indexed.constraint, `${breadcrumb} indexed constraint`);
      return;
    }
    if ((type.flags & ts.TypeFlags.Conditional) !== 0) {
      const conditional = type as ts.ConditionalType;
      visitType(conditional.root.checkType, `${breadcrumb} conditional check`);
      visitType(conditional.root.extendsType, `${breadcrumb} conditional extends`);
      visitType(conditional.resolvedTrueType, `${breadcrumb} conditional true`);
      visitType(conditional.resolvedFalseType, `${breadcrumb} conditional false`);
      return;
    }
    if ((type.flags & ts.TypeFlags.Substitution) !== 0) {
      const substitution = type as ts.SubstitutionType;
      visitType(substitution.baseType, `${breadcrumb} substitution base`);
      visitType(substitution.constraint, `${breadcrumb} substitution constraint`);
      return;
    }
    if ((type.flags & ts.TypeFlags.Index) !== 0) {
      visitType((type as ts.IndexType).type, `${breadcrumb} index`);
      return;
    }
    if ((type.flags & ts.TypeFlags.TemplateLiteral) !== 0) {
      for (const member of (type as ts.TemplateLiteralType).types) {
        visitType(member, `${breadcrumb} template member`);
      }
      return;
    }
    if ((type.flags & ts.TypeFlags.Object) === 0) {
      return;
    }

    for (const argument of typeArguments(type)) {
      visitType(argument, `${breadcrumb} type argument`);
    }
    for (const base of type.getBaseTypes?.() ?? []) {
      visitType(base, `${breadcrumb} base`);
    }

    const symbol = type.getSymbol?.();
    if (symbol && !generatedFile(symbol.declarations?.[0]?.getSourceFile().fileName ?? '')) {
      return;
    }
    visitMembers(type, breadcrumb);
  }

  function visitNamedType(symbol: ts.Symbol, breadcrumb: string): void {
    const target = fullyUnaliasedSymbol(checker, symbol);
    if (seenSymbols.has(target)) {
      return;
    }
    seenSymbols.add(target);
    requireEntryExport(target, breadcrumb);
    visitTypeBody(checker.getDeclaredTypeOfSymbol(target), `${breadcrumb} ${target.name}`);
    if ((target.flags & ts.SymbolFlags.Class) !== 0) {
      visitClassMembers(target, `${breadcrumb} ${target.name}`);
    }
  }

  function visitTypeImplementation(type: ts.Type | undefined, breadcrumb: string): void {
    if (!type || seenTypes.has(type)) {
      return;
    }
    seenTypes.add(type);

    const named = localTypeSymbol(type.aliasSymbol) ?? localTypeSymbol(type.getSymbol?.());
    if (named) {
      visitNamedType(named, breadcrumb);
      for (const argument of type.aliasTypeArguments ?? []) {
        visitType(argument, `${breadcrumb} type argument`);
      }
      for (const argument of typeArguments(type)) {
        visitType(argument, `${breadcrumb} type argument`);
      }
      return;
    }
    visitTypeBody(type, breadcrumb);
  }

  visitType = visitTypeImplementation;

  function visitClassMembers(symbol: ts.Symbol, breadcrumb: string): void {
    const declaration = symbol.valueDeclaration ?? symbol.declarations?.[0] ?? entryLocation;
    const constructorType = checker.getTypeOfSymbolAtLocation(symbol, declaration);
    for (const signature of constructorType.getConstructSignatures()) {
      const signatureDeclaration = signature.declaration;
      if (
        signatureDeclaration &&
        ts.isConstructorDeclaration(signatureDeclaration) &&
        !publicDeclaration(signatureDeclaration)
      ) {
        continue;
      }
      visitSignature(signature, `${breadcrumb} constructor`);
    }
    for (const member of symbol.exports?.values() ?? []) {
      if (member.name !== 'prototype' && publicSymbol(member)) {
        visitSymbolType(member, `${breadcrumb} static ${member.name}`, entryLocation);
      }
    }
  }

  function visitClassRoot(symbol: ts.Symbol, breadcrumb: string): void {
    visitNamedType(symbol, breadcrumb);
  }

  for (const exported of entryExports) {
    const symbol = fullyUnaliasedSymbol(checker, exported);
    const declaration = symbol.valueDeclaration ?? symbol.declarations?.[0] ?? entryLocation;
    const type = checker.getTypeOfSymbolAtLocation(symbol, declaration);
    if ((symbol.flags & ts.SymbolFlags.Class) !== 0) {
      visitClassRoot(symbol, `entry ${exported.name}`);
    } else if (
      (symbol.flags & ts.SymbolFlags.Function) !== 0 ||
      type.getCallSignatures().length > 0
    ) {
      for (const signature of type.getCallSignatures()) {
        visitSignature(signature, `entry ${exported.name}`);
      }
    }
  }

  return [...missing, ...mismatched];
}

function writeSyntheticFixture(root: string): string {
  const fixturePath = join(root, 'type-export-fixture.d.ts');
  const entryPath = join(root, 'type-export-fixture-index.d.ts');
  writeFileSync(
    fixturePath,
    `export interface FixtureNested {
  nativeValue: Promise<Buffer>;
}

export type FixtureBox<T> = {
  items: Array<T>;
  tuple: [T, { nested: T }];
  union: T | undefined;
  intersection: T & { extra: string };
};

interface FixturePrivate {
  secret: string;
}

interface FixtureUnused {
  unused: FixturePrivate;
}

export class FixtureClass {
  private hidden: FixturePrivate;
  visible: FixtureBox<FixtureNested>;
  private hiddenMethod(): FixturePrivate;
  static publicFactory(value: FixtureBox<FixtureNested>): FixtureBox<FixtureNested>;
}

export declare function fixture<T>(value: FixtureBox<T>): Promise<T>;
`,
    'utf8',
  );
  writeFileSync(
    entryPath,
    `export { FixtureClass, fixture } from './type-export-fixture.js';
export type { FixtureBox, FixtureNested } from './type-export-fixture.js';
`,
    'utf8',
  );
  return entryPath;
}

describe('Node entrypoint type exports', () => {
  it('covers types reachable from emitted public callable and class declarations', () => {
    const outputDirectory = mkdtempSync(join(tmpdir(), 'libsonare-node-type-exports-'));
    try {
      const entryPath = emitNodeDeclarations(outputDirectory);
      expect(checkPublicTypeExports(entryPath, outputDirectory)).toEqual([]);

      const fixtureEntryPath = writeSyntheticFixture(outputDirectory);
      expect(checkPublicTypeExports(fixtureEntryPath, outputDirectory)).toEqual([]);
    } finally {
      rmSync(outputDirectory, { force: true, recursive: true });
    }
  });
});
