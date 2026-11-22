// Dumps every export of a TypeScript package's entry points through the type checker.
//
// Usage: node tools/api/ts_exports.cjs <packageDir> <entry.ts>...
// Entries are resolved under <packageDir>/src. The package must have typescript in its
// own node_modules. Output is a JSON array on stdout, one object per (entry, export):
// name, kind, declaring file and line, doc presence, structured call signatures (params
// with name / optional / type text / boolean / string-literal members, return text),
// class and interface members (well-known symbols spelled `[Symbol.dispose]`) with the members
// of any inline object type under a property (`nested`, recursively), enum and
// `as const` object members, the string-literal members of exported type aliases, and for a type
// tagged `@mirrors <Interface>` the platform interface and the members it shares with it.
// Paths are relative to the repository root (two levels above the package directory).
'use strict';
const path = require('path');

const [pkgArg, ...entries] = process.argv.slice(2);
const pkgDir = path.resolve(pkgArg);
const ts = require(path.join(pkgDir, 'node_modules/typescript'));
const cfgPath = ts.findConfigFile(pkgDir, ts.sys.fileExists, 'tsconfig.json');
const cfg = ts.parseJsonConfigFileContent(
  ts.readConfigFile(cfgPath, ts.sys.readFile).config,
  ts.sys,
  path.dirname(cfgPath),
);
const files = entries.map((e) => path.join(pkgDir, 'src', e));
const program = ts.createProgram(files, { ...cfg.options, noEmit: true });
const checker = program.getTypeChecker();
const root = path.resolve(pkgDir, '../..');
const TF = ts.TypeFormatFlags.NoTruncation;
const MAX_TEXT = 4000;

const clip = (s) => (s.length > MAX_TEXT ? s.slice(0, MAX_TEXT) : s);
const where = (d) => {
  const sf = d.getSourceFile();
  const { line } = sf.getLineAndCharacterOfPosition(d.getStart());
  return { file: path.relative(root, sf.fileName), line: line + 1 };
};
const declHasDoc = (d) => !!(d && d.jsDoc && d.jsDoc.length > 0);
const symHasDoc = (s) =>
  s.getDocumentationComment(checker).length > 0 || (s.declarations || []).some(declHasDoc);
// The doc comment with its tags (`@param`, `@returns`), as one string.
const symDocText = (s) =>
  clip(
    [
      ts.displayPartsToString(s.getDocumentationComment(checker)),
      ...s.getJsDocTags(checker).map((g) => ts.displayPartsToString(g.text || [])),
    ]
      .filter(Boolean)
      .join('\n'),
  );
const typeText = (t) => clip(checker.typeToString(t, undefined, TF));

// An array, tuple or standard-library object (Uint8Array, Map ...): its members are not the API's.
const isLibObject = (t) =>
  checker.isArrayType(t) ||
  checker.isTupleType(t) ||
  (t.symbol?.declarations || []).some((d) => d.getSourceFile().hasNoDefaultLib);

// Members of a union after dropping undefined / null.
const definedParts = (t) =>
  (t.isUnion() ? t.types : [t]).filter(
    (p) => !(p.flags & (ts.TypeFlags.Undefined | ts.TypeFlags.Null | ts.TypeFlags.Void)),
  );
const isBoolean = (t) => {
  const parts = definedParts(t);
  return parts.length > 0 && parts.every((p) => p.flags & (ts.TypeFlags.BooleanLike));
};
const stringLiterals = (t) => {
  const parts = definedParts(t);
  return parts.filter((p) => p.isStringLiteral()).map((p) => p.value).sort();
};

const memberName = (m, md) => {
  if (md && md.name && ts.isComputedPropertyName(md.name)) return md.name.getText();
  return m.getName();
};

const describeSignature = (g) => {
  const params = g.getParameters().map((p) => {
    const pd = (p.declarations || [])[0];
    const pt = checker.getTypeOfSymbolAtLocation(p, pd || g.getDeclaration());
    const optional = !!(
      pd &&
      ts.isParameter(pd) &&
      (pd.questionToken || pd.initializer || pd.dotDotDotToken)
    );
    return {
      name: p.getName(),
      optional,
      rest: !!(pd && ts.isParameter(pd) && pd.dotDotDotToken),
      type: typeText(pt),
      boolean: isBoolean(pt),
      literals: stringLiterals(pt),
    };
  });
  return {
    text: clip(checker.signatureToString(g, undefined, TF)),
    params,
    returns: typeText(checker.getReturnTypeOfSignature(g)),
  };
};

