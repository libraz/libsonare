/**
 * Enumerates the public callables of the package entries and marks the ones whose parameters
 * reach a caller-built list, through the package's own TypeScript program.
 *
 * A callable is list-taking when a parameter's type reaches an array, a read-only array or an
 * array-like element list, walking object properties, unions, intersections, type-parameter
 * constraints and defaults to a fixed depth. Excluded by rule, from the type: typed arrays and
 * buffers (real memory, budgeted natively), fixed-length tuples (the length is the type's), and
 * function-typed properties (callbacks).
 */

import path from 'node:path';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';

const PACKAGE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MAX_DEPTH = 4;

/** One public callable: a function, a constructor, or a public method of an exported class. */
export interface CallableInfo {
  /** `functionName`, `Class.method` or `Class.constructor`. */
  name: string;
  /** True when a parameter reaches a caller-built list. */
  listTaking: boolean;
  /** Declaring file and line, relative to the package directory. */
  where: string;
  /** The property paths, rooted at the parameter name, that hold a caller list. */
  listParams: string[];
}

let cached: Map<string, CallableInfo> | undefined;

/** Builds the program once per realm and returns every public callable keyed by name. */
export function enumerateCallables(entries: readonly string[]): Map<string, CallableInfo> {
  const key = entries.join('|');
  if (cached && (cached as Map<string, CallableInfo> & { key?: string }).key === key) {
    return cached;
  }
  const cfgPath = ts.findConfigFile(PACKAGE_DIR, ts.sys.fileExists, 'tsconfig.json');
  if (!cfgPath) {
    throw new Error('tsconfig.json not found');
  }
  const cfg = ts.parseJsonConfigFileContent(
    ts.readConfigFile(cfgPath, ts.sys.readFile).config,
    ts.sys,
    path.dirname(cfgPath),
  );
  const files = entries.map((e) => path.join(PACKAGE_DIR, 'src', e));
  const program = ts.createProgram(files, { ...cfg.options, noEmit: true });
  const checker = program.getTypeChecker();
  const found = new Map<string, CallableInfo>();

  const isBufferLike = (t: ts.Type): boolean => t.getProperty('byteLength') !== undefined;
  const isLibObject = (t: ts.Type): boolean =>
    (t.symbol?.declarations ?? []).some((d) => d.getSourceFile().hasNoDefaultLib);

  /** Appends the property paths under `type` that hold a caller list; `at` names `type` itself. */
  const collectLists = (
    type: ts.Type,
    at: string,
    depth: number,
    visited: Set<number>,
    out: string[],
  ): void => {
    const id = (type as unknown as { id: number }).id;
    if (visited.has(id)) {
      return;
    }
    visited.add(id);
    if (type.isUnion() || type.isIntersection()) {
      for (const member of type.types) {
        collectLists(member, at, depth, visited, out);
      }
      return;
    }
    if (type.flags & ts.TypeFlags.TypeParameter) {
      const param = type as ts.TypeParameter;
      const bound =
        checker.getBaseConstraintOfType(param) ?? checker.getDefaultFromTypeParameter(param);
      if (bound !== undefined && bound !== param) {
        collectLists(bound, at, depth, visited, out);
      }
      return;
    }
    if (!(type.flags & ts.TypeFlags.Object) || isBufferLike(type)) {
      return;
    }
    if (checker.isTupleType(type)) {
      const target = (type as ts.TypeReference).target as ts.TupleType;
      if (target.combinedFlags & (ts.ElementFlags.Rest | ts.ElementFlags.Variadic)) {
        out.push(at);
      } else if (depth < MAX_DEPTH) {
        for (const element of checker.getTypeArguments(type as ts.TypeReference)) {
          collectLists(element, at, depth + 1, visited, out);
        }
      }
      return;
    }
    if (checker.isArrayType(type) || type.getNumberIndexType() !== undefined) {
      out.push(at);
      return;
    }
    if (depth >= MAX_DEPTH || isLibObject(type) || type.getCallSignatures().length > 0) {
      return;
    }
    if (type.symbol && type.symbol.flags & ts.SymbolFlags.Class) {
      return;
    }
    for (const prop of type.getProperties()) {
      const decl = prop.valueDeclaration ?? prop.declarations?.[0];
      if (!decl) {
        continue;
      }
      const propType = checker.getTypeOfSymbolAtLocation(prop, decl);
      if (propType.getCallSignatures().length > 0) {
        continue;
      }
      collectLists(propType, `${at}.${prop.getName()}`, depth + 1, visited, out);
    }
    const stringIndex = type.getStringIndexType();
    if (stringIndex !== undefined) {
      collectLists(stringIndex, `${at}[]`, depth + 1, visited, out);
    }
  };

  const listParamsOf = (signatures: readonly ts.Signature[]): string[] => {
    const out = new Set<string>();
    for (const sig of signatures) {
      for (const param of sig.getParameters()) {
        const decl = param.valueDeclaration ?? param.declarations?.[0];
        if (!decl) {
          continue;
        }
        const paths: string[] = [];
        collectLists(
          checker.getTypeOfSymbolAtLocation(param, decl),
          param.getName(),
          0,
          new Set(),
          paths,
        );
        for (const found of paths) {
          out.add(found);
        }
      }
    }
    return [...out];
  };

  const whereOf = (sym: ts.Symbol): string => {
    const decl = sym.valueDeclaration ?? sym.declarations?.[0];
    if (!decl) {
      return '?';
    }
    const sf = decl.getSourceFile();
    const { line } = sf.getLineAndCharacterOfPosition(decl.getStart());
    return `${path.relative(PACKAGE_DIR, sf.fileName)}:${line + 1}`;
  };

  const record = (name: string, sym: ts.Symbol, signatures: readonly ts.Signature[]): void => {
    if (signatures.length === 0 || found.has(name)) {
      return;
    }
    const listParams = listParamsOf(signatures);
    found.set(name, { name, listTaking: listParams.length > 0, where: whereOf(sym), listParams });
  };

  const isOwnDeclaration = (decl: ts.Node): boolean => {
    const file = decl.getSourceFile();
    return !file.hasNoDefaultLib && !file.fileName.includes('/node_modules/');
  };

  const isPublicMember = (member: ts.Symbol): boolean => {
    const name = member.getName();
    if (name.startsWith('_') || name.startsWith('#') || name.startsWith('__@')) {
      return false;
    }
    const decl = member.valueDeclaration ?? member.declarations?.[0];
    if (!decl || !isOwnDeclaration(decl)) {
      return false;
    }
    const flags = ts.getCombinedModifierFlags(decl as ts.Declaration);
    return !(
      flags & ts.ModifierFlags.Private ||
      flags & ts.ModifierFlags.Protected ||
      (decl as ts.NamedDeclaration).name?.kind === ts.SyntaxKind.PrivateIdentifier
    );
  };

  for (const file of files) {
    const source = program.getSourceFile(file);
    const moduleSym = source && checker.getSymbolAtLocation(source);
    if (!moduleSym) {
      continue;
    }
    for (const exported of checker.getExportsOfModule(moduleSym)) {
      const sym =
        exported.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(exported) : exported;
      const name = exported.getName();
      if (sym.flags & ts.SymbolFlags.Class) {
        const decl = sym.valueDeclaration ?? sym.declarations?.[0];
        if (!decl) {
          continue;
        }
        const classType = checker.getTypeOfSymbolAtLocation(sym, decl);
        record(
          `${name}.constructor`,
          sym,
          classType.getConstructSignatures().filter((sig) => {
            const flags = sig.declaration ? ts.getCombinedModifierFlags(sig.declaration) : 0;
            return !(flags & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected));
          }),
        );
        for (const member of checker.getDeclaredTypeOfSymbol(sym).getProperties()) {
          if (!(member.flags & ts.SymbolFlags.Method) || !isPublicMember(member)) {
            continue;
          }
          const mDecl = member.valueDeclaration ?? member.declarations?.[0];
          if (!mDecl) {
            continue;
          }
          record(
            `${name}.${member.getName()}`,
            member,
            checker.getTypeOfSymbolAtLocation(member, mDecl).getCallSignatures(),
          );
        }
        for (const member of classType.getProperties()) {
          if (!(member.flags & ts.SymbolFlags.Method) || !isPublicMember(member)) {
            continue;
          }
          const mDecl = member.valueDeclaration ?? member.declarations?.[0];
          if (!mDecl) {
            continue;
          }
          record(
            `${name}.${member.getName()}`,
            member,
            checker.getTypeOfSymbolAtLocation(member, mDecl).getCallSignatures(),
          );
        }
      } else if (sym.flags & (ts.SymbolFlags.Function | ts.SymbolFlags.Variable)) {
        const decl = sym.valueDeclaration ?? sym.declarations?.[0];
        if (!decl) {
          continue;
        }
        record(name, sym, checker.getTypeOfSymbolAtLocation(sym, decl).getCallSignatures());
      }
    }
  }
  cached = found;
  (cached as Map<string, CallableInfo> & { key?: string }).key = key;
  return found;
}
