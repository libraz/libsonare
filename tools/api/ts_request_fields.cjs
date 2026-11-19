// Lists the first-level fields of every object-typed parameter of a package's public entries.
//
// Usage: node tools/api/ts_request_fields.cjs <packageDir>
// Reads <packageDir>/src/index.ts through the package's own TypeScript checker. Output is a JSON
// array on stdout, one object per (entry, parameter): entry (`fn` or `Class.method`, `Class` for a
// constructor), parameter name, and the property names its declared type holds. A union takes
// every arm's properties; a rest tuple (`...args: [request: R]`) takes its elements' types. Arrays
// and standard-library objects hold no fields.
'use strict';
const path = require('path');

const pkgDir = path.resolve(process.argv[2]);
const ts = require(path.join(pkgDir, 'node_modules/typescript'));
const cfgPath = ts.findConfigFile(pkgDir, ts.sys.fileExists, 'tsconfig.json');
const cfg = ts.parseJsonConfigFileContent(
  ts.readConfigFile(cfgPath, ts.sys.readFile).config,
  ts.sys,
  path.dirname(cfgPath),
);
const index = path.join(pkgDir, 'src/index.ts');
const program = ts.createProgram([index], { ...cfg.options, noEmit: true });
const checker = program.getTypeChecker();

const isLibObject = (t) =>
  checker.isArrayType(t) ||
  (t.symbol?.declarations || []).some((d) => {
    const sf = d.getSourceFile();
    return sf.hasNoDefaultLib || program.isSourceFileDefaultLibrary(sf) || /[\\/]node_modules[\\/]/.test(sf.fileName);
  });
const parts = (t) =>
  (t.isUnion() ? t.types : [t]).filter(
    (p) => !(p.flags & (ts.TypeFlags.Undefined | ts.TypeFlags.Null | ts.TypeFlags.Void)),
  );

function fieldsOf(t) {
  const out = new Set();
  for (const p of parts(t)) {
    if (checker.isTupleType(p)) {
      for (const e of checker.getTypeArguments(p)) for (const f of fieldsOf(e)) out.add(f);
      continue;
    }
    if (!(p.flags & ts.TypeFlags.Object) || isLibObject(p)) continue;
    if (p.getCallSignatures().length > 0) continue;
    for (const m of checker.getPropertiesOfType(p)) out.add(m.getName());
  }
  return [...out].sort();
}

const rows = [];
function addSignatures(entry, signatures) {
  for (const g of signatures) {
    for (const p of g.getParameters()) {
      const pd = (p.declarations || [])[0];
      const fields = fieldsOf(checker.getTypeOfSymbolAtLocation(p, pd || g.getDeclaration()));
      if (fields.length > 0) rows.push({ entry, param: p.getName(), fields });
    }
  }
}

const moduleSymbol = checker.getSymbolAtLocation(program.getSourceFile(index));
for (const exp of checker.getExportsOfModule(moduleSymbol)) {
  const sym = exp.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(exp) : exp;
  const decl = (sym.declarations || [])[0];
  if (!decl) continue;
  const name = exp.getName();
  if (sym.flags & ts.SymbolFlags.Function) {
    addSignatures(name, checker.getTypeOfSymbolAtLocation(sym, decl).getCallSignatures());
  } else if (sym.flags & ts.SymbolFlags.Class) {
    const ctor = checker.getTypeOfSymbolAtLocation(sym, decl);
    addSignatures(name, ctor.getConstructSignatures());
    for (const m of checker.getPropertiesOfType(ctor)) {
      if (m.getName() === 'prototype') continue;
      addSignatures(`${name}.${m.getName()}`, checker.getTypeOfSymbolAtLocation(m, decl).getCallSignatures());
    }
    const instance = checker.getDeclaredTypeOfSymbol(sym);
    for (const m of checker.getPropertiesOfType(instance)) {
      const md = (m.declarations || [])[0];
      if (!md || ts.getCombinedModifierFlags(md) & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)) continue;
      addSignatures(`${name}.${m.getName()}`, checker.getTypeOfSymbolAtLocation(m, md).getCallSignatures());
    }
  }
}
process.stdout.write(`${JSON.stringify(rows)}\n`);