const isHidden = (md) =>
  !!(ts.getCombinedModifierFlags(md) & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)) ||
  (md.name && ts.isPrivateIdentifier && ts.isPrivateIdentifier(md.name));

// An object type spelled inline (`loudness: { ceilingDb: number }`), not a named interface.
const isInlineObject = (t) =>
  !!(t.flags & ts.TypeFlags.Object) &&
  !isLibObject(t) &&
  (t.symbol?.declarations || []).some((d) => ts.isTypeLiteralNode(d));

// The inline object types a property's type is made of: itself, a union arm, or an array element.
const inlineParts = (t) =>
  definedParts(t).flatMap((p) =>
    isInlineObject(p)
      ? [p]
      : checker.isArrayType(p)
        ? checker.getTypeArguments(p).filter(isInlineObject)
        : [],
  );

// Members of the inline object types under a property, recursively; a type met twice on one
// path stops the walk.
const nestedMembers = (t, seen) => {
  const out = [];
  for (const part of inlineParts(t)) {
    if (seen.has(part)) continue;
    const path = new Set(seen).add(part);
    for (const m of checker.getPropertiesOfType(part)) {
      const r = describeMember(m, false, path);
      if (r) out.push(r);
    }
  }
  return out;
};

const describeMember = (m, isStatic, seen = new Set()) => {
  const md = (m.declarations || [])[0];
  if (!md || isHidden(md)) return null;
  const name = memberName(m, md);
  if (name.startsWith('#')) return null;
  const mt = checker.getTypeOfSymbolAtLocation(m, md);
  // An optional method (`f?(...)`) is typed `F | undefined`, which has no call signatures itself.
  const cs = checker.getNonNullableType(mt).getCallSignatures();
  let kind;
  if (m.flags & ts.SymbolFlags.Method) kind = 'method';
  else if (m.flags & ts.SymbolFlags.Accessor) kind = 'accessor';
  else kind = 'property';
  return {
    name,
    kind,
    static: isStatic,
    optional: !!(m.flags & ts.SymbolFlags.Optional),
    readonly: !!(ts.getCombinedModifierFlags(md) & ts.ModifierFlags.Readonly),
    ...where(md),
    doc: symHasDoc(m),
    docText: symDocText(m),
    sigs: kind === 'method' ? cs.map(describeSignature) : [],
    type: typeText(mt),
    boolean: kind !== 'method' && isBoolean(mt),
    literals: kind !== 'method' ? stringLiterals(mt) : [],
    nested: kind === 'property' ? nestedMembers(mt, seen) : [],
  };
};

// `@mirrors <Interface>` on a type declares it as the part of a platform interface the library
// reads; the members the platform interface also declares are its mirrored members.
const platformMirror = (s, d) => {
  const tag = s.getJsDocTags(checker).find((g) => g.name === 'mirrors');
  if (!tag) return null;
  const name = ts.displayPartsToString(tag.text || []).trim();
  const target = name && checker.resolveName(name, d, ts.SymbolFlags.Type, false);
  if (!target) {
    process.stderr.write(`${s.getName()}: @mirrors names no type in scope: ${name || '(empty)'}\n`);
    process.exit(2);
  }
  const declared = new Set(
    checker.getPropertiesOfType(checker.getDeclaredTypeOfSymbol(target)).map((m) => m.getName()),
  );
  const own = checker.getPropertiesOfType(checker.getDeclaredTypeOfSymbol(s)).map((m) => m.getName());
  return { interface: name, members: own.filter((m) => declared.has(m)).sort() };
};

const kindOf = (s) => {
  const f = s.flags;
  if (f & ts.SymbolFlags.Class) return 'class';
  if (f & ts.SymbolFlags.Function) return 'function';
  if (f & ts.SymbolFlags.Interface) return 'interface';
  if (f & ts.SymbolFlags.TypeAlias) return 'type';
  if (f & ts.SymbolFlags.Enum) return 'enum';
  if (f & ts.SymbolFlags.Variable) return 'const';
  if (f & ts.SymbolFlags.Module) return 'namespace';
  return 'other';
};

