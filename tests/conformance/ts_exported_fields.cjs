/**
 * Field names and optional markers of a package's exported types, read through
 * the TypeScript checker from the package's own `tsconfig.json` and the export
 * list of `src/index.ts`. Shared by the conformance scripts that hold a corpus
 * against the declarations, so none of them re-implements the walk.
 */
'use strict';

const path = require('node:path');

/**
 * @param {string} dir absolute package directory (holding tsconfig.json and node_modules/typescript)
 * @param {string[]} typeNames exported type names to read
 * @returns {{shapes: Record<string, Record<string, boolean>>, missing: string[]}}
 *   per type, field name -> optional; `missing` lists names the entry does not export
 */
function exportedTypeFields(dir, typeNames) {
  const pkg = path.basename(dir);
  const ts = require(path.join(dir, 'node_modules/typescript'));
  const configPath = path.join(dir, 'tsconfig.json');
  const parsed = ts.getParsedCommandLineOfConfigFile(configPath, {}, {
    ...ts.sys,
    onUnRecoverableConfigFileDiagnostic: () => {},
  });
  const entry = path.join(dir, 'src/index.ts');
  const program = ts.createProgram({ rootNames: [entry], options: { ...parsed.options, noEmit: true } });
  const checker = program.getTypeChecker();
  const sf = program.getSourceFile(entry);
  const moduleSymbol = sf && checker.getSymbolAtLocation(sf);
  if (!moduleSymbol) throw new Error(`${pkg}: cannot read the exports of src/index.ts`);
  const exports = new Map();
  for (const sym of checker.getExportsOfModule(moduleSymbol)) exports.set(sym.name, sym);

  const shapes = {};
  const missing = [];
  for (const name of typeNames) {
    let sym = exports.get(name);
    if (!sym) {
      missing.push(name);
      continue;
    }
    if (sym.flags & ts.SymbolFlags.Alias) sym = checker.getAliasedSymbol(sym);
    const type = checker.getDeclaredTypeOfSymbol(sym);
    const fields = {};
    for (const prop of checker.getPropertiesOfType(type)) {
      fields[prop.name] = (prop.flags & ts.SymbolFlags.Optional) !== 0;
    }
    if (Object.keys(fields).length === 0) throw new Error(`${pkg}: ${name} declares no field`);
    shapes[name] = fields;
  }
  return { shapes, missing };
}

module.exports = { exportedTypeFields };