const out = [];
for (let i = 0; i < files.length; i++) {
  const sf = program.getSourceFile(files[i]);
  if (!sf) {
    process.stderr.write(`missing entry: ${files[i]}\n`);
    process.exit(2);
  }
  const mod = checker.getSymbolAtLocation(sf);
  for (let s of checker.getExportsOfModule(mod)) {
    const exportName = s.getName();
    if (s.flags & ts.SymbolFlags.Alias) s = checker.getAliasedSymbol(s);
    const d = (s.declarations || [])[0];
    if (!d) continue;
    const kind = kindOf(s);
    const row = {
      entry: entries[i],
      name: exportName,
      kind,
      ...where(d),
      doc: symHasDoc(s),
      docText: symDocText(s),
      sigs: [],
      members: [],
      enumMembers: [],
      literals: [],
      type: '',
      ctorPrivate: false,
      platformMirror: platformMirror(s, d),
    };
    if (kind === 'function') {
      const t = checker.getTypeOfSymbolAtLocation(s, d);
      row.sigs = t.getCallSignatures().map(describeSignature);
    } else if (kind === 'const') {
      const t = checker.getTypeOfSymbolAtLocation(s, d);
      row.type = typeText(t);
      row.sigs = t.getCallSignatures().map(describeSignature);
    } else if (kind === 'type') {
      const t = checker.getDeclaredTypeOfSymbol(s);
      row.type = clip(d.getText().replace(/\s+/g, ' '));
      row.literals = stringLiterals(t);
      // An object union (a discriminated union, or `Union & { ... }` distributed over it)
      // contributes every property any arm declares.
      const objectFlags = ts.TypeFlags.Object | ts.TypeFlags.Intersection;
      const props =
        t.flags & objectFlags
          ? checker.getPropertiesOfType(t)
          : t.isUnion() && t.types.every((p) => p.flags & objectFlags && !isLibObject(p))
            ? checker.getAllPossiblePropertiesOfTypes(t.types)
            : [];
      for (const m of props) {
        const r = describeMember(m, false);
        if (r) row.members.push(r);
      }
    } else if (kind === 'enum') {
      row.enumMembers = (d.members || []).map((m) => ({
        name: m.name.getText(),
        ...where(m),
        doc: declHasDoc(m),
      }));
    }
    // A `{ ... } as const` object is an enum in all but declaration form.
    const constObject = (s.declarations || []).find(
      (x) =>
        ts.isVariableDeclaration(x) &&
        x.initializer &&
        ts.isAsExpression(x.initializer) &&
        x.initializer.type.getText() === 'const' &&
        ts.isObjectLiteralExpression(x.initializer.expression),
    );
    if (constObject) {
      row.enumMembers = constObject.initializer.expression.properties
        .filter(ts.isPropertyAssignment)
        .map((m) => ({
          name: m.name.getText().replace(/^['"]|['"]$/g, ''),
          ...where(m),
          doc: declHasDoc(m),
        }));
    }
    if (kind === 'class' || kind === 'interface') {
      const inst = checker.getDeclaredTypeOfSymbol(s);
      for (const m of checker.getPropertiesOfType(inst)) {
        const r = describeMember(m, false);
        if (r) row.members.push(r);
      }
      if (kind === 'class') {
        const st = checker.getTypeOfSymbolAtLocation(s, d);
        for (const m of checker.getPropertiesOfType(st)) {
          if (m.getName() === 'prototype') continue;
          const r = describeMember(m, true);
          if (r) row.members.push(r);
        }
        row.sigs = st.getConstructSignatures().map(describeSignature);
        const cd = (s.declarations || []).find(ts.isClassDeclaration);
        const ctor = cd && cd.members.find(ts.isConstructorDeclaration);
        row.ctorPrivate = !!(
          ctor &&
          ts.getCombinedModifierFlags(ctor) & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)
        );
        row.bases = cd && cd.heritageClauses
          ? cd.heritageClauses.flatMap((h) => h.types.map((x) => x.expression.getText()))
          : [];
      }
    }
    out.push(row);
  }
}
process.stdout.write(JSON.stringify(out));
